/** @file AnimationRootMotionDeriver.cpp  @brief Deterministic .rvxanim root-motion derivation. */

#include "Tools/AnimationRootMotionDeriver.h"

#include "Core/Hash/SHA256.h"
#include "Resource/CookManifest.h"
#include "Resource/Cooked/CookedAnimationArtifact.h"
#include "Tools/AssetPipeline.h"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(RVX_PLATFORM_WINDOWS)
#include <windows.h>
#endif

namespace RVX::Tools
{
    namespace
    {
        constexpr uint64 MaxInputBytes = 256ull * 1024ull * 1024ull;
        constexpr char RecipeVersion[] = "rvx-animation-root-motion-derive-v1";
        constexpr char MetadataRecipe[] = "rvx.root-motion.recipe";
        constexpr char MetadataRecipeVersion[] = "rvx.root-motion.recipe.version";
        constexpr char MetadataRecipeHash[] = "rvx.root-motion.recipe.sha256";
        constexpr char MetadataParentAssetId[] = "rvx.root-motion.parent.asset.id";
        constexpr char MetadataParentContentId[] = "rvx.root-motion.parent.asset.content.id";
        constexpr char MetadataInputHash[] = "rvx.root-motion.input.sha256";
        constexpr char MetadataSourceClip[] = "rvx.root-motion.source.clip";
        constexpr char MetadataAxis[] = "rvx.root-motion.axis";
        constexpr char MetadataSpeed[] = "rvx.root-motion.speed";

        std::atomic<uint64> s_stagingSequence{0};
        std::atomic<uint64> s_packageSequence{0};

        bool IsApprovedRequest(const AnimationRootMotionDeriveRequest& request,
                               std::string& outError)
        {
            if (request.inputPath.empty() || request.outputPath.empty())
            {
                outError = "Root-motion derivation requires input and output paths.";
                return false;
            }
            if (request.sourceClip != RVX_ROOT_MOTION_DERIVE_SOURCE_CLIP ||
                request.outputClip != RVX_ROOT_MOTION_DERIVE_OUTPUT_CLIP ||
                request.rootBone != RVX_ROOT_MOTION_DERIVE_ROOT_BONE ||
                request.axis != AnimationRootMotionDeriveAxis::PositiveZ ||
                request.speed != 1.0f ||
                request.duration != RVX_ROOT_MOTION_DERIVE_DURATION_US ||
                request.parentAssetId != RVX_ROOT_MOTION_DERIVE_PARENT_ASSET_ID ||
                FormatAnimationRootMotionParentContentId(request.parentContentId) !=
                    RVX_ROOT_MOTION_DERIVE_PARENT_CONTENT_ID)
            {
                outError = "Root-motion derivation request does not match the approved policy.";
                return false;
            }
            return true;
        }

        bool ReadBoundedFile(const fs::path& path,
                             std::vector<uint8>& outBytes,
                             std::string& outError)
        {
            std::error_code error;
            if (!fs::is_regular_file(path, error) || error)
            {
                outError = "Root-motion input must be a regular file.";
                return false;
            }

            const uintmax_t declaredSize = fs::file_size(path, error);
            if (error || declaredSize > MaxInputBytes)
            {
                outError = "Root-motion input exceeds the 256 MiB read limit.";
                return false;
            }

            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                outError = "Failed to open root-motion input.";
                return false;
            }

            outBytes.clear();
            outBytes.reserve(static_cast<size_t>(declaredSize));
            std::array<char, 64 * 1024> buffer{};
            while (input)
            {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize count = input.gcount();
                if (count <= 0)
                    break;
                if (static_cast<uint64>(count) > MaxInputBytes - outBytes.size())
                {
                    outError = "Root-motion input exceeds the 256 MiB read limit.";
                    outBytes.clear();
                    return false;
                }
                const auto* first = reinterpret_cast<const uint8*>(buffer.data());
                outBytes.insert(outBytes.end(), first, first + count);
            }
            if (!input.eof())
            {
                outError = "Failed while reading root-motion input.";
                outBytes.clear();
                return false;
            }
            return true;
        }

