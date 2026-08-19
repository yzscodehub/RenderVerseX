#include "Core/Hash/SHA256.h"
#include "Core/Log.h"
#include "Resource/Loader/EnvironmentLoader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace RVX;
using namespace RVX::Resource;

namespace
{
    class ScopedTempFile final
    {
    public:
        explicit ScopedTempFile(std::filesystem::path path)
            : m_path(std::move(path))
        {
        }

        ~ScopedTempFile()
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }

        const std::filesystem::path& Get() const { return m_path; }

    private:
        std::filesystem::path m_path;
    };

    std::filesystem::path MakeUniqueHDRPath(const std::string& name)
    {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
               ("RVX_EnvironmentPrepared_" + name + "_" + std::to_string(ticks) + ".hdr");
    }

    void WriteRGBE(std::ofstream& output, float red, float green, float blue)
    {
        const float maximum = std::max({red, green, blue});
        uint8 values[4] = {};
        if (maximum > 0.0f)
        {
            int exponent = 0;
            const float mantissa = std::frexp(maximum, &exponent);
            const float scale = mantissa * 256.0f / maximum;
            values[0] = static_cast<uint8>(std::clamp(red * scale, 0.0f, 255.0f));
            values[1] = static_cast<uint8>(std::clamp(green * scale, 0.0f, 255.0f));
            values[2] = static_cast<uint8>(std::clamp(blue * scale, 0.0f, 255.0f));
            values[3] = static_cast<uint8>(exponent + 128);
        }
        output.write(reinterpret_cast<const char*>(values), sizeof(values));
    }

    bool WriteHDRFixture(const std::filesystem::path& path)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return false;
        }

        constexpr uint32 width = 4;
        constexpr uint32 height = 2;
        output << "#?RADIANCE\n";
        output << "FORMAT=32-bit_rle_rgbe\n\n";
        output << "-Y " << height << " +X " << width << "\n";
        for (uint32 y = 0; y < height; ++y)
        {
            for (uint32 x = 0; x < width; ++x)
            {
                const float horizontal = static_cast<float>(x) / static_cast<float>(width - 1u);
                const float vertical = static_cast<float>(y) / static_cast<float>(height - 1u);
                WriteRGBE(output,
                          0.2f + horizontal,
                          0.3f + vertical,
                          0.5f + horizontal * vertical);
            }
        }
        return output.good();
    }

    ResourceContentIdentity ReadExpectedSourceIdentity(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
        {
            return {};
        }
        const std::streamsize byteCount = input.tellg();
        if (byteCount < 0)
        {
            return {};
        }
        input.seekg(0, std::ios::beg);
        std::vector<uint8> bytes(static_cast<size_t>(byteCount));
        if (byteCount != 0 &&
            !input.read(reinterpret_cast<char*>(bytes.data()), byteCount))
        {
            return {};
        }

        ResourceContentIdentity identity;
        identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = ResourceContentIdentityDomain::Source;
        identity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = ResourceContentHashAlgorithm::SHA256;
        identity.digest = Hash::FormatSHA256Digest(
            Hash::ComputeSHA256(bytes.data(), bytes.size()));
        identity.byteCount = static_cast<uint64>(bytes.size());
        identity.fileCount = 1;
        return identity;
    }

    ResourceLoadPreparationContext MakeContext(const std::filesystem::path& path,
                                                const EnvironmentPreparationState& state)
    {
        ResourceLoadPreparationContext context;
        context.assetKey = MakeAssetKey(path.string(),
                                        ResourceType::Environment,
                                        state.importOptionsHash,
                                        0,
                                        1);
        context.resourceIdentityPath = context.assetKey.canonicalPath;
        context.requestedPath = path.string();
        context.resolvedPath = path.string();
        context.rootResourceId = GenerateResourceId(context.resourceIdentityPath);
        return context;
    }

    class EnvironmentManagerGuard final
    {
    public:
        EnvironmentManagerGuard()
        {
            Log::Initialize();
            JobSystem::Get().Shutdown();
            ResourceManagerConfig config;
            config.asyncThreadCount = 2;
            config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
            config.runtimePolicy.allowSourceAssetReads = true;
            ResourceManager::Get().Initialize(config);
        }

        ~EnvironmentManagerGuard()
        {
            if (ResourceManager::Get().IsInitialized())
            {
                ResourceManager::Get().Shutdown();
            }
            JobSystem::Get().Shutdown();
            Log::Shutdown();
        }
    };
} // namespace

