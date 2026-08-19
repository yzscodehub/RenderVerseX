#include "Core/Hash/SHA256.h"
#include "Resource/CookManifest.h"
#include "Resource/Cooked/CookedAnimationArtifact.h"
#include "Tools/AnimationRootMotionDeriver.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace RVX;

namespace
{
    namespace fs = std::filesystem;
    using RVX::Animation::AnimationClip;
    using RVX::Animation::KeyframeQuat;
    using RVX::Animation::KeyframeVec3;
    using RVX::Animation::TransformTrack;
    using RVX::Resource::CookedAnimationArtifact;
    using RVX::Tools::AnimationRootMotionDeriveRequest;

    class TemporaryDirectory final
    {
    public:
        explicit TemporaryDirectory(const char* label)
        {
            const auto token = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
            m_path = fs::temp_directory_path() /
                     (std::string("rvx_root_motion_derivation_") + label +
                      "_" + std::to_string(token));
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

    fs::path ParentArtifactPath()
    {
        return fs::path(RVX_SOURCE_DIR) / "Samples" / "RenderVerseSamples" /
               "Assets" / "cooked" / "casual-female" / "models" /
               "casual-female" / "Casual_Female.rvdeps" / "animation.rvxanim";
    }

    fs::path AssetsRoot()
    {
        return fs::path(RVX_SOURCE_DIR) / "Samples" / "RenderVerseSamples" / "Assets";
    }

    fs::path ParentManifestPath()
    {
        return AssetsRoot() / "cooked" / "casual-female" / "CookManifest.rvxmanifest";
    }

    std::vector<uint8> ReadBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(input.is_open()) << path;
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void WriteBytes(const fs::path& path, const std::vector<uint8>& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open()) << path;
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good()) << path;
    }

    const TransformTrack& RequireTrack(const AnimationClip& clip,
                                       const char* targetName)
    {
        const TransformTrack* track = clip.FindTransformTrack(targetName);
        EXPECT_NE(track, nullptr) << targetName;
        return *track;
    }

