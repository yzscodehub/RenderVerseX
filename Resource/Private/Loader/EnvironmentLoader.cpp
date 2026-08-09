#include "Resource/Loader/EnvironmentLoader.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <type_traits>

namespace RVX::Resource
{
namespace
{
    constexpr uint64 FNVOffsetBasis = 14695981039346656037ull;
    constexpr uint64 FNVPrime = 1099511628211ull;
    constexpr uint32 EnvironmentLoaderSchemaVersion = 1;

    void HashByte(uint64& hash, uint8 value)
    {
        hash ^= value;
        hash *= FNVPrime;
    }

    template<typename T>
    void HashInteger(uint64& hash, T value)
    {
        static_assert(std::is_unsigned_v<T>);
        for (uint32 byteIndex = 0; byteIndex < sizeof(T); ++byteIndex)
        {
            HashByte(hash, static_cast<uint8>((value >> (byteIndex * 8u)) & 0xFFu));
        }
    }

    ResourceLoadError InvalidOptionsError()
    {
        return {ResourceLoadErrorCode::InvalidRequest,
                "Environment preparation options are invalid or non-finite."};
    }
} // namespace

bool EnvironmentPreparationState::IsValid() const
{
    return importOptionsHash != 0 && hdrOptions.generateCubemap && hdrOptions.generateIBL &&
           hdrOptions.cubemapResolution != 0 && hdrOptions.irradianceResolution != 0 &&
           hdrOptions.prefilteredResolution != 0 && hdrOptions.prefilteredMipLevels != 0 &&
           hdrOptions.brdfLUTResolution != 0 && hdrOptions.convolutionSamples != 0 &&
           std::isfinite(hdrOptions.exposure) && hdrOptions.exposure > 0.0f;
}

EnvironmentLoader::EnvironmentLoader(EnvironmentLoadOptions options)
    : m_options(options)
{
}

std::vector<std::string> EnvironmentLoader::GetSupportedExtensions() const
{
    // EXR capability is verified by HDRTextureLoader at preparation. It stays
    // advertised here so the asset contract does not change with a build flag.
    return {".hdr", ".exr"};
}

bool EnvironmentLoader::CanLoad(const std::string& path) const
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character)
                   {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension == ".hdr" || extension == ".exr";
}

IResource* EnvironmentLoader::Load(const std::string& path)
{
    const EnvironmentPreparationState state = CapturePreparationState();
    if (!state.IsValid() || !CanLoad(path))
    {
        return nullptr;
    }

    // Compatibility-only synchronous path. Unlike Prepare this marks the
    // resulting resources ready, but still avoids ResourceManager/cache access.
    HDRTextureLoader loader(nullptr, false);
    const IBLData ibl = loader.LoadIBL(path, state.hdrOptions);
    if (!ibl.IsValid())
    {
        return nullptr;
    }

    TextureHandle environment(ibl.environmentMap);
    TextureHandle irradiance(ibl.irradianceMap);
    TextureHandle prefiltered(ibl.prefilteredMap);
    TextureHandle brdfLUT(ibl.brdfLUT);

    const std::string identity = CanonicalizeAssetPath(path);
    if (identity.empty())
    {
        return nullptr;
    }
    AssignPreparedIdentity(*environment, identity + "#environment", path, "Environment");
    AssignPreparedIdentity(*irradiance, identity + "#irradiance", path, "Irradiance");
    AssignPreparedIdentity(*prefiltered, identity + "#prefiltered", path, "Prefiltered");
    AssignPreparedIdentity(*brdfLUT, identity + "#brdf_lut", path, "BRDF_LUT");

    EnvironmentResourceData data;
    data.sourcePath = path;
    data.environment = std::move(environment);
    data.irradiance = std::move(irradiance);
    data.prefiltered = std::move(prefiltered);
    data.brdfLUT = std::move(brdfLUT);
    data.environmentResolution = data.environment->GetWidth();
    data.irradianceResolution = data.irradiance->GetWidth();
    data.prefilteredResolution = data.prefiltered->GetWidth();
    data.prefilteredMipLevels = data.prefiltered->GetMipLevels();
    data.brdfLUTResolution = data.brdfLUT->GetWidth();
    data.intensity = state.hdrOptions.exposure;

    auto* resource = new EnvironmentResource();
    resource->SetId(GenerateResourceId(identity));
    resource->SetPath(path);
    resource->SetName(std::filesystem::path(path).stem().string());
    if (!resource->SetData(std::move(data)))
    {
        delete resource;
        return nullptr;
    }
    return resource;
}

