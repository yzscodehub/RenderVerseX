/**
 * @file CookedAnimationArtifact.cpp
 * @brief Canonical little-endian .rvxanim codec.
 */

#include "Resource/Cooked/CookedAnimationArtifact.h"

#include "Core/Hash/SHA256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <set>
#include <string_view>
#include <unordered_set>

namespace RVX::Resource
{
namespace
{
    constexpr uint64 MaxArtifactBytes = 256ull * 1024ull * 1024ull;
    constexpr uint32 MaxStringBytes = 64u * 1024u;
    constexpr uint32 MaxBoneCount = 65535u;
    constexpr uint32 MaxClipCount = 4096u;
    constexpr uint32 MaxTracksPerClip = 65535u;
    constexpr uint32 MaxMetadataEntries = 4096u;
    constexpr uint32 MaxKeyframesPerChannel = 1'000'000u;
    constexpr uint32 MaxTotalTracks = 65535u;
    constexpr uint64 MaxTotalKeyframes = 4'000'000ull;
    constexpr size_t MinimumBoneWireBytes = 116u;
    constexpr size_t MinimumMetadataEntryWireBytes = 9u;
    constexpr size_t MinimumTransformTrackWireBytes = 86u;
    constexpr size_t Vec3KeyframeWireBytes = 21u;
    constexpr size_t QuatKeyframeWireBytes = 25u;
    constexpr size_t HeaderByteCount = 8u + sizeof(uint32) + sizeof(uint64) +
                                       Hash::RVX_SHA256_DIGEST_SIZE;

    class ByteWriter
    {
    public:
        explicit ByteWriter(size_t maxBytes = MaxArtifactBytes)
            : m_maxBytes(maxBytes)
        {
        }

        void U8(uint8 value)
        {
            if (!CanAppend(1))
                return;
            m_bytes.push_back(value);
        }

        void U32(uint32 value)
        {
            for (uint32 shift = 0; shift < 32; shift += 8)
                U8(static_cast<uint8>((value >> shift) & 0xffu));
        }

        void I32(int32 value) { U32(std::bit_cast<uint32>(value)); }

        void U64(uint64 value)
        {
            for (uint32 shift = 0; shift < 64; shift += 8)
                U8(static_cast<uint8>((value >> shift) & 0xffu));
        }

        void I64(int64 value) { U64(std::bit_cast<uint64>(value)); }

        void F32(float32 value)
        {
            if (value == 0.0f)
                value = 0.0f;
            U32(std::bit_cast<uint32>(value));
        }

        void String(std::string_view value)
        {
            if (value.size() > MaxStringBytes ||
                value.size() > std::numeric_limits<uint32>::max())
            {
                m_valid = false;
                return;
            }
            U32(static_cast<uint32>(value.size()));
            Bytes(reinterpret_cast<const uint8*>(value.data()), value.size());
        }

        void Bytes(const uint8* values, size_t count)
        {
            if (!CanAppend(count))
                return;
            m_bytes.insert(m_bytes.end(), values, values + count);
        }

        [[nodiscard]] bool IsValid() const { return m_valid; }
        [[nodiscard]] const std::vector<uint8>& GetBytes() const { return m_bytes; }
        [[nodiscard]] std::vector<uint8> TakeBytes() { return std::move(m_bytes); }

    private:
        bool CanAppend(size_t count)
        {
            if (!m_valid || count > m_maxBytes - m_bytes.size())
            {
                m_valid = false;
                return false;
            }
            return true;
        }

        std::vector<uint8> m_bytes;
        size_t m_maxBytes = 0;
        bool m_valid = true;
    };

    class ByteReader
    {
    public:
        explicit ByteReader(std::span<const uint8> bytes)
            : m_bytes(bytes)
        {
        }

        bool U8(uint8& value)
        {
            if (Remaining() < 1)
                return false;
            value = m_bytes[m_offset++];
            return true;
        }

        bool U32(uint32& value)
        {
            if (Remaining() < 4)
                return false;
            value = 0;
            for (uint32 shift = 0; shift < 32; shift += 8)
                value |= static_cast<uint32>(m_bytes[m_offset++]) << shift;
            return true;
        }

        bool I32(int32& value)
        {
            uint32 bits = 0;
            if (!U32(bits))
                return false;
            value = std::bit_cast<int32>(bits);
            return true;
        }

