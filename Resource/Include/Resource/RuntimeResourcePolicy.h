#pragma once

/**
 * @file RuntimeResourcePolicy.h
 * @brief Runtime resource path policy and load diagnostics
 */

#include "Core/App/AppMode.h"
#include "Core/Types.h"
#include <string>
#include <vector>

namespace RVX::Resource
{
    inline constexpr const char* RVX_RESOURCE_LOAD_DIAGNOSTIC_SCHEMA_ID =
        "RVX.Resource.LoadDiagnostic";
    inline constexpr uint32 RVX_RESOURCE_LOAD_DIAGNOSTIC_SCHEMA_VERSION = 1;

    enum class ResourceRuntimeMode : uint8
    {
        Editor = 0,
        CookedRuntime,
        PackagedRuntime,
    };

    enum class ResourceLoadDomain : uint8
    {
        Unknown = 0,
        SourceAsset,
        CookedArtifact,
        RuntimePackage,
    };

    enum class ResourceLoadFailureCode : uint8
    {
        None = 0,
        EmptyPath,
        UnsupportedScheme,
        SourceAssetsDisabled,
        CookedArtifactRequired,
        RuntimePackageRequired,
        InvalidPackagePath,
        PackageRootMissing,
        PackageMountMissing,
        PackageArtifactMissing,
        PackageArtifactHashMismatch,
        LoaderUnavailable,
        LoaderFailed,
        PathEscapesRoot,
        ResourceRootMissing,
    };

    struct ResourcePackageArtifact
    {
        std::string logicalPath;
        std::string resolvedArtifactPath;
        std::string contentHash;
    };

    struct ResourcePackageMount
    {
        std::string packageName;
        int32 mountPriority = 0;
        std::string mountRoot;
        std::vector<ResourcePackageArtifact> artifacts;
    };

    struct ResourceRuntimePolicy
    {
        ResourceRuntimeMode mode = ResourceRuntimeMode::Editor;
        bool allowSourceAssetReads = true;
        bool requireCookedArtifacts = false;
        bool requireRuntimePackage = false;
        std::string sourceRoot;
        std::string cookedRoot;
        std::string packageRoot;
        std::vector<ResourcePackageMount> packageMounts;
    };

    struct ResourcePathResolution
    {
        bool allowed = false;
        ResourceLoadDomain domain = ResourceLoadDomain::Unknown;
        ResourceLoadFailureCode failure = ResourceLoadFailureCode::None;
        std::string requestedPath;
        std::string logicalPath;
        std::string resolvedPath;
        std::string packageName;
        int32 packageMountPriority = 0;
        std::string packageLogicalPath;
        std::string packageArtifactPath;
        std::string packageExpectedContentHash;
        std::string packageActualContentHash;
        std::string diagnosticMessage;
        bool sourceAssetRead = false;
        bool cookedArtifactRead = false;
        bool runtimePackageRead = false;
        bool packageHashChecked = false;
        bool packageHashMatched = false;
    };

    struct ResourceLoadDiagnostic
    {
        bool attempted = false;
        bool success = false;
        ResourceLoadDomain domain = ResourceLoadDomain::Unknown;
        ResourceLoadFailureCode failure = ResourceLoadFailureCode::None;
        std::string requestedPath;
        std::string resolvedPath;
        std::string packageName;
        int32 packageMountPriority = 0;
        std::string packageLogicalPath;
        std::string packageArtifactPath;
        std::string packageExpectedContentHash;
        std::string packageActualContentHash;
        std::string message;
        bool sourceAssetRead = false;
        bool cookedArtifactRead = false;
        bool runtimePackageRead = false;
        bool packageHashChecked = false;
        bool packageHashMatched = false;
    };

    const char* GetResourceLoadDomainName(ResourceLoadDomain domain);
    const char* GetResourceLoadFailureCodeName(ResourceLoadFailureCode code);

    std::string ExportResourceLoadDiagnosticJson(const ResourceLoadDiagnostic& diagnostic);
    bool SaveResourceLoadDiagnosticJson(const char* filename, const ResourceLoadDiagnostic& diagnostic);

    ResourceRuntimePolicy MakeResourceRuntimePolicyForAppMode(AppMode mode);

    ResourcePathResolution ResolveRuntimeResourcePath(const ResourceRuntimePolicy& policy,
                                                      const std::string& basePath,
                                                      const std::string& path);

} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceLoadDiagnostic;
    using Resource::ResourceLoadDomain;
    using Resource::ResourceLoadFailureCode;
    using Resource::ExportResourceLoadDiagnosticJson;
    using Resource::ResourcePackageArtifact;
    using Resource::ResourcePackageMount;
    using Resource::ResourcePathResolution;
    using Resource::ResourceRuntimeMode;
    using Resource::ResourceRuntimePolicy;
    using Resource::SaveResourceLoadDiagnosticJson;
}
