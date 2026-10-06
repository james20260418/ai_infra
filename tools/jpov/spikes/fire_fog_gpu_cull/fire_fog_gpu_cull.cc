// JPOV Fire-Fog tile culling — GPU 小 FBO 建表 vs CPU SAT/AABB 基准（临时 spike，验证后删）
//
// 目的：量测「GPU 小 FBO 建表」（设计文档 jpov_volumetric_fog_design.md §13.5 后路）
// 相对 CPU 侧 culling 的**性能**，并做**正确性**对照（拿单个点雾的已知例子）。
//
// GPU 算法（K 趟 GL_MIN）：
//   1. 开一张与 tile 网格同尺寸的小 FBO（1 像素 = 1 tile，R32F）。
//   2. 每个团画一个 instanced quad = 它在小 FBO 上的屏幕 AABB；fragment 输出该团 index。
//   3. GL_MIN 混合 ⇒ 一趟得到「每 tile 最小 index」。
//   4. K 槽需 K 趟：第 n 趟只保留「> 上趟阈值」的最小 index（阈值 = 上趟结果纹理）。
// 说明：quad = AABB ⇒ GPU 覆盖是 AABB（矩形）；CPU 另有 SAT（凸包/六边形，更紧）。
//       故正确性对照 GPU ↔ CPU-AABB（应逐格一致）；CPU-SAT 是更紧的另一档。
//
// 运行：bazel run //tools/jpov/spikes/fire_fog_gpu_cull:fire_fog_gpu_cull
#define GL_GLEXT_PROTOTYPES

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <glog/logging.h>
#include <unistd.h>

namespace {

constexpr float kBig = 1.0e30f;

// ---------- 精简 headless GL 引导（复用 pbr_tbn_probe 的模式） ----------
GLFWwindow* BootGL(int w, int h) {
    if (!glfwInit()) {
        int pid = fork();
        if (pid == 0) {
            execlp("Xvfb", "Xvfb", ":99", "-ac", "-screen", "0", "1280x720x24",
                   "-noreset", "+extension", "GLX", "+iglx", nullptr);
            _exit(1);
        }
        setenv("DISPLAY", ":99", 1);
        sleep(1);
        CHECK(glfwInit()) << "glfwInit failed after Xvfb";
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win = glfwCreateWindow(w, h, "fire_fog_gpu_cull", nullptr, nullptr);
    CHECK(win != nullptr) << "glfwCreateWindow failed";
    glfwMakeContextCurrent(win);
    return win;
}

unsigned int Compile(GLenum type, const char* src) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG(FATAL) << "shader compile failed: " << log;
    }
    return s;
}
unsigned int MakeProgram(const char* vs, const char* fs) {
    unsigned int p = glCreateProgram();
    glAttachShader(p, Compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, Compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    int ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOG(FATAL) << "link failed: " << log;
    }
    return p;
}

// ---------- 极简向量 ----------
struct V3 { float x, y, z; };
struct V2 { float x, y; };
struct Body { V3 c; float r; };