        bool U64(uint64& value)
        {
            if (Remaining() < 8)
                return false;
            value = 0;
            for (uint32 shift = 0; shift < 64; shift += 8)
                value |= static_cast<uint64>(m_bytes[m_offset++]) << shift;
            return true;
        }

        bool I64(int64& value)
        {
            uint64 bits = 0;
            if (!U64(bits))
                return false;
            value = std::bit_cast<int64>(bits);
            return true;
        }

        bool F32(float32& value)
        {
            uint32 bits = 0;
            if (!U32(bits))
                return false;
            value = std::bit_cast<float32>(bits);
            return std::isfinite(value) &&
                   !(value == 0.0f && std::signbit(value));
        }

        bool String(std::string& value, bool allowEmpty = true)
        {
            uint32 byteCount = 0;
            if (!U32(byteCount) || byteCount > MaxStringBytes ||
                byteCount > Remaining() || (!allowEmpty && byteCount == 0))
            {
                return false;
            }
            const char* begin = reinterpret_cast<const char*>(m_bytes.data() + m_offset);
            value.assign(begin, begin + byteCount);
            m_offset += byteCount;
            return value.find('\0') == std::string::npos;
        }

        [[nodiscard]] size_t Remaining() const { return m_bytes.size() - m_offset; }
        [[nodiscard]] bool AtEnd() const { return m_offset == m_bytes.size(); }

    private:
        std::span<const uint8> m_bytes;
        size_t m_offset = 0;
    };

    bool IsValidString(std::string_view value, bool allowEmpty = true)
    {
        return value.size() <= MaxStringBytes &&
               (allowEmpty || !value.empty()) &&
               value.find('\0') == std::string_view::npos;
    }