    void ExpectVec3KeyframesEqual(const std::vector<KeyframeVec3>& expected,
                                  const std::vector<KeyframeVec3>& actual)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t index = 0; index < expected.size(); ++index)
        {
            EXPECT_EQ(actual[index].time, expected[index].time) << index;
            EXPECT_EQ(actual[index].interpolation, expected[index].interpolation) << index;
            EXPECT_EQ(actual[index].value.x, expected[index].value.x) << index;
            EXPECT_EQ(actual[index].value.y, expected[index].value.y) << index;
            EXPECT_EQ(actual[index].value.z, expected[index].value.z) << index;
        }
    }

    void ExpectQuatKeyframesEqual(const std::vector<KeyframeQuat>& expected,
                                  const std::vector<KeyframeQuat>& actual)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t index = 0; index < expected.size(); ++index)
        {
            EXPECT_EQ(actual[index].time, expected[index].time) << index;
            EXPECT_EQ(actual[index].interpolation, expected[index].interpolation) << index;
            EXPECT_EQ(actual[index].value.w, expected[index].value.w) << index;
            EXPECT_EQ(actual[index].value.x, expected[index].value.x) << index;
            EXPECT_EQ(actual[index].value.y, expected[index].value.y) << index;
            EXPECT_EQ(actual[index].value.z, expected[index].value.z) << index;
        }
    }

    void ExpectTracksEqual(const TransformTrack& expected, const TransformTrack& actual)
    {
        EXPECT_EQ(actual.targetName, expected.targetName);
        EXPECT_EQ(actual.targetType, expected.targetType);
        EXPECT_EQ(actual.mode, expected.mode);
        ExpectVec3KeyframesEqual(expected.translationKeyframes, actual.translationKeyframes);
        ExpectQuatKeyframesEqual(expected.rotationKeyframes, actual.rotationKeyframes);
        ExpectVec3KeyframesEqual(expected.scaleKeyframes, actual.scaleKeyframes);
        EXPECT_EQ(actual.matrixKeyframes.size(), expected.matrixKeyframes.size());
    }

    CookedAnimationArtifact ReadArtifact(const fs::path& path)
    {
        CookedAnimationArtifact artifact;
        std::string error;
        EXPECT_TRUE(RVX::Resource::DeserializeCookedAnimationArtifact(
            ReadBytes(path), artifact, error)) << error;
        return artifact;
    }

    AnimationRootMotionDeriveRequest MakeRequest(const fs::path& input,
                                                 const fs::path& output)
    {
        AnimationRootMotionDeriveRequest request;
        request.inputPath = input;
        request.outputPath = output;
        return request;
    }

    RVX::Tools::AnimationRootMotionPackagePublishRequest MakePackageRequest(
        const fs::path& sourceRoot,
        const fs::path& input,
        const fs::path& packagePath)
    {
        RVX::Tools::AnimationRootMotionPackagePublishRequest request;
        request.sourceRoot = sourceRoot;
        request.packagePath = packagePath;
        request.derivation.inputPath = input;
        return request;
    }

    std::string HashText(const std::string& value)
    {
        return RVX::Hash::FormatSHA256Digest(
            RVX::Hash::ComputeSHA256(value.data(), value.size()));
    }

    std::string BuildExpectedPackageCookSettings(
        const RVX::Tools::AnimationRootMotionDeriveResult& derivation)
    {
        return std::string("schema=RVX_ANIMATION_ROOT_MOTION_PACKAGE_V1\n") +
               "deriver.name=RVXAnimationDerive\n" +
               "deriver.tool.version=" +
                   RVX::Tools::RVX_ANIMATION_ROOT_MOTION_DERIVE_TOOL_VERSION + "\n" +
               "deriver.recipe.sha256=" + derivation.recipeSha256 + "\n" +
               "parent.asset.id=casual-female\n" +
               "parent.asset.content.id=" +
                   std::string(RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_CONTENT_ID) + "\n" +
               "parent.animation.byteCount=927584\n" +
               "parent.animation.sha256=" +
                   std::string(RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256) + "\n" +
               "source.clip=Walk\n"
               "output.clip=casual-female-walk-root-motion\n"
               "root.bone=CharacterArmature/Bone/Body/Hips\n"
               "translation.axis=+Z\n"
               "translation.speed=1.0\n"
               "duration.us=1250000\n";
    }

    std::string BuildExpectedPackageManifestRecipe(
        const RVX::Tools::AnimationRootMotionDeriveResult& derivation)
    {
        const std::string settingsHash = HashText(BuildExpectedPackageCookSettings(derivation));
        return std::string("recipeSchema=RVX_COOK_RECIPE_V1\n") +
               "toolName=RVXCook\n"
               "toolVersion=2.0.0\n"
               "importer=RVXAnimationDerive\n"
               "assetType=Animation\n"
               "output.path=" +
                   std::string(RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH) + "\n" +
               "source.path=" +
                   std::string(RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SOURCE_PATH) + "\n" +
               "source.byteCount=927584\n"
               "source.sha256=" +
                   std::string(RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256) + "\n" +
               "sourceDependencyCount=0\n"
               "cookSettingsHash=" + settingsHash + "\n"
               "dependencyCount=0\n";
    }

    RVX::Resource::ResourceContentIdentity MakeSourceIdentity()
    {
        RVX::Resource::ResourceContentIdentity identity;
        identity.schemaVersion = RVX::Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = RVX::Resource::ResourceContentIdentityDomain::Source;
        identity.scope = RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = RVX::Resource::ResourceContentHashAlgorithm::SHA256;
        identity.digest = RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256;
        identity.byteCount = RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_BYTE_COUNT;
        identity.fileCount = 1;
        return identity;
    }

    void ExpectPublishedPackage(
        const fs::path& packagePath,
        const RVX::Tools::AnimationRootMotionPackagePublishResult& result)
    {
        const fs::path artifactPath = packagePath /
            RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH;
        const fs::path manifestPath = packagePath /
            RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_MANIFEST_PATH;
        ASSERT_TRUE(fs::is_regular_file(artifactPath));
        ASSERT_TRUE(fs::is_regular_file(manifestPath));
        size_t fileCount = 0;
        for (const fs::directory_entry& entry : fs::directory_iterator(packagePath))
        {
            EXPECT_TRUE(entry.is_regular_file());
            ++fileCount;
        }
        EXPECT_EQ(fileCount, 2u);

        RVX::Resource::CookManifest manifest;
        std::string error;
        ASSERT_TRUE(RVX::Resource::LoadCookManifest(manifestPath, manifest, error)) << error;
        EXPECT_EQ(manifest.schema, RVX::Resource::CookManifest::SchemaName);
        EXPECT_EQ(manifest.version, RVX::Resource::RVX_COOK_MANIFEST_SCHEMA_VERSION);
        EXPECT_EQ(manifest.toolName, RVX::Resource::CookManifest::DefaultToolName);
        EXPECT_EQ(manifest.toolVersion, RVX::Resource::CookManifest::DefaultToolVersion);
        ASSERT_EQ(manifest.entries.size(), 1u);
        const RVX::Resource::CookManifestEntry& entry = manifest.entries.front();
        EXPECT_TRUE(entry.success);
        EXPECT_EQ(entry.sourcePath,
                  RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SOURCE_PATH);
        EXPECT_EQ(entry.outputPath,
                  RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH);
        EXPECT_EQ(entry.type, RVX::Resource::CookAssetType::Animation);
        EXPECT_EQ(entry.importerName, "RVXAnimationDerive");
        EXPECT_EQ(entry.sourceContent.byteCount,
                  RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_BYTE_COUNT);
        EXPECT_EQ(entry.sourceContent.sha256,
                  RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256);
        EXPECT_TRUE(entry.sourceDependencies.empty());
        EXPECT_TRUE(entry.dependencies.empty());
        ASSERT_EQ(entry.artifacts.size(), 1u);
        EXPECT_EQ(entry.artifacts.front().relativePath,
                  RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH);
        EXPECT_EQ(entry.artifacts.front().byteCount, result.artifactByteCount);
        EXPECT_EQ(entry.artifacts.front().sha256, result.artifactSha256);
        EXPECT_EQ(entry.canonicalCookSettings,
                  BuildExpectedPackageCookSettings(result.derivation));
        EXPECT_EQ(entry.cookSettingsHash, HashText(entry.canonicalCookSettings));
        EXPECT_EQ(entry.cookSettingsHash, result.cookSettingsHash);
        EXPECT_EQ(entry.recipeHash,
                  HashText(BuildExpectedPackageManifestRecipe(result.derivation)));
        EXPECT_EQ(entry.recipeHash, result.manifestRecipeHash);

        RVX::Resource::ResourceContentIdentity cookedIdentity;
        ASSERT_TRUE(RVX::Resource::ComputeCookedContentIdentity(
            manifest, cookedIdentity, error)) << error;
        EXPECT_EQ(cookedIdentity.digest, result.cookedContentSha256);
        EXPECT_EQ(manifest.manifestContentIdentity.digest, result.manifestContentSha256);

        RVX::Resource::CookManifestExpectation expectation;
        expectation.selector.sourcePath =
            RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SOURCE_PATH;
        expectation.selector.outputPath =
            RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH;
        expectation.selector.type = RVX::Resource::CookAssetType::Animation;
        expectation.expectedSourceContentIdentity = MakeSourceIdentity();
        expectation.expectedCookedContentIdentity = cookedIdentity;
        expectation.expectedManifestContentIdentity = manifest.manifestContentIdentity;
        expectation.expectedCookSettingsHash = entry.cookSettingsHash;
        expectation.expectedRecipeHash = entry.recipeHash;
        EXPECT_TRUE(RVX::Resource::VerifyCookedAssetAdmission(
            manifest, AssetsRoot(), packagePath, expectation).IsAccepted());
    }

    TEST(AnimationRootMotionDerivationValidation,
         ProducesAnIsolatedCanonicalClipWithoutTempPaths)
    {
        ASSERT_TRUE(fs::is_regular_file(ParentArtifactPath()));
        const CookedAnimationArtifact source = ReadArtifact(ParentArtifactPath());
        ASSERT_TRUE(source.skeleton);
        ASSERT_EQ(source.skeleton->GetBoneCount(), 23u);
        const auto sourceWalk = source.clips.find(RVX::Tools::RVX_ROOT_MOTION_DERIVE_SOURCE_CLIP);
        ASSERT_NE(sourceWalk, source.clips.end());
        ASSERT_TRUE(sourceWalk->second);

        TemporaryDirectory firstRoot("first");
        TemporaryDirectory secondRoot("second");
        const fs::path firstInput = firstRoot.Get() / "input.rvxanim";
        const fs::path secondInput = secondRoot.Get() / "input.rvxanim";
        const fs::path firstOutput = firstRoot.Get() / "derived.rvxanim";
        const fs::path secondOutput = secondRoot.Get() / "derived.rvxanim";
        const std::vector<uint8> sourceBytes = ReadBytes(ParentArtifactPath());
        WriteBytes(firstInput, sourceBytes);
        WriteBytes(secondInput, sourceBytes);

        RVX::Tools::AnimationRootMotionDeriveResult firstResult;
        RVX::Tools::AnimationRootMotionDeriveResult secondResult;
        std::string error;
        ASSERT_TRUE(RVX::Tools::DeriveAnimationRootMotion(
            MakeRequest(firstInput, firstOutput), firstResult, error)) << error;
        ASSERT_TRUE(RVX::Tools::DeriveAnimationRootMotion(
            MakeRequest(secondInput, secondOutput), secondResult, error)) << error;
        const std::vector<uint8> firstBytes = ReadBytes(firstOutput);
        EXPECT_EQ(firstBytes, ReadBytes(secondOutput));
        EXPECT_EQ(sourceBytes.size(),
                  RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_BYTE_COUNT);
        EXPECT_EQ(firstResult.inputSha256,
                  RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256);
        EXPECT_EQ(firstResult.recipe, secondResult.recipe);
        EXPECT_EQ(firstResult.recipeSha256, secondResult.recipeSha256);

        const std::string payload(firstBytes.begin(), firstBytes.end());
        EXPECT_EQ(payload.find(firstRoot.Get().generic_string()), std::string::npos);
        EXPECT_EQ(payload.find(secondRoot.Get().generic_string()), std::string::npos);

        const CookedAnimationArtifact output = ReadArtifact(firstOutput);
        ASSERT_TRUE(output.skeleton);
        EXPECT_EQ(output.skeleton->GetBoneCount(), 23u);
        ASSERT_EQ(output.clips.size(), 1u);
        const auto derivedEntry = output.clips.find(
            RVX::Tools::RVX_ROOT_MOTION_DERIVE_OUTPUT_CLIP);
        ASSERT_NE(derivedEntry, output.clips.end());
        const AnimationClip& derived = *derivedEntry->second;
        const AnimationClip& sourceClip = *sourceWalk->second;
        EXPECT_EQ(derived.duration, RVX::Tools::RVX_ROOT_MOTION_DERIVE_DURATION_US);
        EXPECT_TRUE(derived.hasRootMotion);
        EXPECT_EQ(derived.rootMotionBoneName, RVX::Tools::RVX_ROOT_MOTION_DERIVE_ROOT_BONE);

        const TransformTrack& sourceRoot = RequireTrack(
            sourceClip, RVX::Tools::RVX_ROOT_MOTION_DERIVE_ROOT_BONE);
        const TransformTrack& derivedRoot = RequireTrack(
            derived, RVX::Tools::RVX_ROOT_MOTION_DERIVE_ROOT_BONE);
        ASSERT_EQ(derivedRoot.translationKeyframes.size(),
                  sourceRoot.translationKeyframes.size());
        for (size_t index = 0; index < sourceRoot.translationKeyframes.size(); ++index)
        {
            const KeyframeVec3& sourceKey = sourceRoot.translationKeyframes[index];
            const KeyframeVec3& derivedKey = derivedRoot.translationKeyframes[index];
            EXPECT_EQ(derivedKey.time, sourceKey.time) << index;
            EXPECT_EQ(derivedKey.interpolation, sourceKey.interpolation) << index;
            EXPECT_EQ(derivedKey.value.x, 0.0f) << index;
            EXPECT_EQ(derivedKey.value.y, sourceKey.value.y) << index;
            EXPECT_EQ(derivedKey.value.z,
                      static_cast<float32>(static_cast<float64>(derivedKey.time) /
                                           1'000'000.0 * 1.0)) << index;
        }
        ExpectQuatKeyframesEqual(sourceRoot.rotationKeyframes,
                                 derivedRoot.rotationKeyframes);
        ExpectVec3KeyframesEqual(sourceRoot.scaleKeyframes,
                                 derivedRoot.scaleKeyframes);
        for (const TransformTrack& sourceTrack : sourceClip.transformTracks)
        {
            if (sourceTrack.targetName != RVX::Tools::RVX_ROOT_MOTION_DERIVE_ROOT_BONE)
                ExpectTracksEqual(sourceTrack, RequireTrack(derived, sourceTrack.targetName.c_str()));
        }

        const auto& metadata = derived.metadata.customData;
        EXPECT_EQ(metadata.at("rvx.root-motion.recipe"), firstResult.recipe);
        EXPECT_EQ(metadata.at("rvx.root-motion.recipe.version"),
                  "rvx-animation-root-motion-derive-v1");
        EXPECT_EQ(metadata.at("rvx.root-motion.recipe.sha256"), firstResult.recipeSha256);
        EXPECT_EQ(metadata.at("rvx.root-motion.parent.asset.id"), "casual-female");
        EXPECT_EQ(metadata.at("rvx.root-motion.parent.asset.content.id"),
                  RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_CONTENT_ID);
        EXPECT_EQ(metadata.at("rvx.root-motion.input.sha256"), firstResult.inputSha256);
        EXPECT_EQ(metadata.at("rvx.root-motion.source.clip"), "Walk");
        EXPECT_EQ(metadata.at("rvx.root-motion.axis"), "+Z");
        EXPECT_EQ(metadata.at("rvx.root-motion.speed"), "1.0");
        EXPECT_EQ(metadata.size(), 9u);

        std::vector<uint8> roundTrip;
        ASSERT_TRUE(RVX::Resource::SerializeCookedAnimationArtifact(output, roundTrip, error))
            << error;
        EXPECT_EQ(roundTrip, firstBytes);
    }

    TEST(AnimationRootMotionDerivationValidation,
         RejectsInvalidPolicyWithoutReplacingExistingOutput)
    {
        TemporaryDirectory temporary("failure");
        const fs::path input = temporary.Get() / "input.rvxanim";
        const fs::path output = temporary.Get() / "existing.rvxanim";
        WriteBytes(input, ReadBytes(ParentArtifactPath()));
        const std::vector<uint8> sentinel = {0x8a, 0x4f, 0x31, 0xc2};
        WriteBytes(output, sentinel);

        const auto expectRejected = [&](const AnimationRootMotionDeriveRequest& request)
        {
            RVX::Tools::AnimationRootMotionDeriveResult result;
            std::string error;
            EXPECT_FALSE(RVX::Tools::DeriveAnimationRootMotion(request, result, error));
            EXPECT_FALSE(error.empty());
            EXPECT_EQ(ReadBytes(output), sentinel);
        };

        auto request = MakeRequest(input, output);
        request.parentAssetId = "not-casual-female";
        expectRejected(request);
        request = MakeRequest(input, output);
        request.duration = 1;
        expectRejected(request);
        request = MakeRequest(input, output);
        request.rootBone = "OtherRoot";
        expectRejected(request);
        request = MakeRequest(input, output);
        request.sourceClip = "Run";
        expectRejected(request);
        request = MakeRequest(input, output);
        request.speed = 2.0f;
        expectRejected(request);

        std::error_code error;
        for (const fs::directory_entry& entry : fs::directory_iterator(temporary.Get(), error))
        {
            EXPECT_EQ(entry.path().filename().string().find(".rvx-root-motion-staging-"),
                      std::string::npos);
        }
        EXPECT_FALSE(error);
    }

    TEST(AnimationRootMotionDerivationValidation,
         RejectsTamperedParentArtifactBeforePublication)
    {
        TemporaryDirectory temporary("tampered");
        const fs::path input = temporary.Get() / "input.rvxanim";
        const fs::path output = temporary.Get() / "existing.rvxanim";
        std::vector<uint8> tampered = ReadBytes(ParentArtifactPath());
        ASSERT_EQ(tampered.size(),
                  RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_BYTE_COUNT);
        tampered.back() ^= 0x1u;
        WriteBytes(input, tampered);
        const std::vector<uint8> sentinel = {0x64, 0x01, 0xca, 0xfe};
        WriteBytes(output, sentinel);

        RVX::Tools::AnimationRootMotionDeriveResult result;
        std::string error;
        EXPECT_FALSE(RVX::Tools::DeriveAnimationRootMotion(
            MakeRequest(input, output), result, error));
        EXPECT_EQ(error,
                  "Root-motion input does not match the approved parent animation artifact.");
        EXPECT_EQ(ReadBytes(output), sentinel);

        std::error_code directoryError;
        for (const fs::directory_entry& entry : fs::directory_iterator(
                 temporary.Get(), directoryError))
        {
            EXPECT_EQ(entry.path().filename().string().find(".rvx-root-motion-staging-"),
                      std::string::npos);
        }
        EXPECT_FALSE(directoryError);
    }

    TEST(AnimationRootMotionDerivationValidation,
         PublishesDeterministicValidatedChildPackageWithoutChangingParent)
    {
        const std::vector<uint8> parentAnimationBefore = ReadBytes(ParentArtifactPath());
        const std::vector<uint8> parentManifestBefore = ReadBytes(ParentManifestPath());
        TemporaryDirectory firstRoot("package_first");
        TemporaryDirectory secondRoot("package_second");
        const fs::path firstPackage = firstRoot.Get() / "child-package";
        const fs::path secondPackage = secondRoot.Get() / "child-package";
        fs::create_directories(firstPackage);
        const fs::path replacedSentinel = firstPackage / "old-package.bin";
        WriteBytes(replacedSentinel, {0x7f, 0x12, 0xa4, 0x09});

        RVX::Tools::AnimationRootMotionPackagePublishResult firstResult;
        RVX::Tools::AnimationRootMotionPackagePublishResult secondResult;
        std::string error;
        ASSERT_TRUE(RVX::Tools::PublishAnimationRootMotionPackage(
            MakePackageRequest(AssetsRoot(), ParentArtifactPath(), firstPackage),
            firstResult, error)) << error;
        ASSERT_TRUE(RVX::Tools::PublishAnimationRootMotionPackage(
            MakePackageRequest(AssetsRoot(), ParentArtifactPath(), secondPackage),
            secondResult, error)) << error;

        ExpectPublishedPackage(firstPackage, firstResult);
        ExpectPublishedPackage(secondPackage, secondResult);
        EXPECT_FALSE(fs::exists(replacedSentinel));
        EXPECT_EQ(ReadBytes(firstPackage /
                            RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH),
                  ReadBytes(secondPackage /
                            RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH));
        const std::vector<uint8> firstManifest = ReadBytes(firstPackage /
            RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_MANIFEST_PATH);
        EXPECT_EQ(firstManifest, ReadBytes(secondPackage /
            RVX::Tools::RVX_ROOT_MOTION_DERIVE_PACKAGE_MANIFEST_PATH));
        EXPECT_EQ(firstResult.artifactSha256, secondResult.artifactSha256);
        EXPECT_EQ(firstResult.cookSettingsHash, secondResult.cookSettingsHash);
        EXPECT_EQ(firstResult.manifestRecipeHash, secondResult.manifestRecipeHash);
        EXPECT_EQ(firstResult.cookedContentSha256, secondResult.cookedContentSha256);
        EXPECT_EQ(firstResult.manifestContentSha256, secondResult.manifestContentSha256);

        const std::string manifestText(firstManifest.begin(), firstManifest.end());
        EXPECT_EQ(manifestText.find(firstRoot.Get().string()), std::string::npos);
        EXPECT_EQ(manifestText.find(secondRoot.Get().string()), std::string::npos);
        EXPECT_EQ(manifestText.find(AssetsRoot().string()), std::string::npos);
        EXPECT_EQ(ReadBytes(ParentArtifactPath()), parentAnimationBefore);
        EXPECT_EQ(ReadBytes(ParentManifestPath()), parentManifestBefore);
    }

    TEST(AnimationRootMotionDerivationValidation,
         TamperedPackageInputPreservesExistingPackageAndCleansTransactions)
    {
        TemporaryDirectory temporary("package_tampered");
        const fs::path sourceRoot = temporary.Get() / "assets";
        const fs::path parentPath = sourceRoot /
            RVX::Tools::RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SOURCE_PATH;
        fs::create_directories(parentPath.parent_path());
        std::vector<uint8> tampered = ReadBytes(ParentArtifactPath());
        tampered.back() ^= 0x1u;
        WriteBytes(parentPath, tampered);

        const fs::path packagePath = temporary.Get() / "existing-package";
        fs::create_directories(packagePath);
        const fs::path sentinelPath = packagePath / "sentinel.bin";
        const std::vector<uint8> sentinel = {0x13, 0x37, 0xc0, 0xde};
        WriteBytes(sentinelPath, sentinel);

        RVX::Tools::AnimationRootMotionPackagePublishResult result;
        std::string error;
        EXPECT_FALSE(RVX::Tools::PublishAnimationRootMotionPackage(
            MakePackageRequest(sourceRoot, parentPath, packagePath), result, error));
        EXPECT_EQ(error,
                  "Root-motion input does not match the approved parent animation artifact.");
        EXPECT_EQ(ReadBytes(sentinelPath), sentinel);
        size_t packageFileCount = 0;
        for (const fs::directory_entry& entry : fs::directory_iterator(packagePath))
        {
            EXPECT_EQ(entry.path().filename(), "sentinel.bin");
            ++packageFileCount;
        }
        EXPECT_EQ(packageFileCount, 1u);

        std::error_code directoryError;
        for (const fs::directory_entry& entry : fs::directory_iterator(
                 temporary.Get(), directoryError))
        {
            EXPECT_EQ(entry.path().filename().string().find(
                          ".rvx-root-motion-package-staging-"),
                      std::string::npos);
            EXPECT_EQ(entry.path().filename().string().find(
                          ".rvx-root-motion-package-backup-"),
                      std::string::npos);
        }
        EXPECT_FALSE(directoryError);
    }
} // namespace
