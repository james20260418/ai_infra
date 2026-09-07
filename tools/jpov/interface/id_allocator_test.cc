// IdAllocator 单测：freelist / LIFO 空槽复用 + live 集 + 避回绕 / 重复 release 忽略。
#include "tools/jpov/interface/id_allocator.h"

#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

namespace jpov {

TEST(IdAllocatorTest, MonotonicWhenNoRelease) {
    IdAllocator alloc;
    EXPECT_FALSE(alloc.occupied(1));
    const uint32_t a = alloc.Acquire();
    const uint32_t b = alloc.Acquire();
    const uint32_t c = alloc.Acquire();
    EXPECT_EQ(a, 1u);
    EXPECT_EQ(b, 2u);
    EXPECT_EQ(c, 3u);
    EXPECT_EQ(alloc.size(), 3u);
    EXPECT_TRUE(alloc.occupied(1));
    EXPECT_FALSE(alloc.empty());
}

TEST(IdAllocatorTest, NeverReturnsZero) {
    IdAllocator alloc;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_NE(alloc.Acquire(), 0u);
    }
}

TEST(IdAllocatorTest, ReleaseThenReuseLifo) {
    IdAllocator alloc;
    (void)alloc.Acquire();  // 1
    (void)alloc.Acquire();  // 2
    const uint32_t c = alloc.Acquire();  // 3
    alloc.Release(1);
    alloc.Release(3);
    // LIFO：最近释放的 3 先被复用。
    const uint32_t x = alloc.Acquire();
    EXPECT_EQ(x, 3u);
    const uint32_t y = alloc.Acquire();
    EXPECT_EQ(y, 1u);
    // 空槽耗尽后开新号。
    const uint32_t z = alloc.Acquire();
    EXPECT_EQ(z, 4u);
    // 全部 release 到空 (注意：id 2 一直没被释放，仍在 live)。
    alloc.Release(2u);
    alloc.Release(x);
    alloc.Release(y);
    alloc.Release(z);
    EXPECT_TRUE(alloc.empty());
}

TEST(IdAllocatorTest, DoubleReleaseIgnoredNoCorruption) {
    IdAllocator alloc;
    const uint32_t id = alloc.Acquire();  // 1
    alloc.Release(id);
    alloc.Release(id);  // 重复释放 → 静默忽略，不得把 1 放回两次（否则会重复发出同一号）。
    const uint32_t a = alloc.Acquire();
    const uint32_t b = alloc.Acquire();
    ASSERT_EQ(a, 1u);   // 复用刚释放的 1
    ASSERT_EQ(b, 2u);   // 不该再拿到 1（否则 live 撞号 → 踩踏）
    EXPECT_TRUE(alloc.occupied(1));
    EXPECT_TRUE(alloc.occupied(2));
    EXPECT_FALSE(alloc.occupied(1) && (a == b));
}

TEST(IdAllocatorTest, ReleaseNeverAllocatedIgnored) {
    IdAllocator alloc;
    alloc.Release(7);   // 从未分配 → 忽略
    alloc.Release(0);   // 无效 0 → 忽略
    EXPECT_TRUE(alloc.empty());
    EXPECT_EQ(alloc.Acquire(), 1u);  // 7 没进池，新号仍从 1 单调开
}

TEST(IdAllocatorTest, OccupancyMirrorsReleaseAcquire) {
    IdAllocator alloc;
    std::vector<uint32_t> ids;
    for (int i = 0; i < 5; ++i) ids.push_back(alloc.Acquire());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_TRUE(alloc.occupied(ids[i]));
    }
    alloc.Release(ids[2]);
    alloc.Release(ids[4]);
    EXPECT_FALSE(alloc.occupied(ids[2]));
    EXPECT_FALSE(alloc.occupied(ids[4]));
    EXPECT_EQ(alloc.size(), 3u);
}

}  // namespace jpov
