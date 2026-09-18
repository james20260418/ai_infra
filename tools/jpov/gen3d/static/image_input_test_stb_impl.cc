// JPOV gen3d image_input_test — stb_image_write 的实现落地单元
//
// //third_party/stb:stb_image_write 是 header-only 目标（其实现宏写在自身 copts 里，
// 不向依赖方传播），因此**依赖方必须自己定义 STB_IMAGE_WRITE_IMPLEMENTATION** 并
// 在恰一个编译单元里 include 该头，否则 stbi_write_* 符号缺失。
// 生产侧由 tools/jpov/src/orm_unpack.cc 承担；本测试单独编译、不链接那个库，
// 故在此自带一份（并在下面 #define 实现宏）。
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"
