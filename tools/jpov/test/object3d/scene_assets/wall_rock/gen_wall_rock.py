#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成石头墙板 glTF (wall_rock.gltf + wall_rock.bin)。
- baseColor = rock_x4.png  (cube_tex 中间石头 panel 镜像扩张 4 倍, 512x512 seamless)
- normal    = rock_normal.png
- metallic/roughness 不写在 glTF 里：由 scene test 手动 override PBRMaterial
  （MR 同 cube test 的两张贴图, 用户要求"不带AO/emissive", 用显式填最可控）。

尺寸: 4 宽 x 3 高 x 0.2 厚 (竖直墙板, 与 wall.gltf 同, 面向 +Z)。
"""
import struct, json

W,H,T = 4.0, 3.0, 0.2
x0,y0,z0 = -W/2, 0.0, -T/2
x1,y1,z1 =  W/2, H,     T/2
V = [(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0),
     (x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]
N = [(0,0,-1)]*4 + [(0,0,1)]*4
UV = [(0,0),(1,0),(1,1),(0,1)]*2
IND = [4,5,6,4,6,7, 0,3,2,0,2,1, 3,0,4,3,4,7, 1,2,6,1,6,5, 2,3,7,2,7,6, 0,1,5,0,5,4]

pos_b=struct.pack('<%df'%(len(V)*3),*[c for v in V for c in v])
nrm_b=struct.pack('<%df'%(len(N)*3),*[c for n in N for c in n])
uv_b =struct.pack('<%df'%(len(UV)*2),*[c for u in UV for c in u])
idx_b=struct.pack('<%dH'%len(IND),*IND)
buf=pos_b+nrm_b+uv_b+idx_b
offs=[];cur=0
for blk,al in ((pos_b,4),(nrm_b,4),(uv_b,4),(idx_b,2)):
    if cur%al: cur+=al-(cur%al)
    offs.append(cur);cur+=len(blk)
bvs=[{'buffer':0,'byteOffset':offs[0],'byteLength':len(pos_b),'target':'ARRAY_BUFFER'},
     {'buffer':0,'byteOffset':offs[1],'byteLength':len(nrm_b),'target':'ARRAY_BUFFER'},
     {'buffer':0,'byteOffset':offs[2],'byteLength':len(uv_b),'target':'ARRAY_BUFFER'},
     {'buffer':0,'byteOffset':offs[3],'byteLength':len(idx_b),'target':'ELEMENT_ARRAY_BUFFER'}]
VC=len(V)
gltf={
 "asset":{"version":"2.0","generator":"jpov-wall-rock"},
 "scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
 "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],
 "materials":[{"name":"wall_rock",
   "pbrMetallicRoughness":{"baseColorTexture":{"index":1},"baseColorFactor":[1,1,1,1]},
   "normalTexture":{"index":0,"scale":1.0}}],
 "buffers":[{"byteLength":len(buf),"uri":"wall_rock.bin"}],
 "bufferViews":bvs,
 "accessors":[
   {"bufferView":0,"componentType":5126,"count":VC,"type":"VEC3","min":[x0,y0,z0],"max":[x1,y1,z1]},
   {"bufferView":1,"componentType":5126,"count":VC,"type":"VEC3"},
   {"bufferView":2,"componentType":5126,"count":VC,"type":"VEC2"},
   {"bufferView":3,"componentType":5123,"count":len(IND),"type":"SCALAR"}],
 "images":[{"uri":"rock_normal.png","mimeType":"image/png"},
           {"uri":"rock_x4.png","mimeType":"image/png"}],
 "textures":[{"source":0,"sampler":0},{"source":1,"sampler":0}],
 "samplers":[{"magFilter":9729,"minFilter":9987,"wrapS":10497,"wrapT":10497}],
}
json.dump(gltf,open('wall_rock.gltf','w'),indent=2)
open('wall_rock.bin','wb').write(buf)
print("生成 wall_rock.gltf(4x3x0.2) + wall_rock.bin, baseColor=rock_x4 + normal")
