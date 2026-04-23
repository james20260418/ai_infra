# Vec Library - N维向量库

## 概述

Vec库提供了一个通用的N维向量实现，支持2D和3D向量的特化版本。该库包含基本的向量运算、几何计算和实用功能。

## 目录结构

```
geom/common/
├── vec.h              # 主向量库实现
├── vec_test.cc        # GTest测试用例
├── check.h           # 检查断言工具
├── common.h          # 通用工具函数
├── common.cc         # 通用工具实现
├── math_util.h       # 数学工具函数
├── proto/vec.proto   # Protocol Buffers定义
└── BUILD             # Bazel构建配置
```

## 核心类

### `Vec<T, Dim>` - 通用N维向量
```cpp
template <typename T, int Dim>
struct Vec : public VecBase<T, Dim>;
```

### `Vec<T, 2>` - 2D向量特化
```cpp
template <typename T>
struct Vec<T, 2> : public VecBase<T, 2>;
```

### `Vec<T, 3>` - 3D向量特化
```cpp
template <typename T>
struct Vec<T, 3> : public VecBase<T, 3>;
```

## 类型别名

```cpp
using Vec1d = Vec<double, 1>;  // 1D双精度向量
using Vec2d = Vec<double, 2>;  // 2D双精度向量
using Vec3d = Vec<double, 3>;  // 3D双精度向量
```

## 基本用法

### 构造向量
```cpp
// 2D向量
Vec2d v1(1.0, 2.0);
Vec2d v2(3.0, 4.0);

// 3D向量
Vec3d v3(1.0, 2.0, 3.0);
Vec3d v4(4.0, 5.0, 6.0);

// 默认构造（零向量）
Vec2d zero;
```

### 访问元素
```cpp
Vec2d v(1.0, 2.0);

// 使用访问器
double x = v.x();      // 1.0
double y = v.y();      // 2.0

// 使用下标运算符
double first = v[0];   // 1.0
double second = v[1];  // 2.0

// 修改元素
v.x() = 3.0;
v[1] = 4.0;
```

## 向量运算

### 代数运算
```cpp
Vec2d a(1, 2);
Vec2d b(3, 4);

// 加法
Vec2d sum = a + b;      // (4, 6)
a += b;                 // a变为(4, 6)

// 减法
Vec2d diff = a - b;     // (-2, -2)
a -= b;                 // a变为(-2, -2)

// 标量乘法
Vec2d scaled = a * 2.0; // (2, 4)
a *= 2.0;               // a变为(2, 4)

// 一元负号
Vec2d neg = -a;         // (-1, -2)
```

### 点积和叉积
```cpp
Vec2d a(1, 2);
Vec2d b(3, 4);

// 点积
double dot = a.Dot(b);  // 1*3 + 2*4 = 11

// 2D叉积（返回标量）
double cross = a.Cross(b);  // 1*4 - 2*3 = -2

// 3D叉积（返回向量）
Vec3d c(1, 2, 3);
Vec3d d(4, 5, 6);
Vec3d cross3d = c.Cross(d);  // (-3, 6, -3)
```

### 范数和距离
```cpp
Vec2d a(3, 4);

// 范数（长度）
double norm = a.Norm();  // sqrt(3^2 + 4^2) = 5

// 单位向量
Vec2d unit = a.Unit();   // (0.6, 0.8)

// 距离平方
Vec2d b(0, 0);
double dist_sq = a.DistanceSquareTo(b);  // 25

// 近似比较
Vec2d c(3.000000001, 4.000000001);
bool is_near = a.IsNear(c, 1e-8);  // true
```

## 实用函数

### 随机向量
```cpp
std::mt19937 rng(42);  // 随机数生成器

// 随机单位向量
Vec2d random_unit = Vec2d::RandomUnit(&rng);

// 随机盒内向量
Vec2d lb(0, 0);  // 下界
Vec2d ub(1, 1);  // 上界
Vec2d random_in_box = Vec2d::RandomInBox(lb, ub, &rng);
```

### 比较运算符
```cpp
Vec2d a(1, 2);
Vec2d b(1, 2);
Vec2d c(1, 3);

bool equal = (a == b);    // true
bool not_equal = (a != c); // true
```

## Protocol Buffers支持

### 定义
```proto
syntax = "proto3";
package geom;

message Vec2dProto {
  double x = 1;
  double y = 2;
}
```

### 使用
```cpp
#include "geom/common/proto/vec.pb.h"

// 序列化和反序列化支持
Vec2d vec(1.0, 2.0);
geom::Vec2dProto proto;
// ... 转换逻辑
```

## 构建和使用

### Bazel依赖
```python
# 在BUILD文件中
cc_library(
    name = "vec",
    hdrs = ["vec.h"],
    deps = [
        ":check",
        ":common",
        ":math_util",
        ":vec_cc_proto",
    ],
    copts = ["-std=c++20"],
)

# 测试
cc_test(
    name = "vec_test",
    srcs = ["vec_test.cc"],
    deps = [
        ":vec",
        "@glog_static//:glog",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)
```

### 编译和测试
```bash
# 构建库
bazel build //geom/common:vec

# 运行测试
bazel test //geom/common:vec_test
```

## 设计特点

1. **模板化设计**：支持任意维度和数值类型
2. **特化优化**：对2D和3D向量进行特化优化
3. **值语义**：向量使用值语义，可安全拷贝
4. **constexpr支持**：编译时常量表达式支持
5. **性能优化**：内联关键操作，避免虚函数开销

## 测试覆盖

测试用例包括：
- 构造和访问
- 代数运算
- 点积和范数
- 比较运算符
- 工具函数
- 3D向量操作
- 访问器和修改器

## 注意事项

1. **默认构造函数**：创建零向量
2. **维度检查**：静态断言确保维度>0
3. **数值稳定性**：注意浮点数比较的精度问题
4. **内存布局**：向量是平凡可拷贝的（trivially copyable）

## 扩展建议

1. 添加更多几何算法（旋转、投影等）
2. 支持矩阵运算
3. 添加序列化/反序列化工具
4. 优化SIMD指令支持
5. 添加更多数值类型支持（float, int等）

---

*最后更新: 2026-04-23*