TEST(EnvironmentPreparedLoadingValidation, CapturedAdmissionStateIsDeterministicAndImmutable)
{
    EnvironmentLoadOptions options;
    options.quality = HDRIBLQualityProfile::Validation;
    options.exposure = 1.25f;
    options.applyGamma = true;
    EnvironmentLoader loader(options);

    const EnvironmentPreparationState first = loader.CapturePreparationState();
    const EnvironmentPreparationState second = loader.CapturePreparationState();
    ASSERT_TRUE(first.IsValid());
    EXPECT_TRUE(second.IsValid());
    EXPECT_EQ(first.importOptionsHash, second.importOptionsHash);
    EXPECT_EQ(first.hdrOptions.cubemapResolution, second.hdrOptions.cubemapResolution);
    EXPECT_EQ(first.hdrOptions.convolutionSamples, second.hdrOptions.convolutionSamples);

    ResourceLoadPreparationStateRef admitted;
    uint64 canonicalHash = 0;
    ResourceLoadError admissionError;
    EXPECT_TRUE(loader.CapturePreparationState(0,
                                               admitted,
                                               canonicalHash,
                                               admissionError));
    ASSERT_TRUE(admitted);
    EXPECT_EQ(canonicalHash, first.importOptionsHash);
    EXPECT_NE(std::dynamic_pointer_cast<const EnvironmentPreparationState>(admitted), nullptr);
    EXPECT_FALSE(loader.CapturePreparationState(first.importOptionsHash + 1u,
                                                admitted,
                                                canonicalHash,
                                                admissionError));
    EXPECT_EQ(admissionError.code, ResourceLoadErrorCode::InvalidRequest);

    options.exposure = 2.0f;
    loader.SetOptions(options);
    const EnvironmentPreparationState changed = loader.CapturePreparationState();
    EXPECT_TRUE(changed.IsValid());
    EXPECT_NE(first.importOptionsHash, changed.importOptionsHash);
    EXPECT_FLOAT_EQ(first.hdrOptions.exposure, 1.25f);
}

TEST(EnvironmentPreparedLoadingValidation,
     ExplicitPerRequestStateDoesNotMutateOrObserveGlobalLoaderOptions)
{
    EnvironmentLoadOptions requestOptions;
    requestOptions.quality = HDRIBLQualityProfile::Validation;
    requestOptions.exposure = 1.25f;

    uint64 canonicalHash = 0;
    ResourceLoadError error;
    ResourceLoadPreparationStateRef state =
        EnvironmentLoader::CreatePreparationState(
            requestOptions,
            canonicalHash,
            error);
    ASSERT_TRUE(state) << error.message;
    ASSERT_NE(canonicalHash, 0U);

    EnvironmentLoadOptions unrelatedGlobalOptions;
    unrelatedGlobalOptions.quality = HDRIBLQualityProfile::High;
    unrelatedGlobalOptions.exposure = 4.0f;
    EnvironmentLoader loader(unrelatedGlobalOptions);

    ResourceLoadPreparationStateRef validated;
    uint64 validatedHash = 0;
    EXPECT_TRUE(loader.ValidatePreparationState(canonicalHash,
                                                state,
                                                validated,
                                                validatedHash,
                                                error));
    EXPECT_EQ(validated.get(), state.get());
    EXPECT_EQ(validatedHash, canonicalHash);
    const auto typed =
        std::dynamic_pointer_cast<const EnvironmentPreparationState>(validated);
    ASSERT_TRUE(typed);
    EXPECT_FLOAT_EQ(typed->hdrOptions.exposure, requestOptions.exposure);
    EXPECT_EQ(typed->sourceOptions.quality, requestOptions.quality);

    EXPECT_FALSE(loader.ValidatePreparationState(canonicalHash + 1u,
                                                 state,
                                                 validated,
                                                 validatedHash,
                                                 error));
    EXPECT_EQ(error.code, ResourceLoadErrorCode::InvalidRequest);
}