        bool ValidateOutputPath(const fs::path& inputPath,
                                const fs::path& outputPath,
                                std::string& outError)
        {
            std::error_code error;
            const fs::path parent = outputPath.has_parent_path()
                ? outputPath.parent_path()
                : fs::current_path(error);
            if (error || !fs::is_directory(parent, error) || error)
            {
                outError = "Root-motion output parent directory must already exist.";
                return false;
            }
            const bool outputExists = fs::exists(outputPath, error);
            if (error)
            {
                outError = "Failed to query root-motion output path.";
                return false;
            }
            if (outputExists && fs::is_directory(outputPath, error))
            {
                outError = "Root-motion output must not be a directory.";
                return false;
            }
            if (error)
            {
                outError = "Failed to query root-motion output type.";
                return false;
            }

            const fs::path normalizedInput = fs::absolute(inputPath, error).lexically_normal();
            if (error)
            {
                outError = "Failed to resolve root-motion input path.";
                return false;
            }
            const fs::path normalizedOutput = fs::absolute(outputPath, error).lexically_normal();
            if (error)
            {
                outError = "Failed to resolve root-motion output path.";
                return false;
            }
            if (normalizedInput == normalizedOutput)
            {
                outError = "Root-motion output must be distinct from its input.";
                return false;
            }
            return true;
        }

        bool CreateStagingPath(const fs::path& outputPath,
                               fs::path& outStagingPath,
                               std::string& outError)
        {
            for (uint32 attempt = 0; attempt < 128; ++attempt)
            {
                const uint64 sequence = s_stagingSequence.fetch_add(1,
                                                                       std::memory_order_relaxed);
                const uint64 ticks = static_cast<uint64>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
                const fs::path candidate = outputPath.parent_path() /
                    (".rvx-root-motion-artifact-" + std::to_string(ticks) + "-" +
                     std::to_string(sequence));
                std::error_code error;
                if (!fs::exists(candidate, error) && !error)
                {
                    outStagingPath = candidate;
                    return true;
                }
            }
            outError = "Failed to reserve a unique root-motion staging path.";
            return false;
        }

        void RemoveStagingFile(const fs::path& path)
        {
            if (path.empty())
                return;
            std::error_code error;
            fs::remove(path, error);
        }

