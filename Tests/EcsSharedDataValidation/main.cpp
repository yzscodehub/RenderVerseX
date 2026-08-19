#include "ECS/SharedDataStore.h"

#include <gtest/gtest.h>

#include <new>
#include <type_traits>

namespace
{
    struct MaterialBinding
    {
        RVX::uint32 material = 0;
        RVX::uint32 pass = 0;
    };

    struct ThrowingHash
    {
        bool* shouldThrow = nullptr;

        size_t operator()(const RVX::uint32& value) const
        {
            if (*shouldThrow)
            {
                throw std::bad_alloc();
            }
            return value;
        }
    };

    struct PaddedBinding
    {
        RVX::uint8 category = 0;
        RVX::uint32 material = 0;
    };

    static_assert(!std::has_unique_object_representations_v<PaddedBinding>);

    struct PaddedBindingHash
    {
        size_t operator()(const PaddedBinding& value) const noexcept
        {
            return (static_cast<size_t>(value.category) << 32u) ^ value.material;
        }
    };

    struct PaddedBindingEqual
    {
        bool operator()(const PaddedBinding& left, const PaddedBinding& right) const noexcept
        {
            return left.category == right.category && left.material == right.material;
        }
    };
}

TEST(EcsSharedDataValidation, EqualValuesShareOneHandleAndReleaseTracksReferences)
{
    RVX::ECS::SharedDataStore<MaterialBinding> sharedData;
    const MaterialBinding binding = {.material = 17u, .pass = 3u};

    const auto first = sharedData.Acquire(binding);
    const auto second = sharedData.Acquire(binding);

    ASSERT_TRUE(first.IsValid());
    EXPECT_EQ(second, first);
    EXPECT_EQ(sharedData.GetLiveCount(), 1U);
    EXPECT_EQ(sharedData.GetReferenceCount(first), 2U);
    const auto snapshot = sharedData.Read(first);
    ASSERT_TRUE(snapshot.has_value());
    auto modifiedSnapshot = *snapshot;
    modifiedSnapshot.material = 99u;
    const auto unchanged = sharedData.Read(first);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->material, binding.material);
    ASSERT_TRUE(sharedData.Release(first));
    EXPECT_TRUE(sharedData.IsValid(first));
    EXPECT_EQ(sharedData.GetReferenceCount(first), 1U);
    ASSERT_TRUE(sharedData.Release(second));
    EXPECT_FALSE(sharedData.IsValid(first));
    EXPECT_FALSE(sharedData.Read(first).has_value());
    EXPECT_EQ(sharedData.GetLiveCount(), 0U);
}

TEST(EcsSharedDataValidation, FinalReleaseReusesIndexWithANewGeneration)
{
    RVX::ECS::SharedDataStore<MaterialBinding> sharedData;
    const auto first = sharedData.Acquire({.material = 1u, .pass = 2u});
    ASSERT_TRUE(sharedData.Release(first));

    const auto replacement = sharedData.Acquire({.material = 8u, .pass = 9u});
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_EQ(replacement.GetIndex(), first.GetIndex());
    EXPECT_NE(replacement.GetGeneration(), first.GetGeneration());
    EXPECT_EQ(sharedData.GetReferenceCount(first), 0U);
    EXPECT_FALSE(sharedData.Release(first));

    const auto read = sharedData.Read(replacement);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->material, 8U);
    EXPECT_EQ(read->pass, 9U);
}

TEST(EcsSharedDataValidation, FailedAcquireLeavesTheStoreUnchanged)
{
    bool shouldThrow = true;
    RVX::ECS::SharedDataStore<RVX::uint32, ThrowingHash> sharedData({.shouldThrow = &shouldThrow});

    EXPECT_FALSE(sharedData.Acquire(7u).IsValid());
    EXPECT_EQ(sharedData.GetLiveCount(), 0U);

    shouldThrow = false;
    const auto handle = sharedData.Acquire(7u);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(sharedData.GetLiveCount(), 1U);
    EXPECT_EQ(sharedData.GetReferenceCount(handle), 1U);
}

TEST(EcsSharedDataValidation, CustomPolicySupportsPaddedValuesWithoutByteIdentity)
{
    RVX::ECS::SharedDataStore<PaddedBinding, PaddedBindingHash, PaddedBindingEqual> sharedData;

    const auto first = sharedData.Acquire({.category = 2u, .material = 17u});
    const auto second = sharedData.Acquire({.category = 2u, .material = 17u});

    ASSERT_TRUE(first.IsValid());
    EXPECT_EQ(second, first);
    EXPECT_EQ(sharedData.GetReferenceCount(first), 2U);
}