TEST(EnvironmentPreparedLoadingValidation, PrepareBuildsUnpublishedDependencyFirstEnvironmentBundle)
{
    const ScopedTempFile fixture(MakeUniqueHDRPath("Bundle"));
    ASSERT_TRUE(WriteHDRFixture(fixture.Get()));

    EnvironmentLoadOptions options;
    options.quality = HDRIBLQualityProfile::Validation;
    EnvironmentLoader loader(options);
    const EnvironmentPreparationState state = loader.CapturePreparationState();
    ASSERT_TRUE(state.IsValid());

    ResourceLoadPreparationContext context = MakeContext(fixture.Get(), state);
    context.loaderState = std::make_shared<EnvironmentPreparationState>(state);
    PreparedResourceBundle bundle;
    ResourceLoadError error;
    ASSERT_TRUE(loader.Prepare(context, bundle, error)) << error.message;
    ASSERT_TRUE(bundle.IsValid()) << bundle.GetValidationError();
    ASSERT_EQ(bundle.GetEntries().size(), 5u);

    for (size_t index = 0; index < 4; ++index)
    {
        const PreparedResourceEntry& entry = bundle.GetEntries()[index];
        ASSERT_TRUE(entry.resource);
        EXPECT_FALSE(entry.isRoot);
        EXPECT_EQ(entry.resource->GetType(), ResourceType::Texture);
        EXPECT_FALSE(entry.resource->IsLoaded());
    }

    const PreparedResourceEntry& rootEntry = bundle.GetEntries().back();
    ASSERT_TRUE(rootEntry.resource);
    EXPECT_TRUE(rootEntry.isRoot);
    EXPECT_EQ(rootEntry.resource->GetType(), ResourceType::Environment);
    EXPECT_FALSE(rootEntry.resource->IsLoaded());

    auto* environment = dynamic_cast<EnvironmentResource*>(rootEntry.resource.Get());
    ASSERT_NE(environment, nullptr);
    EXPECT_TRUE(environment->GetData().IsStructurallyValid());
    EXPECT_FALSE(environment->GetData().IsValid());
    EXPECT_FLOAT_EQ(environment->GetData().intensity, state.hdrOptions.exposure);
    EXPECT_EQ(environment->GetRequiredDependencies().size(), 4u);
    EXPECT_EQ(environment->GetRequiredDependencies()[0], bundle.GetEntries()[0].resource->GetId());
    EXPECT_EQ(environment->GetRequiredDependencies()[1], bundle.GetEntries()[1].resource->GetId());
    EXPECT_EQ(environment->GetRequiredDependencies()[2], bundle.GetEntries()[2].resource->GetId());
    EXPECT_EQ(environment->GetRequiredDependencies()[3], bundle.GetEntries()[3].resource->GetId());
}

