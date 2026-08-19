#include "ECS/DynamicBufferPool.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

TEST(EcsDynamicBufferValidation, ReuseAdvancesGenerationAndRejectsStaleHandles)
{
    RVX::ECS::DynamicBufferPool<RVX::uint32> buffers;
    const std::array<RVX::uint32, 2> initial = {4u, 8u};
    const auto first = buffers.Create(initial);

    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(buffers.Release(first));
    EXPECT_FALSE(buffers.IsValid(first));
    EXPECT_FALSE(buffers.Read(first).has_value());
    EXPECT_FALSE(buffers.Clear(first));

    const auto replacement = buffers.Create();
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_EQ(replacement.GetIndex(), first.GetIndex());
    EXPECT_NE(replacement.GetGeneration(), first.GetGeneration());
    EXPECT_FALSE(buffers.Replace(first, initial));
    EXPECT_FALSE(buffers.Append(first, initial));
}

TEST(EcsDynamicBufferValidation, ReadUsesSnapshotsAndMutationsAreAtomicForLiveHandles)
{
    RVX::ECS::DynamicBufferPool<RVX::uint32> buffers;
    const std::array<RVX::uint32, 2> initial = {1u, 2u};
    const auto handle = buffers.Create(initial);
    ASSERT_TRUE(handle.IsValid());

    const auto beforeAppend = buffers.Read(handle);
    ASSERT_TRUE(beforeAppend.has_value());
    EXPECT_EQ(*beforeAppend, (std::vector<RVX::uint32>{1u, 2u}));

    ASSERT_TRUE(buffers.Append(handle, 3u));
    const auto afterAppend = buffers.Read(handle);
    ASSERT_TRUE(afterAppend.has_value());
    EXPECT_EQ(*afterAppend, (std::vector<RVX::uint32>{1u, 2u, 3u}));
    EXPECT_EQ(*beforeAppend, (std::vector<RVX::uint32>{1u, 2u}));

    const std::array<RVX::uint32, 3> replacement = {5u, 6u, 7u};
    ASSERT_TRUE(buffers.Replace(handle, replacement));
    const auto afterReplace = buffers.Read(handle);
    ASSERT_TRUE(afterReplace.has_value());
    EXPECT_EQ(*afterReplace, (std::vector<RVX::uint32>{5u, 6u, 7u}));

    ASSERT_TRUE(buffers.Clear(handle));
    const auto afterClear = buffers.Read(handle);
    ASSERT_TRUE(afterClear.has_value());
    EXPECT_TRUE(afterClear->empty());
    EXPECT_EQ(buffers.GetBufferCount(), 1U);
}