    bool IsFinite(const Vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    bool IsFinite(const Quat& value)
    {
        return std::isfinite(value.w) && std::isfinite(value.x) &&
               std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsUnitQuaternion(const Quat& value)
    {
        if (!IsFinite(value))
            return false;
        const float32 lengthSquared = value.w * value.w + value.x * value.x +
                                      value.y * value.y + value.z * value.z;
        return std::abs(lengthSquared - 1.0f) <= 1.0e-3f;
    }

    bool IsCanonicalQuaternion(const Quat& value)
    {
        if (!IsUnitQuaternion(value))
            return false;
        const std::array<float32, 4> components = {
            value.w, value.x, value.y, value.z};
        for (const float32 component : components)
        {
            if (component > 0.0f)
                return true;
            if (component < 0.0f)
                return false;
        }
        return false;
    }

    bool IsFinite(const Mat4& value)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!std::isfinite(value[column][row]))
                    return false;
            }
        }
        return true;
    }

    bool IsSupportedInterpolation(Animation::InterpolationMode interpolation)
    {
        return interpolation == Animation::InterpolationMode::Step ||
               interpolation == Animation::InterpolationMode::Linear;
    }

    template<typename KeyframeType, typename ValueValidator>
    bool ValidateKeyframes(const std::vector<KeyframeType>& keyframes,
                           Animation::TimeUs duration,
                           uint64& totalKeyframes,
                           ValueValidator&& validateValue,
                           std::string& outError)
    {
        if (keyframes.empty() || keyframes.size() > MaxKeyframesPerChannel ||
            totalKeyframes + keyframes.size() > MaxTotalKeyframes)
        {
            outError = "Animation channel has an invalid keyframe count.";
            return false;
        }

        Animation::TimeUs previous = -1;
        for (const KeyframeType& keyframe : keyframes)
        {
            if (keyframe.time < 0 || keyframe.time > duration ||
                keyframe.time <= previous ||
                !IsSupportedInterpolation(keyframe.interpolation) ||
                !validateValue(keyframe.value))
            {
                outError = "Animation keyframes must be finite, supported, and strictly increasing.";
                return false;
            }
            previous = keyframe.time;
        }
        totalKeyframes += keyframes.size();
        return true;
    }

    bool ValidateSkeleton(const Animation::Skeleton& skeleton,
                          std::string& outError)
    {
        if (skeleton.bones.empty() || skeleton.bones.size() > MaxBoneCount ||
            !skeleton.Validate())
        {
            outError = "Animation artifact requires a valid non-empty skeleton.";
            return false;
        }

        std::unordered_set<std::string> names;
        names.reserve(skeleton.bones.size());
        std::vector<std::vector<int>> expectedChildren(skeleton.bones.size());
        std::vector<int> expectedRoots;
        for (size_t index = 0; index < skeleton.bones.size(); ++index)
        {
            const Animation::Bone& bone = skeleton.bones[index];
            if (!IsValidString(bone.name, false) ||
                !names.insert(bone.name).second ||
                bone.parentIndex >= static_cast<int>(index) || bone.parentIndex < -1 ||
                skeleton.FindBoneIndex(bone.name) != static_cast<int>(index) ||
                !IsFinite(bone.localBindPose.translation) ||
                !IsUnitQuaternion(bone.localBindPose.rotation) ||
                !IsFinite(bone.localBindPose.scale) ||
                std::abs(bone.localBindPose.scale.x) <= 1.0e-8f ||
                std::abs(bone.localBindPose.scale.y) <= 1.0e-8f ||
                std::abs(bone.localBindPose.scale.z) <= 1.0e-8f ||
                !IsFinite(bone.inverseBindPose) ||
                !std::isfinite(bone.boundingRadius) || bone.boundingRadius < 0.0f)
            {
                outError = "Animation skeleton contains invalid topology, identity, or bind data.";
                return false;
            }

            if (bone.parentIndex < 0)
                expectedRoots.push_back(static_cast<int>(index));
            else
                expectedChildren[static_cast<size_t>(bone.parentIndex)].push_back(
                    static_cast<int>(index));
        }

        if (skeleton.rootBoneIndices != expectedRoots)
        {
            outError = "Animation skeleton root list does not match its parent topology.";
            return false;
        }
        for (size_t index = 0; index < skeleton.bones.size(); ++index)
        {
            if (skeleton.bones[index].childIndices != expectedChildren[index])
            {
                outError = "Animation skeleton child lists do not match its parent topology.";
                return false;
            }
        }
        return true;
    }

    bool ValidateClip(const std::string& mapName,
                      const Animation::AnimationClip& clip,
                      const Animation::Skeleton::ConstPtr& skeleton,
                      uint32& totalTracks,
                      uint64& totalKeyframes,
                      std::string& outError)
    {
        if (!IsValidString(mapName, false) || clip.name != mapName ||
            !IsValidString(clip.name, false) ||
            !IsValidString(clip.description) ||
            !IsValidString(clip.metadata.sourceFile) ||
            clip.skeleton != skeleton || clip.duration < 0 ||
            static_cast<uint8>(clip.defaultWrapMode) >
                static_cast<uint8>(Animation::WrapMode::ClampForever) ||
            !std::isfinite(clip.defaultSpeed) || clip.defaultSpeed <= 0.0f ||
            clip.metadata.sourceFps <= 0 ||
            clip.metadata.sourceEndFrame < clip.metadata.sourceStartFrame ||
            static_cast<uint8>(clip.metadata.sourceFormat) >
                static_cast<uint8>(Animation::AnimationSourceFormat::Custom) ||
            clip.metadata.customData.size() > MaxMetadataEntries ||
            clip.transformTracks.empty() ||
            clip.transformTracks.size() > MaxTracksPerClip ||
            clip.transformTracks.size() > MaxTotalTracks - totalTracks ||
            !clip.blendShapeTracks.empty() || !clip.propertyTracks.empty() ||
            !clip.visibilityTracks.empty() || !clip.eventTrack.IsEmpty() ||
            (clip.hasRootMotion &&
             (clip.rootMotionBoneName.empty() ||
              skeleton->FindBoneIndex(clip.rootMotionBoneName) < 0)) ||
            (!clip.hasRootMotion && !clip.rootMotionBoneName.empty()))
        {
            outError = "Animation clip metadata or supported-track contract is invalid.";
            return false;
        }

        for (const auto& [key, value] : clip.metadata.customData)
        {
            if (!IsValidString(key, false) || !IsValidString(value))
            {
                outError = "Animation clip metadata contains an invalid string.";
                return false;
            }
        }

        totalTracks += static_cast<uint32>(clip.transformTracks.size());
        std::set<std::pair<uint8, std::string>> targets;
        Animation::TimeUs computedDuration = 0;
        bool rootMotionTrackFound = false;
        for (const Animation::TransformTrack& track : clip.transformTracks)
        {
            if (!IsValidString(track.targetName, false) ||
                track.targetType != Animation::TrackTargetType::Bone ||
                track.mode != Animation::TransformMode::TRS ||
                !track.matrixKeyframes.empty() ||
                !targets.emplace(static_cast<uint8>(track.targetType),
                                 track.targetName).second ||
                (track.targetType == Animation::TrackTargetType::Bone &&
                 skeleton->FindBoneIndex(track.targetName) < 0))
            {
                outError = "Animation clip contains an invalid or duplicate transform target.";
                return false;
            }
            rootMotionTrackFound = rootMotionTrackFound ||
                (clip.hasRootMotion &&
                 track.targetName == clip.rootMotionBoneName);

            if (!ValidateKeyframes(track.translationKeyframes,
                                   clip.duration,
                                   totalKeyframes,
                                   [](const Vec3& value) { return IsFinite(value); },
                                   outError) ||
                !ValidateKeyframes(track.rotationKeyframes,
                                   clip.duration,
                                   totalKeyframes,
                                   [](const Quat& value) { return IsUnitQuaternion(value); },
                                   outError) ||
                !ValidateKeyframes(track.scaleKeyframes,
                                   clip.duration,
                                   totalKeyframes,
                                   [](const Vec3& value) { return IsFinite(value); },
                                   outError))
            {
                return false;
            }
            computedDuration = std::max(computedDuration,
                                        track.translationKeyframes.back().time);
            computedDuration = std::max(computedDuration,
                                        track.rotationKeyframes.back().time);
            computedDuration = std::max(computedDuration,
                                        track.scaleKeyframes.back().time);
        }

        if (computedDuration != clip.duration)
        {
            outError = "Animation clip duration does not match its canonical keyframe range.";
            return false;
        }
        if (clip.hasRootMotion && !rootMotionTrackFound)
        {
            outError = "Animation clip root-motion bone has no transform track.";
            return false;
        }
        return true;
    }

    bool ValidatePayload(
        const Animation::Skeleton::ConstPtr& skeleton,
        const CookedAnimationArtifact::AnimationClipMap& clips,
        std::string& outError)
    {
        if (!skeleton || clips.empty() || clips.size() > MaxClipCount ||
            !ValidateSkeleton(*skeleton, outError))
        {
            if (outError.empty())
                outError = "Animation payload is incomplete.";
            return false;
        }

        uint32 totalTracks = 0;
        uint64 totalKeyframes = 0;
        for (const auto& [name, clip] : clips)
        {
            if (!clip || !ValidateClip(name,
                                       *clip,
                                       skeleton,
                                       totalTracks,
                                       totalKeyframes,
                                       outError))
                return false;
        }
        return true;
    }

    bool ValidateArtifact(const CookedAnimationArtifact& artifact,
                          std::string& outError)
    {
        if (!IsValidString(artifact.sourcePath, false))
        {
            outError = "Animation artifact source identity is invalid.";
            return false;
        }
        return ValidatePayload(artifact.skeleton, artifact.clips, outError);
    }

    void WriteVec3(ByteWriter& writer, const Vec3& value)
    {
        writer.F32(value.x);
        writer.F32(value.y);
        writer.F32(value.z);
    }

    void WriteQuat(ByteWriter& writer, const Quat& value)
    {
        Quat canonical = value;
        if (!IsCanonicalQuaternion(canonical))
        {
            canonical.w = -canonical.w;
            canonical.x = -canonical.x;
            canonical.y = -canonical.y;
            canonical.z = -canonical.z;
        }
        writer.F32(canonical.w);
        writer.F32(canonical.x);
        writer.F32(canonical.y);
        writer.F32(canonical.z);
    }

    void WriteMat4(ByteWriter& writer, const Mat4& value)
    {
        for (uint32 column = 0; column < 4; ++column)
            for (uint32 row = 0; row < 4; ++row)
                writer.F32(value[column][row]);
    }

    bool ReadVec3(ByteReader& reader, Vec3& value)
    {
        return reader.F32(value.x) && reader.F32(value.y) && reader.F32(value.z);
    }

    bool ReadQuat(ByteReader& reader, Quat& value)
    {
        return reader.F32(value.w) && reader.F32(value.x) &&
               reader.F32(value.y) && reader.F32(value.z) &&
               IsCanonicalQuaternion(value);
    }

    bool ReadMat4(ByteReader& reader, Mat4& value)
    {
        for (uint32 column = 0; column < 4; ++column)
            for (uint32 row = 0; row < 4; ++row)
                if (!reader.F32(value[column][row]))
                    return false;
        return true;
    }

    template<typename KeyframeType, typename WriteValue>
    void WriteKeyframes(ByteWriter& writer,
                        const std::vector<KeyframeType>& keyframes,
                        WriteValue&& writeValue)
    {
        writer.U32(static_cast<uint32>(keyframes.size()));
        for (const KeyframeType& keyframe : keyframes)
        {
            writer.I64(keyframe.time);
            writer.U8(static_cast<uint8>(keyframe.interpolation));
            writeValue(writer, keyframe.value);
        }
    }

    template<typename KeyframeType, typename ReadValue>
    bool ReadKeyframes(ByteReader& reader,
                       std::vector<KeyframeType>& keyframes,
                       uint64& totalKeyframes,
                       size_t wireStride,
                       ReadValue&& readValue)
    {
        uint32 count = 0;
        if (!reader.U32(count) || count == 0 || count > MaxKeyframesPerChannel ||
            totalKeyframes + count > MaxTotalKeyframes || wireStride == 0 ||
            count > reader.Remaining() / wireStride)
        {
            return false;
        }

        keyframes.clear();
        keyframes.reserve(count);
        for (uint32 index = 0; index < count; ++index)
        {
            Animation::TimeUs time = 0;
            uint8 interpolationValue = 0;
            typename KeyframeType::value_type value{};
            if (!reader.I64(time) || !reader.U8(interpolationValue) ||
                interpolationValue >
                    static_cast<uint8>(Animation::InterpolationMode::Linear) ||
                !readValue(reader, value))
            {
                return false;
            }
            keyframes.emplace_back(
                time,
                value,
                static_cast<Animation::InterpolationMode>(interpolationValue));
        }
        totalKeyframes += count;
        return true;
    }
} // namespace