// ---------- 相机 → 列主序 mvp[16] ----------
void BuildMvp(float camx, float camy, float camz, float fovy_deg, int W, int H,
              float mvp[16]) {
    auto nz = [](V3 v) { float n = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); return V3{v.x/n,v.y/n,v.z/n}; };
    V3 cam{camx,camy,camz}, tgt{0,0,0}, up{0,1,0};
    V3 fwd = nz(V3{tgt.x-cam.x,tgt.y-cam.y,tgt.z-cam.z});
    V3 right = nz(V3{fwd.y*up.z-fwd.z*up.y, fwd.z*up.x-fwd.x*up.z, fwd.x*up.y-fwd.y*up.x});
    V3 u2{right.y*fwd.z-right.z*fwd.y, right.z*fwd.x-right.x*fwd.z, right.x*fwd.y-right.y*fwd.x};
    float view[16] = {right.x,u2.x,-fwd.x,0, right.y,u2.y,-fwd.y,0, right.z,u2.z,-fwd.z,0,
                      -(right.x*cam.x+right.y*cam.y+right.z*cam.z),
                      -(u2.x*cam.x+u2.y*cam.y+u2.z*cam.z),
                       (fwd.x*cam.x+fwd.y*cam.y+fwd.z*cam.z), 1};
    float t = 1.0f/std::tan(fovy_deg*3.14159265358979f/360.0f), asp=(float)W/H;
    float n=0.05f, f=1000.0f;
    float proj[16] = {t/asp,0,0,0, 0,t,0,0, 0,0,(f+n)/(n-f),-1, 0,0,2*f*n/(n-f),0};
    for (int c=0;c<4;++c) for (int r=0;r<4;++r){ float s=0; for(int k=0;k<4;++k) s+=proj[k*4+r]*view[c*4+k]; mvp[c*4+r]=s; }
}

bool ProjectPt(const float m[16], const V3& p, int W, int H, float* ox, float* oy) {
    const float w = m[3]*p.x + m[7]*p.y + m[11]*p.z + m[15];
    if (w <= 0.0f) return false;
    const float inv = 1.0f/w;
    *ox = ((m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12])*inv*0.5f+0.5f)*W;
    *oy = ((m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13])*inv*0.5f+0.5f)*H;
    return true;
}

// 团的屏幕 AABB（px）。返回：0=OK(有 AABB)，1=跨相机(部分角在前，保守全屏)，2=全在相机后(跳过)。
int FogAabb(const Body& b, const float m[16], int W, int H,
            float* x0, float* y0, float* x1, float* y1) {
    float mnx=1e30f,mxx=-1e30f,mny=1e30f,mxy=-1e30f; int nfront=0;
    for (int s=0;s<8;++s){
        float sx=(s&1)?b.r:-b.r, sy=(s&2)?b.r:-b.r, sz=(s&4)?b.r:-b.r;
        float px,py; if(!ProjectPt(m, V3{b.c.x+sx,b.c.y+sy,b.c.z+sz}, W,H,&px,&py)) continue;
        ++nfront; mnx=std::min(mnx,px);mxx=std::max(mxx,px);mny=std::min(mny,py);mxy=std::max(mxy,py);
    }
    if (nfront==0) return 2;              // 全部在相机后 → 不贡献
    if (nfront<8)  return 1;              // 跨相机 → 保守全屏
    *x0=mnx-1; *x1=mxx+1; *y0=mny-1; *y1=mxy+1; return 0;
}

// ---------- 凸包 + SAT（同生产实现） ----------
std::vector<V2> Hull2D(std::vector<V2> p){
    std::sort(p.begin(),p.end(),[](const V2&a,const V2&b){return a.x!=b.x?a.x<b.x:a.y<b.y;});
    p.erase(std::unique(p.begin(),p.end(),[](const V2&a,const V2&b){return a.x==b.x&&a.y==b.y;}),p.end());
    if(p.size()<3) return p;
    auto cr=[](const V2&o,const V2&a,const V2&b){return (a.x-o.x)*(b.y-o.y)-(a.y-o.y)*(b.x-o.x);};
    std::vector<V2> h;
    for(auto&q:p){while(h.size()>=2&&cr(h[h.size()-2],h.back(),q)<=0)h.pop_back();h.push_back(q);}
    size_t lo=h.size()+1;
    for(size_t i=p.size()-1;i-->0;){while(h.size()>=lo&&cr(h[h.size()-2],h.back(),p[i])<=0)h.pop_back();h.push_back(p[i]);}
    h.pop_back(); return h;
}
bool RectHull(float x0,float y0,float x1,float y1,const std::vector<V2>& h){
    auto sep=[&](float ax,float ay)->bool{
        float r0=ax*x0+ay*y0,r1=ax*x1+ay*y0,r2=ax*x0+ay*y1,r3=ax*x1+ay*y1;
        float rmin=std::min(std::min(r0,r1),std::min(r2,r3)),rmax=std::max(std::max(r0,r1),std::max(r2,r3));
        float hmin=1e30f,hmax=-1e30f; for(auto&v:h){float q=ax*v.x+ay*v.y;hmin=std::min(hmin,q);hmax=std::max(hmax,q);}
        return (rmax<hmin)||(hmax<rmin);
    };
    if(sep(1,0))return false; if(sep(0,1))return false;
    for(size_t i=0;i<h.size();++i){const V2&a=h[i];const V2&b=h[(i+1)%h.size()];float ex=b.x-a.x,ey=b.y-a.y;if(sep(-ey,ex))return false;}
    return true;
}

