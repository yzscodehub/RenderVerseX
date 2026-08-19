#include "Animation/Data/AnimationClip.h"
#include "Animation/Data/Skeleton.h"
#include "Core/Hash/SHA256.h"
#include "Core/Job/JobSystem.h"
#include "Core/Log.h"
#include "Resource/Cooked/CookedAnimationArtifact.h"
#include "Resource/Loader/AnimationLoader.h"
#include "Resource/RuntimeResourcePolicy.h"
#include "Resource/Types/AnimationResource.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace RVX;
using namespace RVX::Animation;
using namespace RVX::Resource;

namespace
{
    namespace fs = std::filesystem;

    constexpr size_t AnimationHeaderByteCount = 52;

    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { Log::Initialize(); }
        void TearDown() override { Log::Shutdown(); }
    };

    [[maybe_unused]] const auto* g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment);

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            const auto token = std::chrono::steady_clock::now()
                                   .time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                     ("rvx_animation_resource_validation_" + std::to_string(token));
            fs::create_directories(m_path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        const fs::path& Get() const { return m_path; }

    private:
        fs::path m_path;
    };

    class ManagerGuard final
    {
    public:
        explicit ManagerGuard(const fs::path& cookedRoot)
        {
            ResourceManagerConfig config;
            config.asyncThreadCount = 0;
            config.runtimePolicy.mode = ResourceRuntimeMode::CookedRuntime;
            config.runtimePolicy.allowSourceAssetReads = false;
            config.runtimePolicy.requireCookedArtifacts = true;
            config.runtimePolicy.cookedRoot = cookedRoot.string();
            ResourceManager::Get().Initialize(config);
        }

        ~ManagerGuard()
        {
            ResourceManager::Get().Shutdown();
            JobSystem::Get().Shutdown();
        }
    };

    Skeleton::Ptr MakeSkeleton()
    {
        auto skeleton = Skeleton::Create();
        skeleton->Reserve(2);

        Bone root("CharacterArmature/Bone", -1);
        root.localBindPose = TransformSample::Identity();
        root.inverseBindPose = Mat4(1.0f);
        skeleton->AddBone(root);

        Bone hips("CharacterArmature/Bone/Body/Hips", 0);
        hips.localBindPose = TransformSample(
            Vec3(0.0f, 1.0f, 0.0f),
            Quat(1.0f, 0.0f, 0.0f, 0.0f),
            Vec3(1.0f));
        hips.inverseBindPose = inverse(hips.localBindPose.ToMatrix());
        skeleton->AddBone(hips);
        return skeleton;
    }

    TransformTrack MakeTrack(const std::string& target,
                             TimeUs duration,
                             float32 distance)
    {
        TransformTrack track;
        track.targetName = target;
        track.targetType = TrackTargetType::Bone;
        track.mode = TransformMode::TRS;
        track.translationKeyframes.emplace_back(
            0, Vec3(0.0f), InterpolationMode::Linear);
        track.translationKeyframes.emplace_back(
            duration, Vec3(0.0f, 0.0f, distance), InterpolationMode::Linear);
        track.rotationKeyframes.emplace_back(
            0, Quat(1.0f, 0.0f, 0.0f, 0.0f), InterpolationMode::Linear);
        track.rotationKeyframes.emplace_back(
            duration, Quat(1.0f, 0.0f, 0.0f, 0.0f), InterpolationMode::Linear);
        track.scaleKeyframes.emplace_back(
            0, Vec3(1.0f), InterpolationMode::Step);
        track.scaleKeyframes.emplace_back(
            duration, Vec3(1.0f), InterpolationMode::Step);
        return track;
    }

    AnimationClip::Ptr MakeClip(const std::string& name,
                                const Skeleton::ConstPtr& skeleton,
                                TimeUs duration,
                                float32 distance,
                                bool rootMotion)
    {
        auto clip = AnimationClip::Create(name);
        clip->description = name + " qualification clip";
        clip->duration = duration;
        clip->defaultWrapMode = WrapMode::Loop;
        clip->defaultSpeed = 1.0f;
        clip->metadata.sourceFps = 30;
        clip->metadata.sourceStartFrame = 0;
        clip->metadata.sourceEndFrame = 30;
        clip->metadata.sourceFormat = AnimationSourceFormat::glTF;
        clip->metadata.sourceFile = "Casual_Female.gltf";
        clip->metadata.customData.emplace("recipe", "source-clip");
        clip->metadata.customData.emplace("asset", "casual-female");
        clip->hasRootMotion = rootMotion;
        if (rootMotion)
            clip->rootMotionBoneName = "CharacterArmature/Bone/Body/Hips";
        clip->skeleton = skeleton;

        // Deliberately use non-canonical insertion order. The writer owns the
        // lexical on-disk ordering contract.
        clip->transformTracks.push_back(MakeTrack(
            "CharacterArmature/Bone/Body/Hips", duration, distance));
        clip->transformTracks.push_back(MakeTrack(
            "CharacterArmature/Bone", duration, 0.0f));
        return clip;
    }

    CookedAnimationArtifact MakeArtifact()
    {
        CookedAnimationArtifact artifact;
        artifact.sourcePath = "Assets/Characters/Casual_Female.gltf";
        artifact.skeleton = MakeSkeleton();
        artifact.clips.emplace("Idle", MakeClip("Idle", artifact.skeleton, 1'000'000, 0.0f, false));
        artifact.clips.emplace("Run", MakeClip("Run", artifact.skeleton, 750'000, 1.5f, false));
        artifact.clips.emplace("Walk", MakeClip("Walk", artifact.skeleton, 1'250'000, 1.25f, true));
        return artifact;
    }

    void WriteBytes(const fs::path& path, const std::vector<uint8>& bytes)
    {
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(file.good());
    }

    void WriteU32LE(std::vector<uint8>& bytes, size_t offset, uint32 value)
    {
        ASSERT_LE(offset + 4, bytes.size());
        for (uint32 shift = 0; shift < 32; shift += 8)
            bytes[offset++] = static_cast<uint8>((value >> shift) & 0xffu);
    }

    void RefreshPayloadDigest(std::vector<uint8>& bytes)
    {
        ASSERT_GE(bytes.size(), AnimationHeaderByteCount);
        const Hash::SHA256Digest digest = Hash::ComputeSHA256(
            bytes.data() + AnimationHeaderByteCount,
            bytes.size() - AnimationHeaderByteCount);
        std::copy(digest.begin(), digest.end(), bytes.begin() + 20);
    }

    void RefreshPayloadHeader(std::vector<uint8>& bytes)
    {
        ASSERT_GE(bytes.size(), AnimationHeaderByteCount);
        WriteU32LE(bytes, 12, static_cast<uint32>(bytes.size() - AnimationHeaderByteCount));
        for (uint32 shift = 32; shift < 64; shift += 8)
            bytes[12 + (shift / 8)] = static_cast<uint8>(
                ((static_cast<uint64>(bytes.size() - AnimationHeaderByteCount) >> shift) &
                 0xffu));
        RefreshPayloadDigest(bytes);
    }

    class PayloadCursor final
    {
    public:
        explicit PayloadCursor(std::span<const uint8> bytes) : m_bytes(bytes) {}

        bool ReadU8(uint8& value)
        {
            if (m_offset >= m_bytes.size())
                return false;
            value = m_bytes[m_offset++];
            return true;
        }

        bool ReadU32(uint32& value)
        {
            if (m_bytes.size() - m_offset < sizeof(uint32))
                return false;
            value = 0;
            for (uint32 shift = 0; shift < 32; shift += 8)
                value |= static_cast<uint32>(m_bytes[m_offset++]) << shift;
            return true;
        }

        bool Skip(size_t count)
        {
            if (count > m_bytes.size() - m_offset)
                return false;
            m_offset += count;
            return true;
        }

        bool SkipString()
        {
            uint32 count = 0;
            return ReadU32(count) && Skip(count);
        }

        size_t Offset() const { return m_offset; }

    private:
        std::span<const uint8> m_bytes;
        size_t m_offset = 0;
    };

    struct FirstTrackOffsets
    {
        size_t trackCount = 0;
        size_t targetType = 0;
        size_t translationCount = 0;
        size_t translationX = 0;
        size_t rotationW = 0;
    };

    bool LocateFirstTrackOffsets(const std::vector<uint8>& bytes,
                                 FirstTrackOffsets& offsets)
    {
        if (bytes.size() < AnimationHeaderByteCount)
            return false;
        PayloadCursor reader(std::span<const uint8>(bytes).subspan(
            AnimationHeaderByteCount));
        if (!reader.SkipString())
            return false;

        uint32 boneCount = 0;
        if (!reader.ReadU32(boneCount))
            return false;
        for (uint32 index = 0; index < boneCount; ++index)
        {
            if (!reader.SkipString() || !reader.Skip(4 + 12 + 16 + 12 + 64 + 4))
                return false;
        }

        uint32 clipCount = 0;
        if (!reader.ReadU32(clipCount) || clipCount == 0)
            return false;
        if (!reader.SkipString() || !reader.SkipString() ||
            !reader.Skip(8 + 1 + 4 + 4 + 4 + 4 + 1) ||
            !reader.SkipString())
        {
            return false;
        }

        uint32 metadataCount = 0;
        if (!reader.ReadU32(metadataCount))
            return false;
        for (uint32 index = 0; index < metadataCount; ++index)
        {
            if (!reader.SkipString() || !reader.SkipString())
                return false;
        }

        uint8 hasRootMotion = 0;
        if (!reader.ReadU8(hasRootMotion) || !reader.SkipString())
            return false;

        offsets.trackCount = AnimationHeaderByteCount + reader.Offset();
        uint32 trackCount = 0;
        if (!reader.ReadU32(trackCount) || trackCount == 0)
            return false;

        if (!reader.SkipString())
            return false;
        offsets.targetType = AnimationHeaderByteCount + reader.Offset();
        uint8 targetType = 0;
        uint8 transformMode = 0;
        if (!reader.ReadU8(targetType) || !reader.ReadU8(transformMode))
            return false;

        offsets.translationCount = AnimationHeaderByteCount + reader.Offset();
        uint32 translationCount = 0;
        offsets.translationX = AnimationHeaderByteCount + reader.Offset() + 13;
        if (!reader.ReadU32(translationCount) || translationCount == 0 ||
            !reader.Skip(static_cast<size_t>(translationCount) * 21))
        {
            return false;
        }

        uint32 rotationCount = 0;
        if (!reader.ReadU32(rotationCount) || rotationCount == 0 ||
            reader.Offset() + 9 > bytes.size() - AnimationHeaderByteCount)
        {
            return false;
        }
        offsets.rotationW = AnimationHeaderByteCount + reader.Offset() + 9;
        return true;
    }

    void WriteU32LEAt(std::vector<uint8>& bytes, size_t offset, uint32 value)
    {
        ASSERT_LE(offset + sizeof(uint32), bytes.size());
        for (uint32 shift = 0; shift < 32; shift += 8)
            bytes[offset++] = static_cast<uint8>((value >> shift) & 0xffu);
    }

    CookedAnimationArtifact CloneWithMutableClip(
        const CookedAnimationArtifact& source,
        const std::string& clipName,
        AnimationClip::Ptr& outClip)
    {
        CookedAnimationArtifact clone = source;
        outClip = source.clips.at(clipName)->Clone();
        outClip->skeleton = clone.skeleton;
        clone.clips[clipName] = outClip;
        return clone;
    }
} // namespace

TEST(AnimationResourceValidation, DeterministicRoundTripPreservesSkeletonClipsAndRootMotion)
{
    const CookedAnimationArtifact artifact = MakeArtifact();
    std::vector<uint8> firstBytes;
    std::string error;
    ASSERT_TRUE(SerializeCookedAnimationArtifact(artifact, firstBytes, error)) << error;
    ASSERT_GT(firstBytes.size(), AnimationHeaderByteCount);
    EXPECT_TRUE(std::equal(firstBytes.begin(), firstBytes.begin() + 8,
                           reinterpret_cast<const uint8*>(
                               RVX_ANIMATION_ARTIFACT_MAGIC)));

    std::vector<uint8> secondBytes;
    ASSERT_TRUE(SerializeCookedAnimationArtifact(artifact, secondBytes, error)) << error;
    EXPECT_EQ(secondBytes, firstBytes);

    CookedAnimationArtifact decoded;
    ASSERT_TRUE(DeserializeCookedAnimationArtifact(firstBytes, decoded, error)) << error;
    ASSERT_TRUE(decoded.skeleton);
    ASSERT_EQ(decoded.skeleton->GetBoneCount(), 2u);
    EXPECT_EQ(decoded.skeleton->bones[0].name, "CharacterArmature/Bone");
    EXPECT_EQ(decoded.skeleton->bones[1].parentIndex, 0);
    EXPECT_EQ(decoded.skeleton->rootBoneIndices, std::vector<int>({0}));
    ASSERT_EQ(decoded.clips.size(), 3u);
    EXPECT_EQ(decoded.clips.begin()->first, "Idle");
    ASSERT_TRUE(decoded.clips.contains("Walk"));
    const AnimationClip::ConstPtr walk = decoded.clips.at("Walk");
    ASSERT_TRUE(walk);
    EXPECT_TRUE(walk->hasRootMotion);
    EXPECT_EQ(walk->rootMotionBoneName,
              "CharacterArmature/Bone/Body/Hips");
    ASSERT_EQ(walk->transformTracks.size(), 2u);
    EXPECT_EQ(walk->transformTracks[0].targetName, "CharacterArmature/Bone");
    EXPECT_EQ(walk->transformTracks[1].translationKeyframes.back().time,
              1'250'000);
    EXPECT_FLOAT_EQ(walk->transformTracks[1].translationKeyframes.back().value.z,
                    1.25f);

    std::vector<uint8> roundTripBytes;
    ASSERT_TRUE(SerializeCookedAnimationArtifact(decoded, roundTripBytes, error)) << error;
    EXPECT_EQ(roundTripBytes, firstBytes);

    AnimationResource resource;
    ASSERT_TRUE(resource.SetData(decoded.skeleton, decoded.clips));
    EXPECT_EQ(resource.GetType(), ResourceType::Animation);
    EXPECT_TRUE(resource.GetClip("Idle"));
    EXPECT_FALSE(resource.GetClip("Missing"));
    EXPECT_GT(resource.GetMemoryUsage(), sizeof(AnimationResource));
}

TEST(AnimationResourceValidation, UnsupportedOrInconsistentSourceDataFailsBeforePublication)
{
    const CookedAnimationArtifact artifact = MakeArtifact();
    std::vector<uint8> bytes;
    std::string error;

    AnimationClip::Ptr mutableClip;
    CookedAnimationArtifact withEvent =
        CloneWithMutableClip(artifact, "Walk", mutableClip);
    mutableClip->AddEvent("Footstep", static_cast<TimeUs>(100'000));
    EXPECT_FALSE(SerializeCookedAnimationArtifact(withEvent, bytes, error));
    EXPECT_NE(error.find("supported-track"), std::string::npos);

    CookedAnimationArtifact withMatrix =
        CloneWithMutableClip(artifact, "Walk", mutableClip);
    mutableClip->transformTracks[0].mode = TransformMode::Matrix;
    mutableClip->transformTracks[0].matrixKeyframes.emplace_back(
        0, Mat4(1.0f), InterpolationMode::Linear);
    EXPECT_FALSE(SerializeCookedAnimationArtifact(withMatrix, bytes, error));

    CookedAnimationArtifact withDuplicate =
        CloneWithMutableClip(artifact, "Walk", mutableClip);
    mutableClip->transformTracks.push_back(mutableClip->transformTracks.front());
    EXPECT_FALSE(SerializeCookedAnimationArtifact(withDuplicate, bytes, error));
    EXPECT_NE(error.find("duplicate"), std::string::npos);

    CookedAnimationArtifact withBadDuration =
        CloneWithMutableClip(artifact, "Walk", mutableClip);
    ++mutableClip->duration;
    EXPECT_FALSE(SerializeCookedAnimationArtifact(withBadDuration, bytes, error));
    EXPECT_NE(error.find("duration"), std::string::npos);

    CookedAnimationArtifact withBadQuaternion =
        CloneWithMutableClip(artifact, "Walk", mutableClip);
    mutableClip->transformTracks[0].rotationKeyframes[0].value =
        Quat(0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(SerializeCookedAnimationArtifact(withBadQuaternion, bytes, error));

    CookedAnimationArtifact withBadSkeleton = artifact;
    auto badSkeleton = std::make_shared<Skeleton>(*artifact.skeleton);
    badSkeleton->bones[1].parentIndex = 1;
    withBadSkeleton.skeleton = badSkeleton;
    for (auto& [name, clip] : withBadSkeleton.clips)
    {
        auto clone = clip->Clone();
        clone->skeleton = badSkeleton;
        clip = std::move(clone);
    }
    EXPECT_FALSE(SerializeCookedAnimationArtifact(withBadSkeleton, bytes, error));

    AnimationResource resource;
    AnimationResource::AnimationClipMap wrongNames = artifact.clips;
    wrongNames.emplace("Alias", wrongNames.begin()->second);
    EXPECT_FALSE(resource.SetData(artifact.skeleton, std::move(wrongNames)));
    EXPECT_FALSE(resource.GetSkeleton());
    EXPECT_TRUE(resource.GetClips().empty());
}

TEST(AnimationResourceValidation, CorruptionTruncationAndAllocationBombsFailClosed)
{
    const CookedAnimationArtifact artifact = MakeArtifact();
    std::vector<uint8> bytes;
    std::string error;
    ASSERT_TRUE(SerializeCookedAnimationArtifact(artifact, bytes, error)) << error;

    auto expectRejected = [&](std::vector<uint8> malformed)
    {
        CookedAnimationArtifact decoded = MakeArtifact();
        EXPECT_FALSE(DeserializeCookedAnimationArtifact(malformed, decoded, error));
        EXPECT_FALSE(decoded.skeleton);
        EXPECT_TRUE(decoded.clips.empty());
    };

    std::vector<uint8> corrupt = bytes;
    corrupt.back() ^= 0x1u;
    expectRejected(std::move(corrupt));
    EXPECT_NE(error.find("SHA-256"), std::string::npos);

    std::vector<uint8> truncated = bytes;
    truncated.pop_back();
    expectRejected(std::move(truncated));

    std::vector<uint8> trailing = bytes;
    trailing.push_back(0u);
    expectRejected(std::move(trailing));

    std::vector<uint8> unknownSchema = bytes;
    WriteU32LE(unknownSchema, 8, 2u);
    expectRejected(std::move(unknownSchema));
    EXPECT_NE(error.find("schema"), std::string::npos);

    std::vector<uint8> boneCountBomb = bytes;
    const size_t boneCountOffset = AnimationHeaderByteCount + sizeof(uint32) +
                                   artifact.sourcePath.size();
    WriteU32LE(boneCountBomb, boneCountOffset, 65536u);
    RefreshPayloadDigest(boneCountBomb);
    expectRejected(std::move(boneCountBomb));
    EXPECT_NE(error.find("bone count"), std::string::npos);
}

TEST(AnimationResourceValidation, LegalCountAllocationBombsWithShortPayloadFailClosed)
{
    const CookedAnimationArtifact artifact = MakeArtifact();
    std::vector<uint8> bytes;
    std::string error;
    ASSERT_TRUE(SerializeCookedAnimationArtifact(artifact, bytes, error)) << error;

    FirstTrackOffsets offsets;
    ASSERT_TRUE(LocateFirstTrackOffsets(bytes, offsets));

    auto expectRejected = [&](std::vector<uint8> malformed)
    {
        RefreshPayloadHeader(malformed);
        CookedAnimationArtifact decoded = MakeArtifact();
        EXPECT_FALSE(DeserializeCookedAnimationArtifact(malformed, decoded, error));
        EXPECT_FALSE(decoded.skeleton);
        EXPECT_TRUE(decoded.clips.empty());
    };

    std::vector<uint8> boneCountBomb = bytes;
    const size_t boneCountOffset = AnimationHeaderByteCount + sizeof(uint32) +
                                   artifact.sourcePath.size();
    WriteU32LEAt(boneCountBomb, boneCountOffset, 65535u);
    boneCountBomb.resize(boneCountOffset + sizeof(uint32));
    expectRejected(std::move(boneCountBomb));

    std::vector<uint8> trackCountBomb = bytes;
    WriteU32LEAt(trackCountBomb, offsets.trackCount, 65535u);
    trackCountBomb.resize(offsets.trackCount + sizeof(uint32));
    expectRejected(std::move(trackCountBomb));

    std::vector<uint8> keyframeCountBomb = bytes;
    WriteU32LEAt(keyframeCountBomb, offsets.translationCount, 1'000'000u);
    keyframeCountBomb.resize(offsets.translationCount + sizeof(uint32));
    expectRejected(std::move(keyframeCountBomb));
}

TEST(AnimationResourceValidation, CanonicalWireRejectsNegativeZeroAndNonCanonicalQuaternion)
{
    const CookedAnimationArtifact artifact = MakeArtifact();
    std::vector<uint8> bytes;
    std::string error;
    ASSERT_TRUE(SerializeCookedAnimationArtifact(artifact, bytes, error)) << error;

    FirstTrackOffsets offsets;
    ASSERT_TRUE(LocateFirstTrackOffsets(bytes, offsets));

    auto expectRejected = [&](std::vector<uint8> malformed)
    {
        RefreshPayloadDigest(malformed);
        CookedAnimationArtifact decoded = MakeArtifact();
        EXPECT_FALSE(DeserializeCookedAnimationArtifact(malformed, decoded, error));
        EXPECT_FALSE(decoded.skeleton);
        EXPECT_TRUE(decoded.clips.empty());
    };

    std::vector<uint8> negativeZero = bytes;
    uint32 zeroBits = 0;
    WriteU32LEAt(negativeZero, offsets.translationX, zeroBits | 0x80000000u);
    expectRejected(std::move(negativeZero));

    std::vector<uint8> negativeQuaternion = bytes;
    uint32 oneBits = 0x3f800000u;
    WriteU32LEAt(negativeQuaternion, offsets.rotationW, oneBits | 0x80000000u);
    expectRejected(std::move(negativeQuaternion));
}

TEST(AnimationResourceValidation, UnsupportedNodeTrackAndMissingRootMotionTrackFailClosed)
{
    const CookedAnimationArtifact artifact = MakeArtifact();
    std::vector<uint8> bytes;
    std::string error;
    ASSERT_TRUE(SerializeCookedAnimationArtifact(artifact, bytes, error)) << error;

    FirstTrackOffsets offsets;
    ASSERT_TRUE(LocateFirstTrackOffsets(bytes, offsets));
    std::vector<uint8> nodeTrack = bytes;
    // targetType is a single byte in the wire format; do not modify the
    // neighboring transform-mode byte or the following keyframe count.
    nodeTrack[offsets.targetType] = static_cast<uint8>(TrackTargetType::Node);
    RefreshPayloadDigest(nodeTrack);
    CookedAnimationArtifact decoded = MakeArtifact();
    EXPECT_FALSE(DeserializeCookedAnimationArtifact(nodeTrack, decoded, error));
    EXPECT_FALSE(decoded.skeleton);
    EXPECT_TRUE(decoded.clips.empty());

    CookedAnimationArtifact missingRootTrack = artifact;
    auto skeleton = MakeSkeleton();
    skeleton->AddBone(Animation::Bone(
        "CharacterArmature/Bone/Body/Hips/Hand", 1));
    for (auto& [name, clip] : missingRootTrack.clips)
    {
        auto clone = clip->Clone();
        clone->skeleton = skeleton;
        if (name == "Walk")
            clone->rootMotionBoneName = "CharacterArmature/Bone/Body/Hips/Hand";
        clip = std::move(clone);
    }
    missingRootTrack.skeleton = skeleton;
    EXPECT_FALSE(SerializeCookedAnimationArtifact(missingRootTrack, bytes, error));
}

TEST(AnimationResourceValidation, SetDataDeepCopiesSkeletonClipsAndKeyframes)
{
    auto sourceSkeleton = MakeSkeleton();
    auto sourceClip = MakeClip("Walk", sourceSkeleton, 1'250'000, 1.25f, true);
    AnimationResource::AnimationClipMap sourceClips;
    sourceClips.emplace("Walk", sourceClip);

    AnimationResource resource;
    ASSERT_TRUE(resource.SetData(sourceSkeleton, sourceClips));
    ASSERT_TRUE(resource.GetSkeleton());
    ASSERT_TRUE(resource.GetClip("Walk"));
    ASSERT_NE(resource.GetSkeleton().get(), sourceSkeleton.get());
    ASSERT_NE(resource.GetClip("Walk").get(), sourceClip.get());

    const std::string originalBoneName = resource.GetSkeleton()->bones[0].name;
    const std::string originalTarget =
        resource.GetClip("Walk")->transformTracks[0].targetName;
    const float32 originalDistance = resource.GetClip("Walk")
        ->transformTracks[0].translationKeyframes.back().value.z;

    sourceSkeleton->bones[0].name = "mutated-source-root";
    sourceSkeleton->boneNameMap.erase("CharacterArmature/Bone");
    sourceSkeleton->boneNameMap.emplace("mutated-source-root", 0);
    sourceClip->transformTracks[0].targetName = "mutated-source-track";
    sourceClip->transformTracks[0].translationKeyframes.back().value.z =
        999.0f;

    EXPECT_EQ(resource.GetSkeleton()->bones[0].name, originalBoneName);
    EXPECT_EQ(resource.GetClip("Walk")->transformTracks[0].targetName, originalTarget);
    EXPECT_FLOAT_EQ(resource.GetClip("Walk")
                        ->transformTracks[0]
                        .translationKeyframes.back()
                        .value.z,
                    originalDistance);
}

TEST(AnimationResourceValidation,
     SetDataCanonicalizesQuaternionSignsWithoutRelaxingSkeletonCompatibility)
{
    auto sourceSkeleton = MakeSkeleton();
    auto sourceClip = MakeClip("Walk", sourceSkeleton, 1'250'000, 1.25f, true);
    const Quat nonCanonical(-1.0f, -0.0f, -0.0f, -0.0f);
    sourceSkeleton->bones[1].localBindPose.rotation = nonCanonical;
    for (Animation::KeyframeQuat& keyframe :
         sourceClip->transformTracks[0].rotationKeyframes)
    {
        keyframe.value = nonCanonical;
    }

    AnimationResource::AnimationClipMap sourceClips;
    sourceClips.emplace("Walk", sourceClip);

    AnimationResource sourceResource;
    ASSERT_TRUE(sourceResource.SetData(sourceSkeleton, sourceClips));

    // Publication must not mutate importer-owned input, including signed zero.
    EXPECT_EQ(std::bit_cast<uint32>(sourceSkeleton->bones[1]
                                        .localBindPose.rotation.w),
              0xbf800000u);
    EXPECT_EQ(std::bit_cast<uint32>(sourceSkeleton->bones[1]
                                        .localBindPose.rotation.x),
              0x80000000u);
    EXPECT_EQ(std::bit_cast<uint32>(sourceClip->transformTracks[0]
                                        .rotationKeyframes[0].value.w),
              0xbf800000u);

    const Animation::Skeleton::ConstPtr publishedSkeleton =
        sourceResource.GetSkeleton();
    const Animation::AnimationClip::ConstPtr publishedClip =
        sourceResource.GetClip("Walk");
    ASSERT_TRUE(publishedSkeleton);
    ASSERT_TRUE(publishedClip);
    ASSERT_EQ(publishedClip->transformTracks[0].rotationKeyframes.size(), 2u);

    const auto expectCanonicalIdentity = [](const Quat& value) {
        EXPECT_EQ(std::bit_cast<uint32>(value.w), 0x3f800000u);
        EXPECT_EQ(std::bit_cast<uint32>(value.x), 0u);
        EXPECT_EQ(std::bit_cast<uint32>(value.y), 0u);
        EXPECT_EQ(std::bit_cast<uint32>(value.z), 0u);
    };
    expectCanonicalIdentity(publishedSkeleton->bones[1].localBindPose.rotation);
    expectCanonicalIdentity(
        publishedClip->transformTracks[0].rotationKeyframes[0].value);
    expectCanonicalIdentity(
        publishedClip->transformTracks[0].rotationKeyframes[1].value);

    // The exact comparator remains strict: semantically equal but
    // non-canonical target data is still not accepted directly.
    Animation::AnimationClip::ConstPtr clone;
    EXPECT_FALSE(sourceResource.CloneClipForSkeleton("Walk", sourceSkeleton, clone));
    EXPECT_FALSE(clone);

    // Two independently published equivalent resources are canonicalized to
    // the same immutable skeleton representation and can rebind safely.
    auto equivalentSkeleton = MakeSkeleton();
    equivalentSkeleton->bones[1].localBindPose.rotation = nonCanonical;
    auto equivalentClip = MakeClip(
        "Walk", equivalentSkeleton, 1'250'000, 1.25f, true);
    for (Animation::KeyframeQuat& keyframe :
         equivalentClip->transformTracks[0].rotationKeyframes)
    {
        keyframe.value = nonCanonical;
    }
    AnimationResource::AnimationClipMap equivalentClips;
    equivalentClips.emplace("Walk", equivalentClip);
    AnimationResource equivalentResource;
    ASSERT_TRUE(equivalentResource.SetData(equivalentSkeleton, equivalentClips));

    ASSERT_TRUE(sourceResource.CloneClipForSkeleton(
        "Walk", equivalentResource.GetSkeleton(), clone));
    ASSERT_TRUE(clone);
    EXPECT_EQ(clone->skeleton.get(), equivalentResource.GetSkeleton().get());
}

TEST(AnimationResourceValidation,
     CloneClipForSkeletonRequiresExactCompatibleImmutableSkeleton)
{
    auto sourceSkeleton = MakeSkeleton();
    auto sourceClip = MakeClip("Walk", sourceSkeleton, 1'250'000, 1.25f, true);
    AnimationResource::AnimationClipMap sourceClips;
    sourceClips.emplace("Walk", sourceClip);

    AnimationResource resource;
    ASSERT_TRUE(resource.SetData(sourceSkeleton, sourceClips));
    const Animation::Skeleton::ConstPtr resourceSkeleton = resource.GetSkeleton();
    const Animation::AnimationClip::ConstPtr immutableSource = resource.GetClip("Walk");
    ASSERT_TRUE(resourceSkeleton);
    ASSERT_TRUE(immutableSource);

    const std::string sourceRootName = resourceSkeleton->bones[0].name;
    const float32 sourceDistance = immutableSource->transformTracks[0]
                                       .translationKeyframes.back().value.z;

    auto compatibleTarget = MakeSkeleton();
    compatibleTarget->bones[0].localBindPose.translation.x = -0.0f;

    Animation::AnimationClip::ConstPtr clone;
    ASSERT_TRUE(resource.CloneClipForSkeleton("Walk", compatibleTarget, clone));
    ASSERT_TRUE(clone);
    EXPECT_NE(clone.get(), immutableSource.get());
    EXPECT_EQ(clone->skeleton.get(), compatibleTarget.get());
    EXPECT_EQ(immutableSource->skeleton.get(), resourceSkeleton.get());
    EXPECT_NE(immutableSource->skeleton.get(), compatibleTarget.get());
    EXPECT_EQ(resource.GetClip("Walk").get(), immutableSource.get());
    EXPECT_EQ(resourceSkeleton->bones[0].name, sourceRootName);
    EXPECT_FLOAT_EQ(immutableSource->transformTracks[0]
                        .translationKeyframes.back().value.z,
                    sourceDistance);

    const auto expectRejected = [&resource, &immutableSource](
                                    const std::string& clipName,
                                    const Animation::Skeleton::ConstPtr& targetSkeleton) {
        Animation::AnimationClip::ConstPtr outClip = immutableSource;
        EXPECT_FALSE(resource.CloneClipForSkeleton(clipName, targetSkeleton, outClip));
        EXPECT_FALSE(outClip);
    };

    expectRejected("Walk", nullptr);

    auto nameMismatch = MakeSkeleton();
    nameMismatch->bones[1].name = "CharacterArmature/Bone/Body/RenamedHips";
    nameMismatch->boneNameMap.erase("CharacterArmature/Bone/Body/Hips");
    nameMismatch->boneNameMap.emplace(nameMismatch->bones[1].name, 1);
    expectRejected("Walk", nameMismatch);

    auto orderMismatch = MakeSkeleton();
    std::swap(orderMismatch->bones[0].name, orderMismatch->bones[1].name);
    orderMismatch->boneNameMap.clear();
    orderMismatch->boneNameMap.emplace(orderMismatch->bones[0].name, 0);
    orderMismatch->boneNameMap.emplace(orderMismatch->bones[1].name, 1);
    expectRejected("Walk", orderMismatch);

    auto parentMismatch = MakeSkeleton();
    parentMismatch->bones[1].parentIndex = -1;
    parentMismatch->BuildHierarchy();
    expectRejected("Walk", parentMismatch);

    auto localBindMismatch = MakeSkeleton();
    localBindMismatch->bones[1].localBindPose.translation.x = 0.25f;
    expectRejected("Walk", localBindMismatch);

    auto inverseBindMismatch = MakeSkeleton();
    inverseBindMismatch->bones[1].inverseBindPose[0][0] = 2.0f;
    expectRejected("Walk", inverseBindMismatch);

    auto nonFiniteTarget = MakeSkeleton();
    nonFiniteTarget->bones[1].localBindPose.translation.x =
        std::numeric_limits<float32>::quiet_NaN();
    expectRejected("Walk", nonFiniteTarget);
    expectRejected("Missing", compatibleTarget);
}

TEST(AnimationResourceValidation, PreparedAndManagerLoadsVerifyExactCookedBytes)
{
    const CookedAnimationArtifact artifact = MakeArtifact();
    std::vector<uint8> bytes;
    std::string error;
    ASSERT_TRUE(SerializeCookedAnimationArtifact(artifact, bytes, error)) << error;

    TemporaryDirectory temporary;
    const fs::path animationPath = temporary.Get() / "casual-female.rvxanim";
    WriteBytes(animationPath, bytes);

    AnimationLoader loader;
    EXPECT_TRUE(loader.CanLoad(animationPath.string()));
    EXPECT_FALSE(loader.CanLoad((temporary.Get() / "wrong.anim").string()));

    std::unique_ptr<AnimationResource> synchronous(
        static_cast<AnimationResource*>(loader.Load(animationPath.string())));
    ASSERT_TRUE(synchronous);
    EXPECT_TRUE(synchronous->IsLoaded());
    EXPECT_EQ(synchronous->GetClips().size(), 3u);

    ResourceLoadPreparationContext context;
    context.requestedPath = "cooked://casual-female.rvxanim";
    context.resolvedPath = animationPath.string();
    context.resourceIdentityPath = context.requestedPath;
    context.assetKey = MakeAssetKey(context.requestedPath, ResourceType::Animation);
    context.rootResourceId = GenerateResourceId(context.resourceIdentityPath);

    PreparedResourceBundle bundle;
    ResourceLoadError loadError;
    ASSERT_TRUE(loader.Prepare(context, bundle, loadError)) << loadError.message;
    ASSERT_TRUE(bundle.IsValid());
    const ResourceContentIdentity& identity = bundle.GetObservedContentIdentity();
    ASSERT_TRUE(identity.IsValid());
    EXPECT_EQ(identity.domain, ResourceContentIdentityDomain::CookedArtifact);
    EXPECT_EQ(identity.scope, ResourceContentIdentityScope::SelfContainedArtifact);
    EXPECT_EQ(identity.byteCount, bytes.size());
    EXPECT_EQ(identity.fileCount, 1u);
    EXPECT_EQ(identity.digest, Hash::FormatSHA256Digest(
        Hash::ComputeSHA256(bytes.data(), bytes.size())));
    const auto* prepared = dynamic_cast<const AnimationResource*>(bundle.GetRoot().Get());
    ASSERT_NE(prepared, nullptr);
    EXPECT_EQ(prepared->GetId(), context.rootResourceId);
    EXPECT_EQ(prepared->GetClips().size(), 3u);

    ResourceLoadPreparationContext cancelled = context;
    cancelled.isCancellationRequested = [] { return true; };
    PreparedResourceBundle cancelledBundle;
    EXPECT_FALSE(loader.Prepare(cancelled, cancelledBundle, loadError));
    EXPECT_EQ(loadError.code, ResourceLoadErrorCode::Cancelled);
    EXPECT_TRUE(cancelledBundle.IsEmpty());

    EXPECT_EQ(ResourceManager::GetTypeFromExtension(".RVXANIM"),
              ResourceType::Animation);
    ResourceRuntimePolicy policy;
    policy.mode = ResourceRuntimeMode::CookedRuntime;
    policy.allowSourceAssetReads = false;
    policy.requireCookedArtifacts = true;
    policy.cookedRoot = temporary.Get().string();
    const ResourcePathResolution resolution = ResolveRuntimeResourcePath(
        policy, "", "casual-female.rvxanim");
    ASSERT_TRUE(resolution.allowed) << resolution.diagnosticMessage;
    EXPECT_EQ(resolution.domain, ResourceLoadDomain::CookedArtifact);

    {
        ManagerGuard guard(temporary.Get());
        const AnimationHandle managed = ResourceManager::Get().Load<AnimationResource>(
            "cooked://casual-female.rvxanim");
        ASSERT_TRUE(managed);
        EXPECT_EQ(managed->GetClips().size(), 3u);
        EXPECT_EQ(managed->GetContentVerificationReceipt().status,
                  ResourceContentVerificationStatus::Observed);
        EXPECT_EQ(managed->GetContentVerificationReceipt().observed, identity);
    }
}

TEST(AnimationResourceValidation, PrepareFailurePreservesPreexistingBundle)
{
    const CookedAnimationArtifact artifact = MakeArtifact();
    std::vector<uint8> bytes;
    std::string error;
    ASSERT_TRUE(SerializeCookedAnimationArtifact(artifact, bytes, error)) << error;

    TemporaryDirectory temporary;
    const fs::path animationPath = temporary.Get() / "casual-female.rvxanim";
    WriteBytes(animationPath, bytes);

    ResourceLoadPreparationContext context;
    context.requestedPath = "cooked://casual-female.rvxanim";
    context.resolvedPath = animationPath.string();
    context.resourceIdentityPath = context.requestedPath;
    context.assetKey = MakeAssetKey(context.requestedPath, ResourceType::Animation);
    context.rootResourceId = GenerateResourceId(context.resourceIdentityPath);

    auto* dependency = new AnimationResource();
    const ResourceId dependencyId = GenerateResourceId("sentinel-animation-dependency");
    dependency->SetId(dependencyId);
    dependency->SetPath("sentinel-animation-dependency");
    ASSERT_TRUE(dependency->SetData(artifact.skeleton, artifact.clips));

    PreparedResourceBundle bundle;
    ResourceHandle<IResource> dependencyHandle(dependency);
    ASSERT_TRUE(bundle.AddDependency(dependencyHandle));
    ResourceContentIdentity sentinelIdentity;
    sentinelIdentity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
    sentinelIdentity.domain = ResourceContentIdentityDomain::CookedArtifact;
    sentinelIdentity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
    sentinelIdentity.algorithm = ResourceContentHashAlgorithm::SHA256;
    sentinelIdentity.digest = std::string(64, '0');
    sentinelIdentity.byteCount = 1;
    sentinelIdentity.fileCount = 1;
    ASSERT_TRUE(bundle.SetObservedContentIdentity(sentinelIdentity));

    const size_t entryCountBefore = bundle.GetEntries().size();
    const ResourceHandle<IResource> rootBefore = bundle.GetRoot();
    const ResourceContentIdentity identityBefore = bundle.GetObservedContentIdentity();
    ResourceLoadError loadError;

    std::vector<uint8> corruptBytes = bytes;
    corruptBytes.back() ^= 0x1u;
    WriteBytes(animationPath, corruptBytes);

    AnimationLoader loader;
    ASSERT_FALSE(loader.Prepare(context, bundle, loadError));
    EXPECT_EQ(loadError.code, ResourceLoadErrorCode::LoaderFailure);
    EXPECT_EQ(bundle.GetEntries().size(), entryCountBefore);
    EXPECT_EQ(bundle.GetRoot().Get(), rootBefore.Get());
    EXPECT_TRUE(bundle.Contains(dependencyId));
    EXPECT_EQ(bundle.GetObservedContentIdentity(), identityBefore);
}