bool ValidateCookedAnimationPayload(
    const Animation::Skeleton::ConstPtr& skeleton,
    const CookedAnimationArtifact::AnimationClipMap& clips,
    std::string& outError)
{
    outError.clear();
    try
    {
        return ValidatePayload(skeleton, clips, outError);
    }
    catch (const std::bad_alloc&)
    {
        outError = "Animation payload validation exceeded available memory.";
        return false;
    }
    catch (const std::exception& exception)
    {
        outError = std::string("Animation payload validation failed: ") +
                   exception.what();
        return false;
    }
    catch (...)
    {
        outError = "Animation payload validation failed with an unknown exception.";
        return false;
    }
}

bool SerializeCookedAnimationArtifact(const CookedAnimationArtifact& artifact,
                                      std::vector<uint8>& outBytes,
                                      std::string& outError)
{
    outBytes.clear();
    outError.clear();
    try
    {
        if (!ValidateArtifact(artifact, outError))
            return false;

        ByteWriter payload(MaxArtifactBytes - HeaderByteCount);
        payload.String(artifact.sourcePath);
        payload.U32(static_cast<uint32>(artifact.skeleton->bones.size()));
        for (const Animation::Bone& bone : artifact.skeleton->bones)
        {
            payload.String(bone.name);
            payload.I32(bone.parentIndex);
            WriteVec3(payload, bone.localBindPose.translation);
            WriteQuat(payload, bone.localBindPose.rotation);
            WriteVec3(payload, bone.localBindPose.scale);
            WriteMat4(payload, bone.inverseBindPose);
            payload.F32(bone.boundingRadius);
        }

        payload.U32(static_cast<uint32>(artifact.clips.size()));
        for (const auto& [name, clip] : artifact.clips)
        {
        payload.String(name);
        payload.String(clip->description);
        payload.I64(clip->duration);
        payload.U8(static_cast<uint8>(clip->defaultWrapMode));
        payload.F32(clip->defaultSpeed);
        payload.I32(clip->metadata.sourceFps);
        payload.I32(clip->metadata.sourceStartFrame);
        payload.I32(clip->metadata.sourceEndFrame);
        payload.U8(static_cast<uint8>(clip->metadata.sourceFormat));
        payload.String(clip->metadata.sourceFile);

        std::vector<std::pair<std::string, std::string>> metadata(
            clip->metadata.customData.begin(), clip->metadata.customData.end());
        std::sort(metadata.begin(), metadata.end());
        payload.U32(static_cast<uint32>(metadata.size()));
        for (const auto& [key, value] : metadata)
        {
            payload.String(key);
            payload.String(value);
        }

        payload.U8(clip->hasRootMotion ? 1u : 0u);
        payload.String(clip->rootMotionBoneName);

        std::vector<const Animation::TransformTrack*> tracks;
        tracks.reserve(clip->transformTracks.size());
        for (const Animation::TransformTrack& track : clip->transformTracks)
            tracks.push_back(&track);
        std::sort(tracks.begin(), tracks.end(),
                  [](const Animation::TransformTrack* left,
                     const Animation::TransformTrack* right)
                  {
                      const uint8 leftType = static_cast<uint8>(left->targetType);
                      const uint8 rightType = static_cast<uint8>(right->targetType);
                      return leftType != rightType
                          ? leftType < rightType
                          : left->targetName < right->targetName;
                  });

        payload.U32(static_cast<uint32>(tracks.size()));
        for (const Animation::TransformTrack* track : tracks)
        {
            payload.String(track->targetName);
            payload.U8(static_cast<uint8>(track->targetType));
            payload.U8(static_cast<uint8>(track->mode));
            WriteKeyframes(payload,
                           track->translationKeyframes,
                           [](ByteWriter& writer, const Vec3& value)
                           {
                               WriteVec3(writer, value);
                           });
            WriteKeyframes(payload,
                           track->rotationKeyframes,
                           [](ByteWriter& writer, const Quat& value)
                           {
                               WriteQuat(writer, value);
                           });
            WriteKeyframes(payload,
                           track->scaleKeyframes,
                           [](ByteWriter& writer, const Vec3& value)
                           {
                               WriteVec3(writer, value);
                           });
        }
        }

        const std::vector<uint8>& payloadBytes = payload.GetBytes();
        if (!payload.IsValid() ||
            payloadBytes.size() > MaxArtifactBytes - HeaderByteCount)
        {
            outError = "Animation artifact exceeds the maximum supported byte count.";
            return false;
        }
        const Hash::SHA256Digest digest =
            Hash::ComputeSHA256(payloadBytes.data(), payloadBytes.size());

        ByteWriter artifactWriter(MaxArtifactBytes);
        artifactWriter.Bytes(
            reinterpret_cast<const uint8*>(RVX_ANIMATION_ARTIFACT_MAGIC), 8);
        artifactWriter.U32(RVX_ANIMATION_ARTIFACT_SCHEMA_VERSION);
        artifactWriter.U64(static_cast<uint64>(payloadBytes.size()));
        artifactWriter.Bytes(digest.data(), digest.size());
        artifactWriter.Bytes(payloadBytes.data(), payloadBytes.size());
        if (!artifactWriter.IsValid())
        {
            outError = "Animation artifact exceeds the maximum supported byte count.";
            return false;
        }
        outBytes = artifactWriter.TakeBytes();
        return true;
    }
    catch (const std::bad_alloc&)
    {
        outBytes.clear();
        outError = "Animation artifact serialization exceeded available memory.";
        return false;
    }
    catch (const std::exception& exception)
    {
        outBytes.clear();
        outError = std::string("Animation artifact serialization failed: ") +
                   exception.what();
        return false;
    }
    catch (...)
    {
        outBytes.clear();
        outError = "Animation artifact serialization failed with an unknown exception.";
        return false;
    }
}