        bool WriteStagingFile(const fs::path& path,
                              const std::vector<uint8>& bytes,
                              std::string& outError)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                outError = "Failed to create root-motion staging file.";
                return false;
            }
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output)
            {
                outError = "Failed to finalize root-motion staging file.";
                return false;
            }
            return true;
        }

        bool ReplaceOutputAtomically(const fs::path& stagingPath,
                                     const fs::path& outputPath,
                                     std::string& outError)
        {
#if defined(RVX_PLATFORM_WINDOWS)
            if (!MoveFileExW(stagingPath.c_str(),
                             outputPath.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                outError = "Failed to atomically replace root-motion output.";
                return false;
            }
            return true;
#else
            std::error_code error;
            fs::rename(stagingPath, outputPath, error);
            if (error)
            {
                outError = "Failed to atomically replace root-motion output: " +
                           error.message();
                return false;
            }
            return true;
#endif
        }

        std::string BuildRecipe(const AnimationRootMotionDeriveRequest& request,
                                const std::string& inputSha256)
        {
            return std::string(RecipeVersion) + "\n" +
                   "parent.asset.id=" + request.parentAssetId + "\n" +
                   "parent.asset.content.id=" +
                       FormatAnimationRootMotionParentContentId(request.parentContentId) + "\n" +
                   "input.sha256=" + inputSha256 + "\n" +
                   "source.clip=" + request.sourceClip + "\n" +
                   "output.clip=" + request.outputClip + "\n" +
                   "root.bone=" + request.rootBone + "\n" +
                   "translation.axis=+Z\n" +
                   "translation.speed=1.0\n" +
                   "duration.us=1250000\n";
        }

        bool MakeAbsolutePath(const fs::path& path,
                              fs::path& outPath,
                              std::string& outError)
        {
            std::error_code error;
            outPath = fs::absolute(path, error).lexically_normal();
            if (error || outPath.empty())
            {
                outError = "Failed to resolve package path.";
                return false;
            }
            return true;
        }

        bool IsContainedBy(const fs::path& path, const fs::path& root)
        {
            auto pathIt = path.begin();
            auto rootIt = root.begin();
            for (; rootIt != root.end(); ++rootIt, ++pathIt)
            {
                if (pathIt == path.end() || *pathIt != *rootIt)
                    return false;
            }
            return true;
        }

        bool ValidatePackageRequest(const AnimationRootMotionPackagePublishRequest& request,
                                    fs::path& outSourceRoot,
                                    fs::path& outParentAnimation,
                                    fs::path& outFinalPackage,
                                    std::string& outError)
        {
            if (request.sourceRoot.empty() || request.packagePath.empty() ||
                request.derivation.inputPath.empty() ||
                !request.derivation.outputPath.empty())
            {
                outError = "Root-motion package publication requires source root, input, package path, and no artifact output path.";
                return false;
            }

            std::error_code error;
            outSourceRoot = fs::weakly_canonical(request.sourceRoot, error);
            if (error || !fs::is_directory(outSourceRoot, error) || error)
            {
                outError = "Root-motion package source root must be an existing directory.";
                return false;
            }
            outParentAnimation = fs::weakly_canonical(request.derivation.inputPath, error);
            if (error || !fs::is_regular_file(outParentAnimation, error) || error ||
                !IsContainedBy(outParentAnimation, outSourceRoot))
            {
                outError = "Root-motion package parent animation must be a regular file under the source root.";
                return false;
            }

            const fs::path expectedParent = fs::weakly_canonical(
                outSourceRoot / RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SOURCE_PATH,
                error);
            if (error || expectedParent != outParentAnimation)
            {
                outError = "Root-motion package input must be the approved parent animation path under the Samples asset root.";
                return false;
            }

            if (!MakeAbsolutePath(request.packagePath, outFinalPackage, outError) ||
                outFinalPackage.filename().empty())
            {
                if (outError.empty())
                    outError = "Root-motion package output path is invalid.";
                return false;
            }
            const fs::path packageParent = outFinalPackage.parent_path();
            if (!fs::is_directory(packageParent, error) || error)
            {
                outError = "Root-motion package output parent directory must already exist.";
                return false;
            }
            if (fs::exists(outFinalPackage, error) &&
                !fs::is_directory(outFinalPackage, error))
            {
                outError = "Root-motion package output must be a directory when it already exists.";
                return false;
            }
            if (error)
            {
                outError = "Failed to query root-motion package output path.";
                return false;
            }
            return true;
        }

        bool MakeUniquePackageSibling(const fs::path& finalPackage,
                                      const char* role,
                                      fs::path& outPath,
                                      std::string& outError)
        {
            for (uint32 attempt = 0; attempt < 128; ++attempt)
            {
                const uint64 sequence = s_packageSequence.fetch_add(
                    1, std::memory_order_relaxed);
                const uint64 ticks = static_cast<uint64>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
                const fs::path candidate = finalPackage.parent_path() /
                    (".rvx-root-motion-package-" + std::string(role) + "-" +
                     std::to_string(ticks) + "-" +
                     std::to_string(sequence));
                std::error_code error;
                if (!fs::exists(candidate, error) && !error)
                {
                    outPath = candidate;
                    return true;
                }
            }
            outError = "Failed to reserve a unique root-motion package transaction path.";
            return false;
        }

        bool RemovePackageTree(const fs::path& path, std::string& outError)
        {
            if (path.empty())
                return true;
            std::error_code error;
            if (!fs::exists(path, error))
            {
                if (error)
                {
                    outError = "Failed to query root-motion package transaction path.";
                    return false;
                }
                return true;
            }
            fs::remove_all(path, error);
            if (error)
            {
                outError = "Failed to clean root-motion package transaction path: " +
                           error.message();
                return false;
            }
            return true;
        }

        bool RenamePackagePath(const fs::path& from,
                               const fs::path& to,
                               const char* operation,
                               std::string& outError)
        {
            std::error_code error;
            fs::rename(from, to, error);
            if (error)
            {
                outError = std::string(operation) + ": " + error.message();
                return false;
            }
            return true;
        }

        bool RestorePreviousPackage(const fs::path& finalPackage,
                                    const fs::path& backupPackage,
                                    const fs::path& stagingPackage,
                                    std::string& outError)
        {
            std::string rollbackError;
            if (fs::exists(finalPackage))
            {
                if (!RenamePackagePath(finalPackage,
                                       stagingPackage,
                                       "Failed to move new root-motion package aside during rollback",
                                       rollbackError))
                {
                    outError += "; rollback failed: " + rollbackError;
                    return false;
                }
            }
            if (!RenamePackagePath(backupPackage,
                                   finalPackage,
                                   "Failed to restore previous root-motion package",
                                   rollbackError))
            {
                outError += "; rollback failed: " + rollbackError;
                return false;
            }
            if (!RemovePackageTree(stagingPackage, rollbackError))
            {
                outError += "; rollback staging cleanup failed: " + rollbackError;
                return false;
            }
            return true;
        }

        bool PublishPackageAtomically(const fs::path& stagingPackage,
                                      const fs::path& finalPackage,
                                      const fs::path& backupPackage,
                                      std::string& outError)
        {
            std::error_code error;
            const bool hadPreviousPackage = fs::exists(finalPackage, error);
            if (error)
            {
                outError = "Failed to query existing root-motion package.";
                return false;
            }
            if (!hadPreviousPackage)
            {
                return RenamePackagePath(stagingPackage,
                                         finalPackage,
                                         "Failed to publish root-motion package",
                                         outError);
            }

            if (!RenamePackagePath(finalPackage,
                                   backupPackage,
                                   "Failed to back up existing root-motion package",
                                   outError))
            {
                return false;
            }
            if (!RenamePackagePath(stagingPackage,
                                   finalPackage,
                                   "Failed to publish staged root-motion package",
                                   outError))
            {
                std::string restoreError;
                if (!RenamePackagePath(backupPackage,
                                       finalPackage,
                                       "Failed to restore existing root-motion package",
                                       restoreError))
                {
                    outError += "; rollback failed: " + restoreError;
                }
                return false;
            }
            std::string cleanupError;
            if (!RemovePackageTree(backupPackage, cleanupError))
            {
                outError = "Failed to clean root-motion package backup: " + cleanupError;
                RestorePreviousPackage(finalPackage,
                                       backupPackage,
                                       stagingPackage,
                                       outError);
                return false;
            }
            return true;
        }

        std::string BuildPackageCookSettings(
            const AnimationRootMotionDeriveResult& derivation)
        {
            return std::string("schema=RVX_ANIMATION_ROOT_MOTION_PACKAGE_V1\n") +
                   "deriver.name=RVXAnimationDerive\n" +
                   "deriver.tool.version=" +
                       RVX_ANIMATION_ROOT_MOTION_DERIVE_TOOL_VERSION + "\n" +
                   "deriver.recipe.sha256=" + derivation.recipeSha256 + "\n" +
                   "parent.asset.id=" + RVX_ROOT_MOTION_DERIVE_PARENT_ASSET_ID + "\n" +
                   "parent.asset.content.id=" +
                       RVX_ROOT_MOTION_DERIVE_PARENT_CONTENT_ID + "\n" +
                   "parent.animation.byteCount=" +
                       std::to_string(RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_BYTE_COUNT) + "\n" +
                   "parent.animation.sha256=" +
                       RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256 + "\n" +
                   "source.clip=" + RVX_ROOT_MOTION_DERIVE_SOURCE_CLIP + "\n" +
                   "output.clip=" + RVX_ROOT_MOTION_DERIVE_OUTPUT_CLIP + "\n" +
                   "root.bone=" + RVX_ROOT_MOTION_DERIVE_ROOT_BONE + "\n" +
                   "translation.axis=+Z\n"
                   "translation.speed=1.0\n"
                   "duration.us=1250000\n";
        }

        Resource::ResourceContentIdentity MakeSelfContainedIdentity(
            Resource::ResourceContentIdentityDomain domain,
            const std::string& digest,
            uint64 byteCount)
        {
            Resource::ResourceContentIdentity identity;
            identity.schemaVersion = Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
            identity.domain = domain;
            identity.scope = Resource::ResourceContentIdentityScope::SelfContainedArtifact;
            identity.algorithm = Resource::ResourceContentHashAlgorithm::SHA256;
            identity.digest = digest;
            identity.byteCount = byteCount;
            identity.fileCount = 1;
            return identity;
        }

        bool VerifyStagedPackage(const fs::path& sourceRoot,
                                 const fs::path& stagingPackage,
                                 const AnimationRootMotionDeriveResult& derivation,
                                 AnimationRootMotionPackagePublishResult& outResult,
                                 std::string& outError)
        {
            const fs::path artifactPath = stagingPackage /
                RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH;
            std::vector<uint8> artifactBytes;
            if (!ReadBoundedFile(artifactPath, artifactBytes, outError))
                return false;
            const std::string artifactSha256 = Hash::FormatSHA256Digest(
                Hash::ComputeSHA256(artifactBytes.data(), artifactBytes.size()));

            Resource::CookManifest manifest;
            if (!Resource::LoadCookManifest(
                    stagingPackage / RVX_ROOT_MOTION_DERIVE_PACKAGE_MANIFEST_PATH,
                    manifest,
                    outError))
            {
                outError = "Failed to reload staged root-motion package manifest: " + outError;
                return false;
            }
            if (manifest.entries.size() != 1 ||
                manifest.toolName != Resource::CookManifest::DefaultToolName ||
                manifest.toolVersion != Resource::CookManifest::DefaultToolVersion)
            {
                outError = "Staged root-motion package manifest has an unexpected tool or entry count.";
                return false;
            }
            const Resource::CookManifestEntry& entry = manifest.entries.front();
            if (!entry.success ||
                entry.sourcePath != RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SOURCE_PATH ||
                entry.outputPath != RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH ||
                entry.type != Resource::CookAssetType::Animation ||
                entry.importerName != "RVXAnimationDerive" ||
                !entry.sourceDependencies.empty() || !entry.dependencies.empty() ||
                entry.artifacts.size() != 1 ||
                entry.artifacts.front().relativePath !=
                    RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH ||
                entry.artifacts.front().byteCount != artifactBytes.size() ||
                entry.artifacts.front().sha256 != artifactSha256 ||
                entry.sourceContent.byteCount !=
                    RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_BYTE_COUNT ||
                entry.sourceContent.sha256 !=
                    RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256)
            {
                outError = "Staged root-motion package manifest does not match the approved selector or identities.";
                return false;
            }

            Resource::ResourceContentIdentity cookedIdentity;
            if (!Resource::ComputeCookedContentIdentity(manifest, cookedIdentity, outError))
            {
                outError = "Failed to compute staged root-motion package cooked identity: " + outError;
                return false;
            }
            Resource::CookManifestExpectation expectation;
            expectation.selector.sourcePath =
                RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SOURCE_PATH;
            expectation.selector.outputPath =
                RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH;
            expectation.selector.type = Resource::CookAssetType::Animation;
            expectation.expectedSourceContentIdentity = MakeSelfContainedIdentity(
                Resource::ResourceContentIdentityDomain::Source,
                RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256,
                RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_BYTE_COUNT);
            expectation.expectedCookedContentIdentity = cookedIdentity;
            expectation.expectedManifestContentIdentity = manifest.manifestContentIdentity;
            expectation.expectedCookSettingsHash = entry.cookSettingsHash;
            expectation.expectedRecipeHash = entry.recipeHash;
            const Resource::CookManifestAdmissionReceipt receipt =
                Resource::VerifyCookedAssetAdmission(manifest,
                                                     sourceRoot,
                                                     stagingPackage,
                                                     expectation);
            if (!receipt.IsAccepted() ||
                receipt.observedSourceContentIdentity !=
                    expectation.expectedSourceContentIdentity ||
                receipt.observedCookedContentIdentity != cookedIdentity ||
                receipt.observedManifestContentIdentity !=
                    manifest.manifestContentIdentity)
            {
                outError = "Staged root-motion package admission failed: " + receipt.detail;
                return false;
            }

            outResult.derivation = derivation;
            outResult.artifactByteCount = artifactBytes.size();
            outResult.artifactSha256 = artifactSha256;
            outResult.cookSettingsHash = entry.cookSettingsHash;
            outResult.manifestRecipeHash = entry.recipeHash;
            outResult.cookedContentSha256 = cookedIdentity.digest;
            outResult.manifestContentSha256 = manifest.manifestContentIdentity.digest;
            return true;
        }
    } // namespace

    std::string FormatAnimationRootMotionParentContentId(
        const AnimationRootMotionParentContentId& contentId)
    {
        return "rvx-asset-content-v" + std::to_string(contentId.schemaVersion) + ":" +
               contentId.algorithm + ":" + contentId.digest + ":" +
               std::to_string(contentId.byteCount) + ":" +
               std::to_string(contentId.fileCount);
    }

    bool DeriveAnimationRootMotion(const AnimationRootMotionDeriveRequest& request,
                                   AnimationRootMotionDeriveResult& outResult,
                                   std::string& outError)
    {
        outResult = {};
        outError.clear();
        fs::path stagingPath;
        try
        {
            if (!IsApprovedRequest(request, outError) ||
                !ValidateOutputPath(request.inputPath, request.outputPath, outError))
            {
                return false;
            }

            std::vector<uint8> inputBytes;
            if (!ReadBoundedFile(request.inputPath, inputBytes, outError))
                return false;
            const std::string inputSha256 = Hash::FormatSHA256Digest(
                Hash::ComputeSHA256(inputBytes.data(), inputBytes.size()));
            if (inputBytes.size() !=
                    RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_BYTE_COUNT ||
                inputSha256 != RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256)
            {
                outError = "Root-motion input does not match the approved parent animation artifact.";
                return false;
            }

            Resource::CookedAnimationArtifact inputArtifact;
            if (!Resource::DeserializeCookedAnimationArtifact(inputBytes,
                                                               inputArtifact,
                                                               outError))
            {
                outError = "Failed to deserialize root-motion input: " + outError;
                return false;
            }

            const auto source = inputArtifact.clips.find(request.sourceClip);
            if (source == inputArtifact.clips.end() || !source->second ||
                source->second->duration != request.duration)
            {
                outError = "Root-motion input does not contain the approved Walk clip duration.";
                return false;
            }

            Animation::AnimationClip::Ptr derived = source->second->Clone();
            Animation::TransformTrack* rootTrack =
                derived->FindTransformTrack(request.rootBone);
            if (!rootTrack || rootTrack->translationKeyframes.empty())
            {
                outError = "Root-motion input does not contain the approved root translation track.";
                return false;
            }
            for (Animation::KeyframeVec3& keyframe : rootTrack->translationKeyframes)
            {
                if (keyframe.time < 0 || keyframe.time > request.duration)
                {
                    outError = "Root-motion input root translation has an out-of-range key.";
                    return false;
                }
                keyframe.value.x = 0.0f;
                keyframe.value.z = static_cast<float32>(
                    static_cast<float64>(keyframe.time) / 1'000'000.0 * 1.0);
            }

            derived->name = request.outputClip;
            derived->hasRootMotion = true;
            derived->rootMotionBoneName = request.rootBone;
            derived->metadata.customData.clear();
            const std::string recipe = BuildRecipe(request, inputSha256);
            // Hash through the common streaming path so recipe byte identity is explicit.
            Hash::SHA256Hasher recipeHasher;
            recipeHasher.Update(recipe);
            const std::string canonicalRecipeSha256 = recipeHasher.FinalizeHex();
            derived->metadata.customData[MetadataRecipe] = recipe;
            derived->metadata.customData[MetadataRecipeVersion] = RecipeVersion;
            derived->metadata.customData[MetadataRecipeHash] = canonicalRecipeSha256;
            derived->metadata.customData[MetadataParentAssetId] = request.parentAssetId;
            derived->metadata.customData[MetadataParentContentId] =
                FormatAnimationRootMotionParentContentId(request.parentContentId);
            derived->metadata.customData[MetadataInputHash] = inputSha256;
            derived->metadata.customData[MetadataSourceClip] = request.sourceClip;
            derived->metadata.customData[MetadataAxis] = "+Z";
            derived->metadata.customData[MetadataSpeed] = "1.0";

            Resource::CookedAnimationArtifact outputArtifact;
            outputArtifact.sourcePath = inputArtifact.sourcePath;
            outputArtifact.skeleton = inputArtifact.skeleton;
            outputArtifact.clips.emplace(derived->name, std::move(derived));

            std::vector<uint8> outputBytes;
            if (!Resource::SerializeCookedAnimationArtifact(outputArtifact,
                                                             outputBytes,
                                                             outError))
            {
                outError = "Failed to serialize derived root-motion artifact: " + outError;
                return false;
            }

            if (!CreateStagingPath(request.outputPath, stagingPath, outError) ||
                !WriteStagingFile(stagingPath, outputBytes, outError) ||
                !ReplaceOutputAtomically(stagingPath, request.outputPath, outError))
            {
                RemoveStagingFile(stagingPath);
                return false;
            }

            outResult.inputSha256 = inputSha256;
            outResult.recipe = recipe;
            outResult.recipeSha256 = canonicalRecipeSha256;
            return true;
        }
        catch (const std::exception& exception)
        {
            RemoveStagingFile(stagingPath);
            outError = std::string("Root-motion derivation failed: ") + exception.what();
            return false;
        }
        catch (...)
        {
            RemoveStagingFile(stagingPath);
            outError = "Root-motion derivation failed with an unknown error.";
            return false;
        }
    }

    bool PublishAnimationRootMotionPackage(
        const AnimationRootMotionPackagePublishRequest& request,
        AnimationRootMotionPackagePublishResult& outResult,
        std::string& outError)
    {
        outResult = {};
        outError.clear();
        fs::path stagingPackage;
        fs::path backupPackage;
        try
        {
            fs::path sourceRoot;
            fs::path parentAnimation;
            fs::path finalPackage;
            if (!ValidatePackageRequest(request,
                                        sourceRoot,
                                        parentAnimation,
                                        finalPackage,
                                        outError))
            {
                return false;
            }
            if (!MakeUniquePackageSibling(finalPackage,
                                          "staging",
                                          stagingPackage,
                                          outError) ||
                !MakeUniquePackageSibling(finalPackage,
                                          "backup",
                                          backupPackage,
                                          outError))
            {
                return false;
            }

            std::error_code error;
            fs::create_directory(stagingPackage, error);
            if (error)
            {
                outError = "Failed to create root-motion package staging directory: " +
                           error.message();
                return false;
            }

            const fs::path stagedArtifact = stagingPackage /
                RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH;
            AnimationRootMotionDeriveRequest derivation = request.derivation;
            derivation.inputPath = parentAnimation;
            derivation.outputPath = stagedArtifact;
            AnimationRootMotionDeriveResult derivationResult;
            if (!DeriveAnimationRootMotion(derivation, derivationResult, outError))
            {
                RemovePackageTree(stagingPackage, outError);
                return false;
            }

            CookManifestEntry entry;
            entry.type = AssetType::Animation;
            entry.success = true;
            const std::string settings = BuildPackageCookSettings(derivationResult);
            if (!CookManifest::PopulateSuccessfulEntry(
                    entry,
                    sourceRoot,
                    stagingPackage,
                    parentAnimation,
                    stagedArtifact,
                    {},
                    {},
                    settings,
                    "RVXAnimationDerive",
                    outError))
            {
                RemovePackageTree(stagingPackage, outError);
                return false;
            }

            CookManifest manifest;
            manifest.sourceRoot = sourceRoot.string();
            manifest.outputRoot = finalPackage.string();
            manifest.recursive = false;
            manifest.entries.push_back(std::move(entry));
            if (!manifest.Save(stagingPackage /
                                   RVX_ROOT_MOTION_DERIVE_PACKAGE_MANIFEST_PATH,
                               outError,
                               stagingPackage))
            {
                RemovePackageTree(stagingPackage, outError);
                return false;
            }

            AnimationRootMotionPackagePublishResult stagedResult;
            if (!VerifyStagedPackage(sourceRoot,
                                     stagingPackage,
                                     derivationResult,
                                     stagedResult,
                                     outError))
            {
                RemovePackageTree(stagingPackage, outError);
                return false;
            }
            if (!PublishPackageAtomically(stagingPackage,
                                          finalPackage,
                                          backupPackage,
                                          outError))
            {
                std::string cleanupError;
                if (!RemovePackageTree(stagingPackage, cleanupError))
                    outError += "; staging cleanup failed: " + cleanupError;
                return false;
            }

            outResult = std::move(stagedResult);
            return true;
        }
        catch (const std::exception& exception)
        {
            std::string cleanupError;
            RemovePackageTree(stagingPackage, cleanupError);
            outError = std::string("Root-motion package publication failed: ") +
                       exception.what();
            return false;
        }
        catch (...)
        {
            std::string cleanupError;
            RemovePackageTree(stagingPackage, cleanupError);
            outError = "Root-motion package publication failed with an unknown error.";
            return false;
        }
    }
} // namespace RVX::Tools
