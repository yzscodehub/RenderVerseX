#pragma once

/**
 * @file RuntimeResourcePolicy.h
 * @brief Runtime resource path policy and load diagnostics
 */

#include "Core/App/AppMode.h"
#include "Core/Types.h"
#include <string>

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
        LoaderUnavailable,
        LoaderFailed,
        PathEscapesRoot,
        ResourceRootMissing,
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
    };

    struct ResourcePathResolution
    {
        bool allowed = false;
        ResourceLoadDomain domain = ResourceLoadDomain::Unknown;
        ResourceLoadFailureCode failure = ResourceLoadFailureCode::None;
        std::string requestedPath;
        std::string logicalPath;
        std::string resolvedPath;
        std::string diagnosticMessage;
        bool sourceAssetRead = false;
        bool cookedArtifactRead = false;
        bool runtimePackageRead = false;
    };

    struct ResourceLoadDiagnostic
    {
        bool attempted = false;
        bool success = false;
        ResourceLoadDomain domain = ResourceLoadDomain::Unknown;
        ResourceLoadFailureCode failure = ResourceLoadFailureCode::None;
        std::string requestedPath;
        std::string resolvedPath;
        std::string message;
        bool sourceAssetRead = false;
        bool cookedArtifactRead = false;
        bool runtimePackageRead = false;
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
    using Resource::ResourcePathResolution;
    using Resource::ResourceRuntimeMode;
    using Resource::ResourceRuntimePolicy;
    using Resource::SaveResourceLoadDiagnosticJson;
}
