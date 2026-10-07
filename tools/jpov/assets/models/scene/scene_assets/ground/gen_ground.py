#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
程序化生成地面平面 glTF (ground_dirt.gltf / ground_brick.gltf)。

用途: 场景 gold test 里的两种地面 (土地 / 砖铺)。
特点:
  - 一块大平面 (thin box, 高中心 y, 面向 +Z), 带 tileable 纹理 + 法线贴图
  - baseColor = 对应 tileable png, normal = 对应法线 png
  - metallic/roughness 用常值 (全局 MR, 不用纹理) —— 土地: metal=0,rough=0.9; 砖: metal=0,rough=0.7
  - 尺寸: SIZE x SIZE (默认 30x30), 位于 y=0 平面 (glTF Y-up, loader 换到 Z-up)
  - 外部 .bin + 引用贴图路径 (测 loader 外部纹理路径)

贴图引用: 相对于本 gltf 所在目录的 png 路径。
"""
import struct, json, os, sys

def emit(gltf_name, bin_name, tex_base, tex_nrm, size, metallic, rough, color):
    # 地面平面: 要让它在 JPOV 中成为水平地面 (span JPOV X/Z, 薄在 JPOV Y)。
    # loader 变换 JPOV=(gx, -gz, gy): JPOV.X=glTF.X, JPOV.Y=-glTF.Z, JPOV.Z=glTF.Y
    # → glTF 平面需 span glTF X 和 glTF Y, 薄在 glTF Z (即 glTF 中是竖直 XY 平面)。
    W = size      # JPOV X 方向 (glTF X)
    H = size      # JPOV Z 方向 (glTF Y)
    T = 0.1       # 厚: JPOV Y (glTF Z)
    # glTF 坐标: 平面在 XY, 薄 Z
    x0, y0, z0 = -W/2, -H/2, -T/2
    x1, y1, z1 =  W/2,  H/2,  T/2
    V = [(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0),
         (x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]
    N = [(0,0,-1)]*4 + [(0,0,1)]*4
    UV = [(0,0),(1,0),(1,1),(0,1)]*2
    IND = [4,5,6,4,6,7, 0,3,2,0,2,1, 3,0,4,3,4,7, 1,2,6,1,6,5, 2,3,7,2,7,6, 0,1,5,0,5,4]
    pos_b = struct.pack('<%df'%(len(V)*3), *[c for v in V for c in v])
    nrm_b = struct.pack('<%df'%(len(N)*3), *[c for n in N for c in n])
    uv_b  = struct.pack('<%df'%(len(UV)*2), *[c for u in UV for c in u])
    idx_b = struct.pack('<%dH'%len(IND), *IND)
    buf = b''
    bvs = []
    base = 0
    def add(blk, target, align):
        nonlocal base
        if base % align: base += align - (base % align)
        bvs.append({'buffer':0,'byteOffset':base,'byteLength':len(blk),'target':target})
        base += len(blk); return len(bvs)-1
    b_p = add(pos_b,'ARRAY_BUFFER',4); b_n=add(nrm_b,'ARRAY_BUFFER',4)
    b_u = add(uv_b,'ARRAY_BUFFER',4);  b_i=add(idx_b,'ELEMENT_ARRAY_BUFFER',2)
    buf = pos_b + nrm_b + uv_b + idx_b  # 顺序即 offset; 简化: 无对齐问题(各4/2,总对齐OK)
    # 上面 add 计算的 offset 和实际拼接要一致: 全部 4 对齐, 无 pad, 顺序连接即可
    offset_map = []
    cur=0
    for blk,target,al in ((pos_b,'ARRAY_BUFFER',4),(nrm_b,'ARRAY_BUFFER',4),(uv_b,'ARRAY_BUFFER',4),(idx_b,'ELEMENT_ARRAY_BUFFER',2)):
        if cur%al: cur+=al-(cur%al)
        offset_map.append(cur); cur+=len(blk)
    bvs=[{'buffer':0,'byteOffset':offset_map[0],'byteLength':len(pos_b),'target':'ARRAY_BUFFER'},
         {'buffer':0,'byteOffset':offset_map[1],'byteLength':len(nrm_b),'target':'ARRAY_BUFFER'},
         {'buffer':0,'byteOffset':offset_map[2],'byteLength':len(uv_b),'target':'ARRAY_BUFFER'},
         {'buffer':0,'byteOffset':offset_map[3],'byteLength':len(idx_b),'target':'ELEMENT_ARRAY_BUFFER'}]
    buf = pos_b+nrm_b+uv_b+idx_b
    VC=len(V)
    gltf = {
      "asset":{"version":"2.0","generator":"jpov-ground-proc"},
      "scene":0,"scenes":[{"nodes":[0]}],
      "nodes":[{"mesh":0}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],
      "materials":[{"name":"ground",
        "pbrMetallicRoughness":{"baseColorTexture":{"index":1},
          "baseColorFactor":list(color),"metallicFactor":metallic,"roughnessFactor":rough},
        "normalTexture":{"index":0,"scale":1.0}}],
      "buffers":[{"byteLength":len(buf),"uri":bin_name}],
      "bufferViews":bvs,
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":VC,"type":"VEC3","min":[x0,y0,z0],"max":[x1,y1,z1]},
        {"bufferView":1,"componentType":5126,"count":VC,"type":"VEC3"},
        {"bufferView":2,"componentType":5126,"count":VC,"type":"VEC2"},
        {"bufferView":3,"componentType":5123,"count":len(IND),"type":"SCALAR"}],
      "images":[{"uri":tex_nrm,"mimeType":"image/png"},{"uri":tex_base,"mimeType":"image/png"}],
      "textures":[{"source":0,"sampler":0},{"source":1,"sampler":0}],
      "samplers":[{"magFilter":9729,"minFilter":9987,"wrapS":10497,"wrapT":10497}],
    }
    json.dump(gltf, open(gltf_name,'w'), indent=2)
    open(bin_name,'wb').write(buf)
    print(f"生成 {gltf_name} ({W}x{H}, metal={metallic}, rough={rough})")

# 土地地面
emit("ground_dirt.gltf","ground_dirt.bin",
     "../ground_dirt/dirt_diff_tileable.png","../ground_dirt/dirt_normal.png",
     6, 0.0, 0.9, [1,1,1,1])
# 砖铺地面
emit("ground_brick.gltf","ground_brick.bin",
     "../brick_floor/brick_floor.png","../brick_floor/brick_normal.png",
     6, 0.0, 0.7, [1,1,1,1])