TEST(EnvironmentPreparedLoadingValidation, RejectsHashDriftCancellationAndInvalidOptionsBeforePublish)
{
    EnvironmentLoadOptions options;
    options.quality = HDRIBLQualityProfile::Validation;
    EnvironmentLoader loader(options);
    const EnvironmentPreparationState state = loader.CapturePreparationState();
    ASSERT_TRUE(state.IsValid());

    ResourceLoadPreparationContext hashDrift;
    hashDrift.assetKey = MakeAssetKey("environment.hdr",
                                      ResourceType::Environment,
                                      state.importOptionsHash + 1u,
                                      0,
                                      1);
    hashDrift.resourceIdentityPath = hashDrift.assetKey.canonicalPath;
    hashDrift.requestedPath = "environment.hdr";
    hashDrift.resolvedPath = "environment.hdr";
    hashDrift.rootResourceId = GenerateResourceId(hashDrift.resourceIdentityPath);
    PreparedResourceBundle driftBundle;
    ResourceLoadError error;
    EXPECT_FALSE(loader.Prepare(hashDrift, state, driftBundle, error));
    EXPECT_EQ(error.code, ResourceLoadErrorCode::InvalidRequest);
    EXPECT_TRUE(driftBundle.IsEmpty());

    ResourceLoadPreparationContext cancelled = MakeContext("environment.hdr", state);
    cancelled.isCancellationRequested = [] { return true; };
    PreparedResourceBundle cancelledBundle;
    error = {};
    EXPECT_FALSE(loader.Prepare(cancelled, state, cancelledBundle, error));
    EXPECT_EQ(error.code, ResourceLoadErrorCode::Cancelled);
    EXPECT_TRUE(cancelledBundle.IsEmpty());

    options.exposure = std::numeric_limits<float32>::quiet_NaN();
    loader.SetOptions(options);
    const EnvironmentPreparationState invalid = loader.CapturePreparationState();
    EXPECT_FALSE(invalid.IsValid());
    EXPECT_EQ(invalid.importOptionsHash, 0u);
}

TEST(EnvironmentPreparedLoadingValidation, CooperativeCancellationAbortsIBLBakeWithoutPartialBundle)
{
    const ScopedTempFile fixture(MakeUniqueHDRPath("CooperativeCancellation"));
    ASSERT_TRUE(WriteHDRFixture(fixture.Get()));

    EnvironmentLoadOptions options;
    options.quality = HDRIBLQualityProfile::Validation;
    EnvironmentLoader loader(options);
    const EnvironmentPreparationState state = loader.CapturePreparationState();
    ASSERT_TRUE(state.IsValid());

    ResourceLoadPreparationContext context = MakeContext(fixture.Get(), state);
    context.loaderState = std::make_shared<EnvironmentPreparationState>(state);

    // Validation quality completes the source conversion and irradiance pass
    // before this threshold, so cancellation is observed inside a later
    // prefiltered-convolution loop rather than only at admission.
    constexpr uint32 CancelAfterChecks = 120;
    std::atomic<uint32> cancellationChecks = 0;
    context.isCancellationRequested = [&cancellationChecks]
    {
        return cancellationChecks.fetch_add(1u, std::memory_order_relaxed) >= CancelAfterChecks;
    };

    PreparedResourceBundle bundle;
    ResourceLoadError error;
    EXPECT_FALSE(loader.Prepare(context, bundle, error));
    EXPECT_EQ(error.code, ResourceLoadErrorCode::Cancelled);
    EXPECT_TRUE(bundle.IsEmpty());
    EXPECT_GT(cancellationChecks.load(std::memory_order_relaxed), CancelAfterChecks);
}