// ---------- CPU culling ----------
struct CullResult { std::vector<int> table; long long cand=0, marks=0; int culled=0, behind=0; };

// mode: 0=AABB, 1=SAT. coarse: 视锥粗剔。
CullResult CullCpu(const std::vector<Body>& bodies, const float m[16], int W, int H,
                   int Wpix, int Hpix, int tile, int K, int mode, bool coarse,
                   const float frustum[6][4]) {
    int gw=(W+tile-1)/tile, gh=(H+tile-1)/tile;
    CullResult r; r.table.assign((size_t)gw*gh*K, -1);
    std::vector<int> counts((size_t)gw*gh, 0);
    std::vector<V2> pts; pts.reserve(8);
    (void)Wpix; (void)Hpix;
    for (int bi=0; bi<(int)bodies.size(); ++bi) {
        const Body& b = bodies[bi];
        if (coarse) {
            bool out=false;
            for(int p=0;p<6;++p){ const float* pl=frustum[p];
                float d = pl[0]*b.c.x+pl[1]*b.c.y+pl[2]*b.c.z+pl[3];
                float nl = std::sqrt(pl[0]*pl[0]+pl[1]*pl[1]+pl[2]*pl[2]);
                if (d < -b.r*nl) { out=true; break; }
            }
            if(out){ ++r.culled; continue; }
        }
        float x0,y0,x1,y1;
        int kind=FogAabb(b,m,W,H,&x0,&y0,&x1,&y1);
        if(kind==2){ ++r.behind; continue; }   // 全在相机后 → 跳过（不是全屏！）
        int mnx,mxx,mny,mxy;
        if(kind==1){mnx=0;mxx=W;mny=0;mxy=H;}
        else{mnx=(int)std::floor(x0);mxx=(int)std::ceil(x1);mny=(int)std::floor(y0);mxy=(int)std::ceil(y1);}
        int min_tc=std::max(0,mnx/tile),max_tc=std::min(gw-1,mxx/tile);
        int min_tr=std::max(0,mny/tile),max_tr=std::min(gh-1,mxy/tile);
        if(min_tc>max_tc||min_tr>max_tr) continue;
        std::vector<V2> hull; bool use_sat=(mode==1);
        if(use_sat){
            pts.clear();
            for(int s=0;s<8;++s){ float sx=(s&1)?b.r:-b.r,sy=(s&2)?b.r:-b.r,sz=(s&4)?b.r:-b.r;
                float px,py; if(ProjectPt(m,V3{b.c.x+sx,b.c.y+sy,b.c.z+sz},W,H,&px,&py)) pts.push_back({px,py}); }
            hull=Hull2D(pts); if(hull.size()<3) use_sat=false;
        }
        for(int tr=min_tr;tr<=max_tr;++tr)for(int tc=min_tc;tc<=max_tc;++tc){
            ++r.cand;
            if(use_sat){ if(!RectHull((float)tc*tile,(float)tr*tile,(float)(tc+1)*tile,(float)(tr+1)*tile,hull)) continue; }
            int t=tr*gw+tc; if(counts[t]>=K) continue;
            r.table[(size_t)t*K+counts[t]]=bi; ++counts[t]; ++r.marks;
        }
    }
    return r;
}

