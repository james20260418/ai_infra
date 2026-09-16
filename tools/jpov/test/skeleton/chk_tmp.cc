#include <glog/logging.h>
#include <cmath>
#include "geom/common/quaternion.h"
#include "geom/math/mat4.h"
#include "tools/jpov/src/gltf_loader.h"
int main(){
  const char* glb="/james_pm/ai_infra_2/tools/jpov/test/object3d/mixamo_male/mixamo_male.glb";
  std::vector<jpov::SkeletonType> sk; CHECK(jpov::LoadGltfSkeleton(glb,&sk));
  jpov::MeshData mesh; jpov::GltfMaterialInfo mi; CHECK(jpov::LoadGltf(glb,&mesh,&mi));
  // For vertices weighted mostly to a specific bone (e.g. Head, LeftFoot, Hips), print
  // that vertex's mesh-space position. Skeleton says Head is at y=0.83 (gltf frame).
  const auto& t=sk[0];
  // find bone indices
  auto find=[&](const char* s){for(int j=0;j<t.bone_count();++j) if(t.joints[j].name.find(s)!=std::string::npos) return j; return -1;};
  int head=find("Head"), lfoot=find("LeftFoot"), lhand=find("LeftHand");
  LOG(INFO)<<"head="<<head<<" lfoot="<<lfoot<<" lhand="<<lhand;
  // average mesh position of vertices whose dominant joint is that bone
  for(int b : {head,lfoot}){
    if(b<0) continue;
    double sx=0,sy=0,sz=0; int n=0;
    for(size_t i=0;i<mesh.positions.size();++i){
      int best=0; float bw=-1;
      for(int k=0;k<4;++k){ if(mesh.joint_weights[i][k]>bw){bw=mesh.joint_weights[i][k];best=mesh.joint_indices[i][k];} }
      if(best==b){sx+=mesh.positions[i].x();sy+=mesh.positions[i].y();sz+=mesh.positions[i].z();++n;}
    }
    LOG(INFO)<<"bone "<<t.joints[b].name<<" nverts="<<n
             <<" avg mesh pos=("<<sx/n<<","<<sy/n<<","<<sz/n<<")";
  }
  LOG(INFO)<<"--- reminder: mesh was written as (gx,-gz,gy) from gltf (gx,gy,gz) ---";
}