bool DeserializeCookedAnimationArtifact(std::span<const uint8> bytes,
                                        CookedAnimationArtifact& outArtifact,
                                        std::string& outError)
{
    outArtifact = {};
    outError.clear();
    try
    {
        if (bytes.size() < HeaderByteCount || bytes.size() > MaxArtifactBytes ||
        !std::equal(bytes.begin(), bytes.begin() + 8,
                    reinterpret_cast<const uint8*>(RVX_ANIMATION_ARTIFACT_MAGIC)))
    {
        outError = "Animation artifact header or byte count is invalid.";
        return false;
    }

    ByteReader header(bytes.subspan(8));
    uint32 schemaVersion = 0;
    uint64 payloadByteCount = 0;
    if (!header.U32(schemaVersion) || !header.U64(payloadByteCount) ||
        schemaVersion != RVX_ANIMATION_ARTIFACT_SCHEMA_VERSION ||
        payloadByteCount != bytes.size() - HeaderByteCount)
    {
        outError = "Animation artifact schema or payload boundary is invalid.";
        return false;
    }

    const size_t digestOffset = 8u + sizeof(uint32) + sizeof(uint64);
    const std::span<const uint8> expectedDigest =
        bytes.subspan(digestOffset, Hash::RVX_SHA256_DIGEST_SIZE);
    const std::span<const uint8> payloadBytes = bytes.subspan(HeaderByteCount);
    const Hash::SHA256Digest observedDigest =
        Hash::ComputeSHA256(payloadBytes.data(), payloadBytes.size());
    if (!std::equal(expectedDigest.begin(), expectedDigest.end(),
                    observedDigest.begin()))
    {
        outError = "Animation artifact payload SHA-256 does not match its header.";
        return false;
    }

    ByteReader reader(payloadBytes);
    CookedAnimationArtifact candidate;
    if (!reader.String(candidate.sourcePath, false))
    {
        outError = "Animation artifact source identity is invalid.";
        return false;
    }

    uint32 boneCount = 0;
    if (!reader.U32(boneCount) || boneCount == 0 || boneCount > MaxBoneCount ||
        boneCount > reader.Remaining() / MinimumBoneWireBytes)
    {
        outError = "Animation artifact bone count is invalid.";
        return false;
    }
    Animation::Skeleton::Ptr skeleton = Animation::Skeleton::Create();
    skeleton->Reserve(boneCount);
    for (uint32 index = 0; index < boneCount; ++index)
    {
        Animation::Bone bone;
        if (!reader.String(bone.name, false) || !reader.I32(bone.parentIndex) ||
            !ReadVec3(reader, bone.localBindPose.translation) ||
            !ReadQuat(reader, bone.localBindPose.rotation) ||
            !ReadVec3(reader, bone.localBindPose.scale) ||
            !ReadMat4(reader, bone.inverseBindPose) ||
            !reader.F32(bone.boundingRadius))
        {
            outError = "Animation artifact contains a truncated or malformed bone.";
            return false;
        }
        skeleton->AddBone(bone);
    }
    candidate.skeleton = skeleton;

    uint32 clipCount = 0;
    if (!reader.U32(clipCount) || clipCount == 0 || clipCount > MaxClipCount)
    {
        outError = "Animation artifact clip count is invalid.";
        return false;
    }

    std::string previousClipName;
    uint32 totalTracks = 0;
    uint64 totalKeyframes = 0;
    for (uint32 clipIndex = 0; clipIndex < clipCount; ++clipIndex)
    {
        std::string clipName;
        std::string description;
        Animation::TimeUs duration = 0;
        uint8 wrapMode = 0;
        float32 speed = 0.0f;
        int32 sourceFps = 0;
        int32 sourceStartFrame = 0;
        int32 sourceEndFrame = 0;
        uint8 sourceFormat = 0;
        std::string sourceFile;
        if (!reader.String(clipName, false) ||
            (!previousClipName.empty() && clipName <= previousClipName) ||
            !reader.String(description) || !reader.I64(duration) ||
            !reader.U8(wrapMode) || !reader.F32(speed) ||
            !reader.I32(sourceFps) || !reader.I32(sourceStartFrame) ||
            !reader.I32(sourceEndFrame) || !reader.U8(sourceFormat) ||
            !reader.String(sourceFile))
        {
            outError = "Animation artifact clip metadata is malformed or non-canonical.";
            return false;
        }
        previousClipName = clipName;

        auto clip = Animation::AnimationClip::Create(clipName);
        clip->description = std::move(description);
        clip->duration = duration;
        clip->defaultWrapMode = static_cast<Animation::WrapMode>(wrapMode);
        clip->defaultSpeed = speed;
        clip->metadata.sourceFps = sourceFps;
        clip->metadata.sourceStartFrame = sourceStartFrame;
        clip->metadata.sourceEndFrame = sourceEndFrame;
        clip->metadata.sourceFormat =
            static_cast<Animation::AnimationSourceFormat>(sourceFormat);
        clip->metadata.sourceFile = std::move(sourceFile);
        clip->skeleton = skeleton;

        uint32 metadataCount = 0;
        if (!reader.U32(metadataCount) || metadataCount > MaxMetadataEntries ||
            metadataCount > reader.Remaining() / MinimumMetadataEntryWireBytes)
        {
            outError = "Animation artifact metadata count is invalid.";
            return false;
        }
        std::string previousMetadataKey;
        for (uint32 metadataIndex = 0; metadataIndex < metadataCount; ++metadataIndex)
        {
            std::string key;
            std::string value;
            if (!reader.String(key, false) ||
                (!previousMetadataKey.empty() && key <= previousMetadataKey) ||
                !reader.String(value))
            {
                outError = "Animation artifact metadata is malformed or non-canonical.";
                return false;
            }
            previousMetadataKey = key;
            clip->metadata.customData.emplace(std::move(key), std::move(value));
        }

        uint8 hasRootMotion = 0;
        if (!reader.U8(hasRootMotion) || hasRootMotion > 1 ||
            !reader.String(clip->rootMotionBoneName))
        {
            outError = "Animation artifact root-motion metadata is malformed.";
            return false;
        }
        clip->hasRootMotion = hasRootMotion != 0;

        uint32 trackCount = 0;
        if (!reader.U32(trackCount) || trackCount == 0 ||
            trackCount > MaxTracksPerClip ||
            trackCount > MaxTotalTracks - totalTracks ||
            trackCount > reader.Remaining() / MinimumTransformTrackWireBytes)
        {
            outError = "Animation artifact transform-track count is invalid.";
            return false;
        }
        totalTracks += trackCount;
        std::pair<uint8, std::string> previousTarget{};
        bool hasPreviousTarget = false;
        clip->transformTracks.reserve(trackCount);
        for (uint32 trackIndex = 0; trackIndex < trackCount; ++trackIndex)
        {
            Animation::TransformTrack track;
            uint8 targetType = 0;
            uint8 transformMode = 0;
            if (!reader.String(track.targetName, false) ||
                !reader.U8(targetType) || !reader.U8(transformMode))
            {
                outError = "Animation artifact transform target is malformed.";
                return false;
            }
            const std::pair<uint8, std::string> target{targetType, track.targetName};
            if ((hasPreviousTarget && target <= previousTarget) ||
                targetType != static_cast<uint8>(Animation::TrackTargetType::Bone) ||
                transformMode != static_cast<uint8>(Animation::TransformMode::TRS))
            {
                outError = "Animation artifact transform targets are non-canonical or unsupported.";
                return false;
            }
            previousTarget = target;
            hasPreviousTarget = true;
            track.targetType = static_cast<Animation::TrackTargetType>(targetType);
            track.mode = static_cast<Animation::TransformMode>(transformMode);

            if (!ReadKeyframes(reader,
                               track.translationKeyframes,
                               totalKeyframes,
                               Vec3KeyframeWireBytes,
                               [](ByteReader& source, Vec3& value)
                               {
                                   return ReadVec3(source, value);
                               }) ||
                !ReadKeyframes(reader,
                               track.rotationKeyframes,
                               totalKeyframes,
                               QuatKeyframeWireBytes,
                               [](ByteReader& source, Quat& value)
                               {
                                   return ReadQuat(source, value);
                               }) ||
                !ReadKeyframes(reader,
                               track.scaleKeyframes,
                               totalKeyframes,
                               Vec3KeyframeWireBytes,
                               [](ByteReader& source, Vec3& value)
                               {
                                   return ReadVec3(source, value);
                               }))
            {
                outError = "Animation artifact keyframe channel is malformed.";
                return false;
            }
            clip->transformTracks.push_back(std::move(track));
        }

        candidate.clips.emplace(clipName, std::move(clip));
    }

    if (!reader.AtEnd())
    {
        outError = "Animation artifact contains trailing payload bytes.";
        return false;
    }
    if (!ValidateArtifact(candidate, outError))
        return false;

    outArtifact = std::move(candidate);
    return true;
    }
    catch (const std::bad_alloc&)
    {
        outArtifact = {};
        outError = "Animation artifact parsing exceeded available memory.";
        return false;
    }
    catch (const std::exception& exception)
    {
        outArtifact = {};
        outError = std::string("Animation artifact parsing failed: ") +
                   exception.what();
        return false;
    }
    catch (...)
    {
        outArtifact = {};
        outError = "Animation artifact parsing failed with an unknown exception.";
        return false;
    }
}
} // namespace RVX::Resource