// ---------- GPU culling（K 趟 GL_MIN） ----------
const char* kVs = R"glsl(
#version 330 core
layout(location=0) in vec2 aCorner;
layout(location=1) in vec2 aCenter;   // NDC
layout(location=2) in vec2 aHalf;     // NDC
layout(location=3) in float aIndex;   // 1-based
flat out float vIndex;
void main(){ vIndex=aIndex; gl_Position=vec4(aCenter+aCorner*aHalf,0.0,1.0); }
)glsl";
const char* kFs = R"glsl(
#version 330 core
flat in float vIndex;
out float fragIndex;
uniform sampler2D uPrev;
uniform int uFirst;
void main(){
    if (uFirst==1){ fragIndex=vIndex; return; }
    float prev=texelFetch(uPrev, ivec2(gl_FragCoord.xy), 0).r;
    fragIndex = (vIndex > prev) ? vIndex : 1.0e30;
}
)glsl";

struct GpuSetup {
    unsigned int prog=0, vao=0, quad_vbo=0, inst_vbo=0;
    unsigned int texA=0, texB=0, fbo=0;
    int gw=0, gh=0, cap=0;
};
GpuSetup MakeGpu(int gw,int gh){
    GpuSetup g; g.gw=gw; g.gh=gh;
    g.prog=MakeProgram(kVs,kFs);
    const float quad[12]={-1,-1, 1,-1, 1,1, -1,-1, 1,1, -1,1};
    glGenBuffers(1,&g.quad_vbo); glGenVertexArrays(1,&g.vao);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER,g.quad_vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,(void*)0);
    glGenBuffers(1,&g.inst_vbo); glBindBuffer(GL_ARRAY_BUFFER,g.inst_vbo);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,20,(void*)0);  glVertexAttribDivisor(1,1);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,20,(void*)8);  glVertexAttribDivisor(2,1);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3,1,GL_FLOAT,GL_FALSE,20,(void*)16); glVertexAttribDivisor(3,1);
    glBindVertexArray(0);
    auto mktex=[&](){ unsigned int t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R32F,gw,gh,0,GL_RED,GL_FLOAT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        return t; };
    g.texA=mktex(); g.texB=mktex();
    glGenFramebuffers(1,&g.fbo);
    return g;
}

// 每团小 FBO 上的 NDC quad（center, half）+ 1-based index。
std::vector<float> BuildInstances(const std::vector<Body>& bodies,const float m[16],int W,int H){
    std::vector<float> inst; inst.reserve(bodies.size()*5);
    for(size_t i=0;i<bodies.size();++i){
        float x0,y0,x1,y1;
        int kind=FogAabb(bodies[i],m,W,H,&x0,&y0,&x1,&y1);
        if(kind==2) continue;   // 全在相机后 → 不发实例
        float cx,cy,hx,hy;
        if(kind==1){ cx=0;cy=0;hx=1;hy=1; }
        else { cx=(x0+x1)/(float)W - 1.0f; cy=(y0+y1)/(float)H - 1.0f; hx=(x1-x0)/(float)W; hy=(y1-y0)/(float)H; }
        inst.push_back(cx); inst.push_back(cy); inst.push_back(hx); inst.push_back(hy);
        inst.push_back((float)(i+1));
    }
    return inst;
}

struct GpuResult { std::vector<int> table; double ms=0; };

