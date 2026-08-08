#include "Scene/TransformStore.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    RVX::uint64 ElapsedMicroseconds(Clock::time_point begin,
                                    Clock::time_point end)
    {
        return static_cast<RVX::uint64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin)
                .count());
    }
} // namespace

TEST(SceneTransformScaleValidation,
     StaticAndOnePercentDirtyWorkScaleWithChangedTransforms)
{
    constexpr std::array<size_t, 3> objectCounts = {
        1'000,
        10'000,
        100'000,
    };

    for (size_t objectCount : objectCounts)
    {
        SCOPED_TRACE(::testing::Message()
                     << "objectCount=" << objectCount);

        RVX::TransformStore store;
        store.Reserve(objectCount);
        RVX::HandlePool<RVX::ComponentHandle> handles;
        std::vector<RVX::ComponentHandle> registered;
        registered.reserve(objectCount);

        for (size_t index = 0; index < objectCount; ++index)
        {
            const RVX::ComponentHandle handle = handles.Allocate();
            ASSERT_TRUE(store.Register(
                handle,
                RVX::Vec3(static_cast<float>(index), 0.0f, 0.0f),
                RVX::Quat(1.0f, 0.0f, 0.0f, 0.0f),
                RVX::Vec3(1.0f)));
            registered.push_back(handle);
        }

        const Clock::time_point initialBegin = Clock::now();
        store.Resolve();
        const Clock::time_point initialEnd = Clock::now();
        EXPECT_EQ(store.GetLastResolveStats().registeredCount, objectCount);
        EXPECT_EQ(store.GetLastResolveStats().dirtyInputCount, objectCount);
        EXPECT_EQ(store.GetLastResolveStats().resolvedCount, objectCount);

        store.BeginFrame();
        const Clock::time_point staticBegin = Clock::now();
        store.Resolve();
        const Clock::time_point staticEnd = Clock::now();
        EXPECT_EQ(store.GetLastResolveStats().dirtyInputCount, 0u);
        EXPECT_EQ(store.GetLastResolveStats().resolvedCount, 0u);

        const size_t expectedDirtyCount = objectCount / 100;
        for (size_t index = 0; index < objectCount; index += 100)
        {
            store.SetLocalLocation(
                registered[index],
                RVX::Vec3(static_cast<float>(index), 1.0f, 0.0f));
        }
        ASSERT_EQ(store.GetDirtyCount(), expectedDirtyCount);

        const Clock::time_point dirtyBegin = Clock::now();
        store.Resolve();
        const Clock::time_point dirtyEnd = Clock::now();
        EXPECT_EQ(store.GetLastResolveStats().registeredCount, objectCount);
        EXPECT_EQ(store.GetLastResolveStats().dirtyInputCount,
                  expectedDirtyCount);
        EXPECT_EQ(store.GetLastResolveStats().resolvedCount,
                  expectedDirtyCount);
        EXPECT_FLOAT_EQ(
            store.GetPreviousWorldTransform(registered.front())[3].y,
            0.0f);
        EXPECT_FLOAT_EQ(store.GetWorldTransform(registered.front())[3].y,
                        1.0f);

        const std::string scale = std::to_string(objectCount);
        RecordProperty(
            "transform_" + scale + "_initial_us",
            ElapsedMicroseconds(initialBegin, initialEnd));
        RecordProperty(
            "transform_" + scale + "_static_us",
            ElapsedMicroseconds(staticBegin, staticEnd));
        RecordProperty(
            "transform_" + scale + "_one_percent_dirty_us",
            ElapsedMicroseconds(dirtyBegin, dirtyEnd));
    }
}