TEST(EnvironmentPreparedLoadingValidation, ResourceManagerRoutesTypedEnvironmentAndPublishesDependenciesFirst)
{
    const ScopedTempFile fixture(MakeUniqueHDRPath("Manager"));
    ASSERT_TRUE(WriteHDRFixture(fixture.Get()));
    EnvironmentManagerGuard guard;

    auto* loader = dynamic_cast<EnvironmentLoader*>(
        ResourceManager::Get().GetLoader(ResourceType::Environment));
    ASSERT_NE(loader, nullptr);
    EnvironmentLoadOptions options;
    options.quality = HDRIBLQualityProfile::Validation;
    options.exposure = 1.5f;
    loader->SetOptions(options);

    auto request =
        ResourceManager::Get().RequestAsync<EnvironmentResource>(fixture.Get().string());
    ASSERT_TRUE(request);
    EXPECT_EQ(request.GetSnapshot().assetKey.resourceType, ResourceType::Environment);
    EXPECT_EQ(request.GetSnapshot().assetKey.importOptionsHash,
              EnvironmentLoader::ComputeImportOptionsHash(options));
    EXPECT_FALSE(request.TryGet());

    for (uint32 attempt = 0; attempt < 500 && !request.TryGet(); ++attempt)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    EnvironmentHandle environment = request.TryGet();
    ASSERT_TRUE(environment);
    EXPECT_TRUE(environment->IsLoaded());
    EXPECT_TRUE(environment->GetData().IsValid());
    EXPECT_FLOAT_EQ(environment->GetData().intensity, 1.5f);
    EXPECT_EQ(environment->GetRequiredDependencies().size(), 4u);
    EXPECT_TRUE(ResourceManager::Get().IsLoaded(request.GetSnapshot().assetKey));
    EnvironmentHandle synchronous =
        ResourceManager::Get().Load<EnvironmentResource>(fixture.Get().string());
    EXPECT_EQ(synchronous.Get(), environment.Get());

    const ResourceId rootId = environment.GetId();
    const AssetKey environmentKey = request.GetSnapshot().assetKey;
    const std::vector<ResourceId> dependencyIds = environment->GetRequiredDependencies();
    ASSERT_EQ(dependencyIds.size(), 4u);
    for (ResourceId dependencyId : dependencyIds)
    {
        ASSERT_TRUE(ResourceManager::Get().GetCache().Contains(dependencyId));
    }
    request.Cancel();
    environment.Reset();
    synchronous.Reset();
    ResourceManager::Get().Unload(environmentKey);
    EXPECT_FALSE(ResourceManager::Get().IsLoaded(rootId));
    for (ResourceId dependencyId : dependencyIds)
    {
        ASSERT_FALSE(ResourceManager::Get().GetCache().Contains(dependencyId));
    }

    auto reload =
        ResourceManager::Get().RequestAsync<EnvironmentResource>(fixture.Get().string());
    ASSERT_TRUE(reload);
    for (uint32 attempt = 0; attempt < 500 && !reload.TryGet(); ++attempt)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const EnvironmentHandle reloaded = reload.TryGet();
    ASSERT_TRUE(reloaded);
    EXPECT_TRUE(reloaded->GetData().IsValid());
    EXPECT_EQ(reloaded->GetData().environment.Get(),
              ResourceManager::Get().GetCache().Get(dependencyIds[0]));
    EXPECT_EQ(reloaded->GetData().irradiance.Get(),
              ResourceManager::Get().GetCache().Get(dependencyIds[1]));
    EXPECT_EQ(reloaded->GetData().prefiltered.Get(),
              ResourceManager::Get().GetCache().Get(dependencyIds[2]));
    EXPECT_EQ(reloaded->GetData().brdfLUT.Get(),
              ResourceManager::Get().GetCache().Get(dependencyIds[3]));
}

TEST(EnvironmentPreparedLoadingValidation,
     ResourceManagerUsesExplicitEnvironmentStateWithoutGlobalMutation)
{
    const ScopedTempFile fixture(MakeUniqueHDRPath("ExplicitState"));
    ASSERT_TRUE(WriteHDRFixture(fixture.Get()));
    EnvironmentManagerGuard guard;

    auto* loader = dynamic_cast<EnvironmentLoader*>(
        ResourceManager::Get().GetLoader(ResourceType::Environment));
    ASSERT_NE(loader, nullptr);
    const EnvironmentLoadOptions original = loader->GetOptions();

    EnvironmentLoadOptions requestOptions;
    requestOptions.quality = HDRIBLQualityProfile::Validation;
    requestOptions.exposure = 1.75f;
    uint64 canonicalHash = 0;
    ResourceLoadError error;
    ResourceLoadPreparationStateRef state =
        EnvironmentLoader::CreatePreparationState(
            requestOptions,
            canonicalHash,
            error);
    ASSERT_TRUE(state) << error.message;

    ResourceLoadOptions options;
    options.importOptionsHash = canonicalHash;
    auto request = ResourceManager::Get().RequestAsync<EnvironmentResource>(
        fixture.Get().string(),
        options,
        state);
    ASSERT_TRUE(request);
    EXPECT_FLOAT_EQ(loader->GetOptions().exposure, original.exposure);

    for (uint32 attempt = 0; attempt < 500 && !request.TryGet(); ++attempt)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const EnvironmentHandle environment = request.TryGet();
    ASSERT_TRUE(environment);
    EXPECT_FLOAT_EQ(environment->GetData().intensity,
                    requestOptions.exposure);
    EXPECT_EQ(request.GetSnapshot().assetKey.importOptionsHash,
              canonicalHash);
    EXPECT_FLOAT_EQ(loader->GetOptions().exposure, original.exposure);
}