bool EnvironmentLoader::Prepare(const ResourceLoadPreparationContext& context,
                                PreparedResourceBundle& outBundle,
                                ResourceLoadError& outError)
{
    const auto state = std::dynamic_pointer_cast<const EnvironmentPreparationState>(context.loaderState);
    if (!state)
    {
        outError = {ResourceLoadErrorCode::InvalidRequest,
                    "Environment preparation requires an immutable admission state."};
        return false;
    }
    return Prepare(context, *state, outBundle, outError);
}

bool EnvironmentLoader::Prepare(const ResourceLoadPreparationContext& context,
                                const EnvironmentPreparationState& state,
                                PreparedResourceBundle& outBundle,
                                ResourceLoadError& outError) const
{
    if (context.IsCancellationRequested())
    {
        outError = {ResourceLoadErrorCode::Cancelled,
                    "Environment load was cancelled before preparation."};
        return false;
    }
    if (context.assetKey.resourceType != EnvironmentResource::StaticResourceType ||
        context.rootResourceId == InvalidResourceId || !state.IsValid())
    {
        outError = InvalidOptionsError();
        return false;
    }
    if (context.assetKey.importOptionsHash != state.importOptionsHash)
    {
        outError = {ResourceLoadErrorCode::InvalidRequest,
                    "Environment preparation state does not match the admitted importOptionsHash."};
        return false;
    }
    if (!CanLoad(context.resolvedPath))
    {
        outError = {ResourceLoadErrorCode::InvalidRequest,
                    "Environment loader accepts only HDR or EXR source paths."};
        return false;
    }

    // A manager-null, prepare-only HDR loader cannot cache, notify, touch a
    // Scene, or reach the render upload gateway.
    HDRTextureLoader hdrLoader(nullptr, true);
    const IBLData ibl = hdrLoader.LoadIBL(
        context.resolvedPath,
        state.hdrOptions,
        context.isCancellationRequested);
    // IBLData is the legacy raw-pointer result. Adopt it before observing a
    // late cancellation so a completed prepare-only bake cannot leak if the
    // last subscriber cancels between the final convolution and this check.
    TextureHandle environment(ibl.environmentMap);
    TextureHandle irradiance(ibl.irradianceMap);
    TextureHandle prefiltered(ibl.prefilteredMap);
    TextureHandle brdfLUT(ibl.brdfLUT);
    if (context.IsCancellationRequested())
    {
        outError = {ResourceLoadErrorCode::Cancelled,
                    "Environment load was cancelled during IBL preparation."};
        return false;
    }
    if (!environment || !irradiance || !prefiltered || !brdfLUT)
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "HDR loader could not create the required environment IBL textures."};
        return false;
    }

    AssignPreparedIdentity(*environment,
                           BuildDependencyIdentity(context, "environment"),
                           context.resolvedPath,
                           "Environment");
    AssignPreparedIdentity(*irradiance,
                           BuildDependencyIdentity(context, "irradiance"),
                           context.resolvedPath,
                           "Irradiance");
    AssignPreparedIdentity(*prefiltered,
                           BuildDependencyIdentity(context, "prefiltered"),
                           context.resolvedPath,
                           "Prefiltered");
    AssignPreparedIdentity(*brdfLUT,
                           BuildDependencyIdentity(context, "brdf_lut"),
                           context.resolvedPath,
                           "BRDF_LUT");

    EnvironmentResourceData data;
    data.sourcePath = context.resolvedPath;
    data.environment = environment;
    data.irradiance = irradiance;
    data.prefiltered = prefiltered;
    data.brdfLUT = brdfLUT;
    data.environmentResolution = environment->GetWidth();
    data.irradianceResolution = irradiance->GetWidth();
    data.prefilteredResolution = prefiltered->GetWidth();
    data.prefilteredMipLevels = prefiltered->GetMipLevels();
    data.brdfLUTResolution = brdfLUT->GetWidth();
    data.intensity = state.hdrOptions.exposure;

    EnvironmentHandle root(new EnvironmentResource());
    root->SetId(context.rootResourceId);
    root->SetPath(context.requestedPath);
    root->SetName(std::filesystem::path(context.requestedPath).stem().string());
    if (!root->SetPreparedData(std::move(data)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Environment resource rejected the prepared IBL dependency set."};
        return false;
    }

    if (!outBundle.AddDependency(ResourceHandle<IResource>(environment)) ||
        !outBundle.AddDependency(ResourceHandle<IResource>(irradiance)) ||
        !outBundle.AddDependency(ResourceHandle<IResource>(prefiltered)) ||
        !outBundle.AddDependency(ResourceHandle<IResource>(brdfLUT)) ||
        !outBundle.SetRoot(ResourceHandle<IResource>(root)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Environment loader could not form a dependency-first prepared bundle."};
        return false;
    }
    return true;
}

void EnvironmentLoader::SetOptions(EnvironmentLoadOptions options)
{
    std::lock_guard<std::mutex> lock(m_optionsMutex);
    m_options = options;
}

EnvironmentLoadOptions EnvironmentLoader::GetOptions() const
{
    std::lock_guard<std::mutex> lock(m_optionsMutex);
    return m_options;
}

EnvironmentPreparationState EnvironmentLoader::CapturePreparationState() const
{
    const EnvironmentLoadOptions options = GetOptions();
    EnvironmentPreparationState state;
    if (!ValidateOptions(options))
    {
        return state;
    }

    state.hdrOptions = ResolveHDRIBLQualityProfile(options.quality,
                                                    options.exposure,
                                                    options.applyGamma);
    if (!ValidateHDRLoadOptions(state.hdrOptions))
    {
        return {};
    }
    state.importOptionsHash = ComputeImportOptionsHash(options);
    return state;
}

uint64 EnvironmentLoader::ComputeImportOptionsHash(const EnvironmentLoadOptions& options)
{
    if (!ValidateOptions(options))
    {
        return 0;
    }

    uint64 hash = FNVOffsetBasis;
    HashInteger(hash, EnvironmentLoaderSchemaVersion);
    HashInteger(hash, static_cast<uint32>(options.quality));
    HashInteger(hash, std::bit_cast<uint32>(options.exposure));
    HashInteger(hash, options.applyGamma ? uint8{1} : uint8{0});
    return hash == 0 ? 1 : hash;
}

bool EnvironmentLoader::CapturePreparationState(
    uint64 requestedImportOptionsHash,
    ResourceLoadPreparationStateRef& outState,
    uint64& outCanonicalImportOptionsHash,
    ResourceLoadError& outError) const
{
    const EnvironmentPreparationState captured = CapturePreparationState();
    if (!captured.IsValid())
    {
        outState.reset();
        outCanonicalImportOptionsHash = 0;
        outError = InvalidOptionsError();
        return false;
    }
    if (requestedImportOptionsHash != 0 &&
        requestedImportOptionsHash != captured.importOptionsHash)
    {
        outState.reset();
        outCanonicalImportOptionsHash = 0;
        outError = {ResourceLoadErrorCode::InvalidRequest,
                    "Requested environment importOptionsHash does not match the captured loader state."};
        return false;
    }

    outState = std::make_shared<EnvironmentPreparationState>(captured);
    outCanonicalImportOptionsHash = captured.importOptionsHash;
    outError = {};
    return true;
}

bool EnvironmentLoader::ValidateOptions(const EnvironmentLoadOptions& options)
{
    return options.quality <= HDRIBLQualityProfile::High &&
           std::isfinite(options.exposure) && options.exposure > 0.0f;
}

bool EnvironmentLoader::ValidateHDRLoadOptions(const HDRLoadOptions& options)
{
    return options.generateCubemap && options.generateIBL &&
           options.cubemapResolution != 0 && options.irradianceResolution != 0 &&
           options.prefilteredResolution != 0 && options.prefilteredMipLevels != 0 &&
           options.brdfLUTResolution != 0 && options.convolutionSamples != 0 &&
           std::isfinite(options.exposure) && options.exposure > 0.0f;
}

std::string EnvironmentLoader::BuildDependencyIdentity(
    const ResourceLoadPreparationContext& context,
    const char* suffix)
{
    const std::string& base = context.resourceIdentityPath.empty()
        ? context.assetKey.canonicalPath
        : context.resourceIdentityPath;
    return base + "#environment_" + suffix;
}

void EnvironmentLoader::AssignPreparedIdentity(TextureResource& texture,
                                                const std::string& identity,
                                                const std::string& sourcePath,
                                                const char* displayName)
{
    texture.SetId(GenerateResourceId(identity));
    texture.SetPath(sourcePath + "#" + displayName);
    texture.SetName(displayName);
}
} // namespace RVX::Resource
