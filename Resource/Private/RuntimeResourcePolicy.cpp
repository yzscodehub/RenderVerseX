#include "Resource/RuntimeResourcePolicy.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace RVX::Resource
{
    namespace
    {
        std::string ToLower(std::string value)
        {
            std::transform(value.begin(),
                           value.end(),
                           value.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        bool HasScheme(const std::string& path)
        {
            return path.find("://") != std::string::npos;
        }

        std::string ResolveAgainstRoot(const std::string& root, const std::string& path)
        {
            std::filesystem::path resourcePath(path);
            if (resourcePath.is_absolute() || root.empty())
            {
                resourcePath.make_preferred();
                return resourcePath.string();
            }

            resourcePath.make_preferred();
            std::filesystem::path resolvedPath = (std::filesystem::path(root) / resourcePath).lexically_normal();
            resolvedPath.make_preferred();
            return resolvedPath.string();
        }

        std::string GetExtensionLower(const std::string& path)
        {
            std::string extension = std::filesystem::path(path).extension().string();
            return ToLower(extension);
        }

        ResourcePathResolution Deny(ResourcePathResolution resolution,
                                    ResourceLoadFailureCode failure,
                                    const std::string& message)
        {
            resolution.allowed = false;
            resolution.failure = failure;
            resolution.diagnosticMessage = message;
            return resolution;
        }

        void MarkDomainFlags(ResourcePathResolution& resolution)
        {
            resolution.sourceAssetRead = resolution.domain == ResourceLoadDomain::SourceAsset;
            resolution.cookedArtifactRead = resolution.domain == ResourceLoadDomain::CookedArtifact;
            resolution.runtimePackageRead = resolution.domain == ResourceLoadDomain::RuntimePackage;
        }

        const char* JsonBool(bool value)
        {
            return value ? "true" : "false";
        }

        std::string JsonString(std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size() + 2);
            escaped.push_back('"');
            for (char ch : value)
            {
                switch (ch)
                {
                    case '\\':
                        escaped += "\\\\";
                        break;
                    case '"':
                        escaped += "\\\"";
                        break;
                    case '\n':
                        escaped += "\\n";
                        break;
                    case '\r':
                        escaped += "\\r";
                        break;
                    case '\t':
                        escaped += "\\t";
                        break;
                    default:
                        escaped.push_back(ch);
                        break;
                }
            }
            escaped.push_back('"');
            return escaped;
        }
    } // namespace

    const char* GetResourceLoadDomainName(ResourceLoadDomain domain)
    {
        switch (domain)
        {
            case ResourceLoadDomain::Unknown: return "Unknown";
            case ResourceLoadDomain::SourceAsset: return "SourceAsset";
            case ResourceLoadDomain::CookedArtifact: return "CookedArtifact";
            case ResourceLoadDomain::RuntimePackage: return "RuntimePackage";
            default: return "Invalid";
        }
    }

    const char* GetResourceLoadFailureCodeName(ResourceLoadFailureCode code)
    {
        switch (code)
        {
            case ResourceLoadFailureCode::None: return "None";
            case ResourceLoadFailureCode::EmptyPath: return "EmptyPath";
            case ResourceLoadFailureCode::UnsupportedScheme: return "UnsupportedScheme";
            case ResourceLoadFailureCode::SourceAssetsDisabled: return "SourceAssetsDisabled";
            case ResourceLoadFailureCode::CookedArtifactRequired: return "CookedArtifactRequired";
            case ResourceLoadFailureCode::RuntimePackageRequired: return "RuntimePackageRequired";
            case ResourceLoadFailureCode::InvalidPackagePath: return "InvalidPackagePath";
            case ResourceLoadFailureCode::PackageRootMissing: return "PackageRootMissing";
            case ResourceLoadFailureCode::LoaderUnavailable: return "LoaderUnavailable";
            case ResourceLoadFailureCode::LoaderFailed: return "LoaderFailed";
            default: return "Invalid";
        }
    }

    std::string ExportResourceLoadDiagnosticJson(const ResourceLoadDiagnostic& diagnostic)
    {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"schemaVersion\": " << RVX_RESOURCE_LOAD_DIAGNOSTIC_SCHEMA_VERSION << ",\n";
        ss << "  \"schemaId\": " << JsonString(RVX_RESOURCE_LOAD_DIAGNOSTIC_SCHEMA_ID) << ",\n";
        ss << "  \"id\": \"resourceLoadDiagnosticJson\",\n";
        ss << "  \"kind\": \"ResourceLoadDiagnosticJson\",\n";
        ss << "  \"contentType\": \"application/json\",\n";
        ss << "  \"attempted\": " << JsonBool(diagnostic.attempted) << ",\n";
        ss << "  \"success\": " << JsonBool(diagnostic.success) << ",\n";
        ss << "  \"domain\": " << JsonString(GetResourceLoadDomainName(diagnostic.domain)) << ",\n";
        ss << "  \"domainCode\": " << static_cast<uint32>(diagnostic.domain) << ",\n";
        ss << "  \"failure\": " << JsonString(GetResourceLoadFailureCodeName(diagnostic.failure)) << ",\n";
        ss << "  \"failureCode\": " << static_cast<uint32>(diagnostic.failure) << ",\n";
        ss << "  \"requestedPath\": " << JsonString(diagnostic.requestedPath) << ",\n";
        ss << "  \"resolvedPath\": " << JsonString(diagnostic.resolvedPath) << ",\n";
        ss << "  \"message\": " << JsonString(diagnostic.message) << ",\n";
        ss << "  \"sourceAssetRead\": " << JsonBool(diagnostic.sourceAssetRead) << ",\n";
        ss << "  \"cookedArtifactRead\": " << JsonBool(diagnostic.cookedArtifactRead) << ",\n";
        ss << "  \"runtimePackageRead\": " << JsonBool(diagnostic.runtimePackageRead) << "\n";
        ss << "}\n";
        return ss.str();
    }

    bool SaveResourceLoadDiagnosticJson(const char* filename, const ResourceLoadDiagnostic& diagnostic)
    {
        if (!filename || filename[0] == '\0')
        {
            return false;
        }

        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        file << ExportResourceLoadDiagnosticJson(diagnostic);
        return file.good();
    }

    ResourceRuntimePolicy MakeResourceRuntimePolicyForAppMode(AppMode mode)
    {
        const AppModeTraits traits = GetAppModeTraits(mode);

        ResourceRuntimePolicy policy;
        policy.mode = traits.runtimeExecutionEnabled
            ? ResourceRuntimeMode::CookedRuntime
            : ResourceRuntimeMode::Editor;
        policy.allowSourceAssetReads = traits.sourceAssetAccessAllowed;
        policy.requireCookedArtifacts = traits.cookedRuntimeArtifactsRequired;
        policy.requireRuntimePackage = false;
        return policy;
    }

    ResourcePathResolution ResolveRuntimeResourcePath(const ResourceRuntimePolicy& policy,
                                                      const std::string& basePath,
                                                      const std::string& path)
    {
        ResourcePathResolution resolution;
        resolution.requestedPath = path;

        if (path.empty())
        {
            return Deny(resolution, ResourceLoadFailureCode::EmptyPath, "Resource path is empty.");
        }

        std::string scheme;
        std::string logicalPath = path;

        const size_t schemeSeparator = path.find("://");
        if (schemeSeparator != std::string::npos)
        {
            scheme = ToLower(path.substr(0, schemeSeparator));
            logicalPath = path.substr(schemeSeparator + 3);
        }

        resolution.logicalPath = logicalPath;

        if (scheme.empty())
        {
            resolution.domain = GetExtensionLower(path) == ".rva"
                ? ResourceLoadDomain::CookedArtifact
                : ResourceLoadDomain::SourceAsset;
        }
        else if (scheme == "source")
        {
            resolution.domain = ResourceLoadDomain::SourceAsset;
        }
        else if (scheme == "cooked")
        {
            resolution.domain = ResourceLoadDomain::CookedArtifact;
        }
        else if (scheme == "package")
        {
            resolution.domain = ResourceLoadDomain::RuntimePackage;
        }
        else
        {
            return Deny(resolution,
                        ResourceLoadFailureCode::UnsupportedScheme,
                        "Unsupported resource path scheme: " + scheme);
        }

        MarkDomainFlags(resolution);

        if (policy.requireRuntimePackage && resolution.domain != ResourceLoadDomain::RuntimePackage)
        {
            return Deny(resolution,
                        ResourceLoadFailureCode::RuntimePackageRequired,
                        "Runtime package access is required by the active resource policy.");
        }

        if (policy.requireCookedArtifacts && resolution.domain == ResourceLoadDomain::SourceAsset)
        {
            return Deny(resolution,
                        ResourceLoadFailureCode::CookedArtifactRequired,
                        "Cooked artifact access is required by the active resource policy.");
        }

        if (resolution.domain == ResourceLoadDomain::SourceAsset && !policy.allowSourceAssetReads)
        {
            return Deny(resolution,
                        ResourceLoadFailureCode::SourceAssetsDisabled,
                        "Source asset reads are disabled by the active resource policy.");
        }

        if (resolution.domain == ResourceLoadDomain::RuntimePackage)
        {
            if (logicalPath.empty() || logicalPath == "." || HasScheme(logicalPath))
            {
                return Deny(resolution,
                            ResourceLoadFailureCode::InvalidPackagePath,
                            "Runtime package path must include a package and entry path.");
            }

            if (policy.packageRoot.empty())
            {
                return Deny(resolution,
                            ResourceLoadFailureCode::PackageRootMissing,
                            "Runtime package root is not mounted.");
            }

            resolution.resolvedPath = ResolveAgainstRoot(policy.packageRoot, logicalPath);
        }
        else if (resolution.domain == ResourceLoadDomain::CookedArtifact)
        {
            const std::string& cookedRoot = policy.cookedRoot.empty() ? basePath : policy.cookedRoot;
            resolution.resolvedPath = ResolveAgainstRoot(cookedRoot, logicalPath);
        }
        else
        {
            const std::string& sourceRoot = policy.sourceRoot.empty() ? basePath : policy.sourceRoot;
            resolution.resolvedPath = ResolveAgainstRoot(sourceRoot, logicalPath);
        }

        resolution.allowed = true;
        resolution.failure = ResourceLoadFailureCode::None;
        resolution.diagnosticMessage = "Resource path resolved.";
        return resolution;
    }
} // namespace RVX::Resource