TEST(EnvironmentPreparedLoadingValidation,
     PreparedEnvironmentVerifiesTheSameHDRBytesThatWereDecoded)
{
    const ScopedTempFile fixture(MakeUniqueHDRPath("ContentIdentity"));
    ASSERT_TRUE(WriteHDRFixture(fixture.Get()));
    const ResourceContentIdentity expected = ReadExpectedSourceIdentity(fixture.Get());
    ASSERT_TRUE(expected.IsValid());
    ASSERT_EQ(expected.fileCount, 1u);

    EnvironmentManagerGuard guard;
    auto* loader = dynamic_cast<EnvironmentLoader*>(
        ResourceManager::Get().GetLoader(ResourceType::Environment));
    ASSERT_NE(loader, nullptr);
    EnvironmentLoadOptions environmentOptions;
    environmentOptions.quality = HDRIBLQualityProfile::Validation;
    loader->SetOptions(environmentOptions);

    ResourceLoadOptions verifiedOptions;
    verifiedOptions.expectedContentIdentity = expected;
    auto verified = ResourceManager::Get().RequestAsync<EnvironmentResource>(
        fixture.Get().string(),
        verifiedOptions);
    ASSERT_TRUE(verified);
    for (uint32 attempt = 0; attempt < 500 && !verified.TryGet(); ++attempt)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EnvironmentHandle environment = verified.TryGet();
    ASSERT_TRUE(environment) << verified.GetSnapshot().error.message;
    const ResourceContentVerificationReceipt& receipt =
        environment->GetContentVerificationReceipt();
    EXPECT_EQ(receipt.status, ResourceContentVerificationStatus::Verified);
    EXPECT_EQ(receipt.expected, expected);
    EXPECT_EQ(receipt.observed, expected);
    EXPECT_EQ(receipt.observed.scope,
              ResourceContentIdentityScope::SelfContainedArtifact);
    EXPECT_EQ(receipt.observed.fileCount, 1u);
    EXPECT_EQ(receipt.observed.byteCount, expected.byteCount);

    const AssetKey verifiedKey = verified.GetSnapshot().assetKey;
    verified.Cancel();
    environment.Reset();
    EXPECT_EQ(ResourceManager::Get().Unload(verifiedKey),
              AssetResidencyReleaseResult::Unloaded);

    // Extra trailing data leaves the valid Radiance image decodable while
    // changing the exact encoded byte sequence. The old expected identity must
    // therefore fail before any replacement is published.
    {
        std::ofstream output(fixture.Get(), std::ios::binary | std::ios::app);
        ASSERT_TRUE(output);
        const char mutation = '\x7f';
        output.write(&mutation, 1);
        ASSERT_TRUE(output.good());
    }

    auto mismatched = ResourceManager::Get().RequestAsync<EnvironmentResource>(
        fixture.Get().string(),
        verifiedOptions);
    ASSERT_TRUE(mismatched);
    for (uint32 attempt = 0;
         attempt < 500 && mismatched.GetSnapshot().state != ResourceLoadState::Failed;
         ++attempt)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(mismatched.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(mismatched.GetSnapshot().error.code,
              ResourceLoadErrorCode::ContentIdentityMismatch);
    EXPECT_FALSE(ResourceManager::Get().IsLoaded(mismatched.GetSnapshot().assetKey));
}