GpuResult CullGpu(GpuSetup& g, const std::vector<float>& inst, int K, bool readback) {
    const int N=(int)(inst.size()/5);
    glBindBuffer(GL_ARRAY_BUFFER,g.inst_vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(inst.size()*sizeof(float)),inst.data(),GL_STREAM_DRAW);

    GpuResult r; r.table.assign((size_t)g.gw*g.gh*K,-1);
    std::vector<float> buf((size_t)g.gw*g.gh);
    glBindFramebuffer(GL_FRAMEBUFFER,g.fbo);
    glViewport(0,0,g.gw,g.gh);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND); glBlendEquation(GL_MIN); glBlendFunc(GL_ONE,GL_ONE);
    glUseProgram(g.prog);
    int uPrev=glGetUniformLocation(g.prog,"uPrev");
    int uFirst=glGetUniformLocation(g.prog,"uFirst");
    glUniform1i(uPrev,0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(g.vao);

    unsigned int src=g.texA, dst=g.texB;
    auto t0=std::chrono::high_resolution_clock::now();
    for(int n=0;n<K;++n){
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
        CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
        glClearColor(kBig,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glUniform1i(uFirst, n==0?1:0);
        glBindTexture(GL_TEXTURE_2D,src);
        glDrawArraysInstanced(GL_TRIANGLES,0,6,N);
        if(readback){
            glReadPixels(0,0,g.gw,g.gh,GL_RED,GL_FLOAT,buf.data());
            for(int i=0;i<g.gw*g.gh;++i){ float v=buf[i]; r.table[(size_t)i*K+n] = (v>=kBig*0.5f)?-1:(int)(v+0.5f)-1; }
        }
        std::swap(src,dst);
    }
    glFinish();
    auto t1=std::chrono::high_resolution_clock::now();
    r.ms=std::chrono::duration<double,std::milli>(t1-t0).count();
    glDisable(GL_BLEND); glBindVertexArray(0); glUseProgram(0);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    int N = 10000;
    for (int i=1;i<argc;++i) { if(!strcmp(argv[i],"--n")&&i+1<argc) N=atoi(argv[i+1]); }
    GLFWwindow* win = BootGL(1280,720);
    LOG(INFO) << "GL " << (const char*)glGetString(GL_VERSION);

    const int W=1280,H=720,tile=16,K=16;
    const float camx=6.755f, camy=2.955f, camz=6.755f;  // 与 bench 同机位（R=10,phi=0.3? 改用 R=10）
    float m[16]; BuildMvp(camx,camy,camz,60.0f,W,H,m);

    // 视锥 6 平面（列主序 mvp 的行）
    auto row=[&](int i,int j){ return m[j*4+i]; };
    float frustum[6][4];
    for(int k=0;k<4;++k){ frustum[0][k]=row(3,k)+row(0,k); frustum[1][k]=row(3,k)-row(0,k);
                          frustum[2][k]=row(3,k)+row(1,k); frustum[3][k]=row(3,k)-row(1,k);
                          frustum[4][k]=row(3,k)+row(2,k); frustum[5][k]=row(3,k)-row(2,k); }

    // 10000 团（r=2，spread 在 [-30,30]²）
    std::mt19937 rng(123);
    auto gen=[&](float span,float radius){ std::uniform_real_distribution<float> U(-span,span);
        std::vector<Body> v; v.reserve(N); for(int i=0;i<N;++i) v.push_back({ {U(rng),1.0f,U(rng)}, radius }); return v; };

    printf("\nviewport %dx%d tile=%d K=%d  N=%d\n",W,H,tile,K,N);
    printf("%-46s | %9s | %8s | %8s\n","scenario / method","ms","cand(M)","culled");
    printf("-------------------------------------------------------------------------------\n");

    auto do_cpu=[&](const char* name,const std::vector<Body>& b,int mode,bool coarse)->CullResult{
        CullResult best; double bms=1e9;
        for(int rep=0;rep<4;++rep){
            auto t0=std::chrono::high_resolution_clock::now();
            CullResult r=CullCpu(b,m,W,H,W,H,tile,K,mode,coarse,frustum);
            auto t1=std::chrono::high_resolution_clock::now();
            double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
            if(ms<bms){bms=ms;best=std::move(r);}
        }
        printf("%-46s | %9.2f | %8.2f | %8d\n",name,bms,best.cand/1e6,best.culled);
        return best;
    };

    // 主场景：10000 个 2m 团 spread 30
    auto bodies = gen(30.0f,2.0f);
    CullResult cpu_aabb = do_cpu("CPU AABB  tile16 (no coarse)",bodies,0,false);
    CullResult cpu_aabb_c = do_cpu("CPU AABB  tile16 (+coarse)",bodies,0,true);
    CullResult cpu_sat  = do_cpu("CPU SAT   tile16 (no coarse)",bodies,1,false);
    CullResult cpu_sat_c= do_cpu("CPU SAT   tile16 (+coarse)",bodies,1,true);

    // GPU
    int gw=(W+tile-1)/tile, gh=(H+tile-1)/tile;
    GpuSetup g=MakeGpu(gw,gh);
    std::vector<float> inst=BuildInstances(bodies,m,W,H);
    // 无 readback 计时（纯 K 趟 GPU）
    double best_gpu=1e9;
    for(int rep=0;rep<4;++rep){ GpuResult r=CullGpu(g,inst,K,false); best_gpu=std::min(best_gpu,r.ms); }
    printf("%-46s | %9.2f | %8s | %8s\n","GPU tinyFBO K-pass GL_MIN (no readback)",best_gpu,"-","-");
    // readback 计时（含 K 次回读）
    GpuResult gr = CullGpu(g,inst,K,true);
    printf("%-46s | %9.2f | %8s | %8s\n","GPU tinyFBO K-pass GL_MIN (+readback)",gr.ms,"-","-");

    // 正确性：GPU vs CPU-AABB（应逐格同集合）；CPU-SAT 不同（更紧的六边形）
    auto cmp=[&](const std::vector<int>& A,const std::vector<int>& B)->int{
        int diff=0; for(size_t i=0;i<A.size();++i){ if(A[i]!=B[i]) ++diff; } return diff; };
    printf("\n[correctness] GPU vs CPU-AABB  slot-diff = %d / %zu\n", cmp(gr.table,cpu_aabb.table), gr.table.size());
    printf("[note] CPU-SAT 与 GPU 覆盖不同（凸包六边形 vs AABB 矩形）：slot-diff = %d\n",
           cmp(cpu_sat.table,cpu_aabb.table));

    // 单团正确性（Danis 指的例子：屏幕中心 2m 点雾）
    // GPU 光栅规则 = tile 中心落在 quad（= 团的屏幕 AABB）内 ⇒ 用它做 ground truth。
    {
        std::vector<Body> one = { { {0.0f,1.0f,0.0f}, 2.0f } };
        float x0,y0,x1,y1;
        int kind=FogAabb(one[0],m,W,H,&x0,&y0,&x1,&y1);
        std::vector<int> gt((size_t)gw*gh*K,-1);
        int gt_tiles=0;
        for(int tr=0;tr<gh;++tr)for(int tc=0;tc<gw;++tc){
            float cx=tc*tile+tile*0.5f, cy2=tr*tile+tile*0.5f;   // FBO 像素中心 → 屏幕 px
            bool in = (kind!=0) || (cx>=x0&&cx<=x1&&cy2>=y0&&cy2<=y1);
            if(in){ gt[(size_t)(tr*gw+tc)*K+0]=0; ++gt_tiles; }
        }
        std::vector<float> i1 = BuildInstances(one,m,W,H);
        GpuResult g1 = CullGpu(g,i1,K,true);
        int diff=0; for(size_t i=0;i<gt.size();++i) if((gt[i]>=0)!=(g1.table[i]>=0)) ++diff;
        int gpu_tiles=0; for(size_t i=0;i<g1.table.size();i+=(size_t)K) if(g1.table[i]>=0)++gpu_tiles;
        printf("[single-fog @center] ground-truth(AABB,center-rule) tiles=%d  GPU tiles=%d  mismatch=%d  %s\n",
               gt_tiles,gpu_tiles,diff, diff==0?"OK":"MISMATCH");
    }
    printf("\n");
    return 0;
}
