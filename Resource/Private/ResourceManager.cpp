#include "Resource/ResourceManager.h"
#include "Resource/HotReloadManager.h"
#include "Resource/Loader/EnvironmentLoader.h"
#include "Resource/Loader/MeshLoader.h"
#include "Resource/Loader/ModelLoader.h"
#include "Resource/Loader/ShaderLoader.h"
#include "Resource/Loader/TextureLoader.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/TextureResource.h"
#include "Core/Assert.h"
#include "Core/Diagnostics/JsonWriter.h"
#include "Core/Log.h"
#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

// Logging macros
#ifndef RVX_RESOURCE_INFO
#define RVX_RESOURCE_INFO(...)  RVX_CORE_INFO(__VA_ARGS__)
#define RVX_RESOURCE_WARN(...)  RVX_CORE_WARN(__VA_ARGS__)
#define RVX_RESOURCE_ERROR(...) RVX_CORE_ERROR(__VA_ARGS__)
#define RVX_RESOURCE_DEBUG(...) RVX_CORE_DEBUG(__VA_ARGS__)
#endif

namespace RVX::Resource
{

static ResourceManager* s_instance = nullptr;

namespace
{
    using Diagnostics::JsonBool;
    using Diagnostics::JsonString;

    void AppendJsonString(std::ostringstream& json,
                          std::string_view name,
                          std::string_view value,
                          bool trailingComma = true)
    {
        json << "  \"" << name << "\": " << JsonString(value);
        json << (trailingComma ? ",\n" : "\n");
    }

    void AppendJsonBool(std::ostringstream& json,
                        std::string_view name,
                        bool value,
                        bool trailingComma = true)
    {
        json << "  \"" << name << "\": " << JsonBool(value);
        json << (trailingComma ? ",\n" : "\n");
    }

    void AppendJsonUInt64(std::ostringstream& json,
                          std::string_view name,
                          uint64 value,
                          bool trailingComma = true)
    {
        json << "  \"" << name << "\": " << value;
        json << (trailingComma ? ",\n" : "\n");
    }

    ResourceType DetectCookedArtifactType(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return ResourceType::Unknown;
        }

        std::string firstLine;
        std::getline(file, firstLine);

        if (firstLine == "RVX_TEXTURE_PREBAKE_V1")
        {
            return ResourceType::Texture;
        }
        if (firstLine == "RVX_MESH_PREBAKE_V1")
        {
            return ResourceType::Mesh;
        }
        if (firstLine == "RVX_SHADER_PREBAKE_V1")
        {
            return ResourceType::Shader;
        }

        return ResourceType::Unknown;
    }

    ResourceLoadDiagnostic MakeLoadDiagnostic(const ResourcePathResolution& resolution,
                                              bool success,
                                              ResourceLoadFailureCode failure,
                                              const std::string& message)
    {
        ResourceLoadDiagnostic diagnostic;
        diagnostic.attempted = true;
        diagnostic.success = success;
        diagnostic.domain = resolution.domain;
        diagnostic.failure = failure;
        diagnostic.requestedPath = resolution.requestedPath;
        diagnostic.resolvedPath = resolution.resolvedPath;
        diagnostic.packageName = resolution.packageName;
        diagnostic.packageMountPriority = resolution.packageMountPriority;
        diagnostic.packageLogicalPath = resolution.packageLogicalPath;
        diagnostic.packageArtifactPath = resolution.packageArtifactPath;
        diagnostic.packageExpectedContentHash = resolution.packageExpectedContentHash;
        diagnostic.packageActualContentHash = resolution.packageActualContentHash;
        diagnostic.message = message;
        diagnostic.sourceAssetRead = resolution.sourceAssetRead;
        diagnostic.cookedArtifactRead = resolution.cookedArtifactRead;
        diagnostic.runtimePackageRead = resolution.runtimePackageRead;
        diagnostic.packageHashChecked = resolution.packageHashChecked;
        diagnostic.packageHashMatched = resolution.packageHashMatched;
        return diagnostic;
    }

    ResourceType DetectResourceType(const std::string& resolvedPath)
    {
        const std::string extension = std::filesystem::path(resolvedPath).extension().string();
        ResourceType type = ResourceManager::GetTypeFromExtension(extension);
        if (type != ResourceType::Unknown)
        {
            return type;
        }

        std::string lowerExtension = extension;
        std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(), ::tolower);
        return lowerExtension == ".rva" ? DetectCookedArtifactType(resolvedPath)
                                        : ResourceType::Unknown;
    }

    uint64 GetCurrentThreadToken()
    {
        return static_cast<uint64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    std::string BuildAssetResourceIdentity(const AssetKey& assetKey)
    {
        // Preserve the established path-only ResourceId for the default
        // import profile, so synchronous and asynchronous path loads share
        // exactly one cache entry. Non-default output-affecting inputs get a
        // deterministic suffix and therefore cannot overwrite that entry.
        if (assetKey.importOptionsHash == 0 &&
            assetKey.platformProfileHash == 0 &&
            assetKey.loaderSchemaVersion == 1)
        {
            return assetKey.canonicalPath;
        }

        return assetKey.canonicalPath + "#rvx_asset_" +
               std::to_string(static_cast<uint32>(assetKey.resourceType)) + "_" +
               std::to_string(assetKey.importOptionsHash) + "_" +
               std::to_string(assetKey.platformProfileHash) + "_" +
               std::to_string(assetKey.loaderSchemaVersion);
    }

    ResourceId GetResourceIdForAssetKey(const AssetKey& assetKey)
    {
        return GenerateResourceId(BuildAssetResourceIdentity(assetKey));
    }

    ResourceId GetCanonicalResolvedResourceId(const ResourcePathResolution& resolution)
    {
        return GenerateResourceId(CanonicalizeAssetPath(resolution.resolvedPath));
    }
} // namespace

ResourceManager::ResourceManager() = default;

ResourceManager::~ResourceManager()
{
    RVX_ASSERT_MSG(!m_initialized || IsOwnerThread(),
                   "ResourceManager must be destroyed on its owner/update thread");
    Shutdown();
}

const char* GetResourceHotReloadStatusName(ResourceHotReloadStatus status)
{
    switch (status)
    {
        case ResourceHotReloadStatus::Disabled: return "Disabled";
        case ResourceHotReloadStatus::Active: return "Active";
        case ResourceHotReloadStatus::UnsupportedRuntimePolicy: return "UnsupportedRuntimePolicy";
        case ResourceHotReloadStatus::Uninitialized: return "Uninitialized";
        default: return "Unknown";
    }
}

std::string ExportResourceHotReloadDiagnosticJson(const ResourceHotReloadDiagnostic& diagnostic)
{
    std::ostringstream json;
    json << "{\n";
    AppendJsonUInt64(json, "schemaVersion", RVX_RESOURCE_HOT_RELOAD_DIAGNOSTIC_SCHEMA_VERSION);
    AppendJsonString(json, "schemaId", RVX_RESOURCE_HOT_RELOAD_DIAGNOSTIC_SCHEMA_ID);
    AppendJsonString(json, "id", "resourceHotReloadDiagnosticJson");
    AppendJsonString(json, "kind", "ResourceHotReloadDiagnosticJson");
    AppendJsonString(json, "contentType", "application/json");
    AppendJsonString(json, "contentHash", "");
    AppendJsonString(json, "relativePath", "");
    AppendJsonBool(json, "requested", diagnostic.requested);
    AppendJsonBool(json, "enabled", diagnostic.enabled);
    AppendJsonBool(json, "sourceAssetAccessRequired", diagnostic.sourceAssetAccessRequired);
    AppendJsonBool(json, "watcherInitialized", diagnostic.watcherInitialized);
    AppendJsonString(json, "status", GetResourceHotReloadStatusName(diagnostic.status));
    AppendJsonUInt64(json, "statusCode", static_cast<uint32>(diagnostic.status));
    AppendJsonString(json, "message", diagnostic.message);
    AppendJsonUInt64(json, "watchedFileCount", static_cast<uint64>(diagnostic.watchedFileCount));
    AppendJsonUInt64(json,
                     "registeredResourceCount",
                     static_cast<uint64>(diagnostic.registeredResourceCount));
    AppendJsonUInt64(json, "pendingReloadCount", static_cast<uint64>(diagnostic.pendingReloadCount));
    AppendJsonUInt64(json, "totalReloadCount", static_cast<uint64>(diagnostic.totalReloadCount));
    AppendJsonUInt64(json,
                     "successfulReloadCount",
                     static_cast<uint64>(diagnostic.successfulReloadCount));
    AppendJsonUInt64(json,
                     "failedReloadCount",
                     static_cast<uint64>(diagnostic.failedReloadCount),
                     false);
    json << "}\n";
    return json.str();
}

bool SaveResourceHotReloadDiagnosticJson(const char* filename,
                                         const ResourceHotReloadDiagnostic& diagnostic)
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return false;
    }

    file << ExportResourceHotReloadDiagnosticJson(diagnostic);
    return file.good();
}

ResourceManager& ResourceManager::Get()
{
    if (!s_instance)
    {
        s_instance = new ResourceManager();
    }
    return *s_instance;
}

void ResourceManager::Initialize(const ResourceManagerConfig& config)
{
    if (m_initialized)
    {
        RVX_RESOURCE_WARN("ResourceManager already initialized");
        return;
    }

    m_config = config;
    m_ownerThreadToken = GetCurrentThreadToken();
    Diagnostics::TraceSpan initializationSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "Resource.Manager.Initialize");
    m_registry = std::make_unique<ResourceRegistry>();
    m_cache = std::make_unique<ResourceCache>(config.cacheConfig);
    m_cache->SetBeforeRemoveCallback(
        [this](IResource* resource)
        {
            QueueLifecycleEvent(ResourceLifecycleEventType::BeforeUnload,
                                resource);
        });
    m_dependencyGraph = std::make_unique<DependencyGraph>();

    // Register default loaders
    RegisterDefaultLoaders();

    m_initialized = true;
    ConfigureHotReload(config.enableHotReload);
    StartAsyncWorkers();
    // Publish admission only after every object observed by RequestAsync is
    // initialized. This release pairs with request-side acquire checks.
    m_acceptAsyncRequests.store(true, std::memory_order_release);
    initializationSpan.SetAttribute("result", "initialized");
    RVX_RESOURCE_INFO("ResourceManager initialized");
}

void ResourceManager::RegisterDefaultLoaders()
{
    // Register TextureLoader
    auto textureLoader = std::make_unique<TextureLoader>(this);
    RegisterLoader(ResourceType::Texture, std::move(textureLoader));

    // Register MeshLoader
    auto meshLoader = std::make_unique<MeshLoader>(this);
    RegisterLoader(ResourceType::Mesh, std::move(meshLoader));

    // Register ShaderLoader
    auto shaderLoader = std::make_unique<ShaderLoader>(this);
    RegisterLoader(ResourceType::Shader, std::move(shaderLoader));

    // Register ModelLoader
    auto modelLoader = std::make_unique<ModelLoader>(this);
    RegisterLoader(ResourceType::Model, std::move(modelLoader));

    auto environmentLoader = std::make_unique<EnvironmentLoader>();
    RegisterLoader(ResourceType::Environment, std::move(environmentLoader));

    RVX_RESOURCE_INFO("Registered default resource loaders");
}

void ResourceManager::Shutdown()
{
    if (!m_initialized) return;
    if (!IsOwnerThread())
    {
        RVX_RESOURCE_ERROR("ResourceManager::Shutdown must run on its owner/update thread");
        return;
    }

    // Stop accepting new subscriptions before waiting.  Existing workers retain
    // only loader snapshots and prepared data; they cannot reach manager-owned
    // cache/registry state.
    m_acceptAsyncRequests.store(false, std::memory_order_release);

    // Waiting before signalling cancellation can deadlock a cooperative
    // loader blocked in IO/decode. Request cancellation for every operation
    // first; workers retain their loader snapshots and will stage only a
    // cancelled completion.
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        for (auto& [assetKey, operation] : m_inFlightLoads)
        {
            (void)assetKey;
            operation.CompleteCancelled("ResourceManager is shutting down.");
        }
    }

    if (m_hotReloadCallbackId != 0 &&
        HotReloadManager::Get().IsInitialized())
    {
        HotReloadManager::Get().RemoveReloadCallback(m_hotReloadCallbackId);
        m_hotReloadCallbackId = 0;
    }
    if (m_hotReloadInitializedByManager && HotReloadManager::Get().IsInitialized())
    {
        HotReloadManager::Get().Shutdown();
    }
    {
        std::lock_guard<std::mutex> lock(m_hotReloadMutex);
        m_hotReloadWatchIds.clear();
        m_hotReloadInitializedByManager = false;
    }
    SetHotReloadDiagnostic(ResourceHotReloadStatus::Uninitialized,
                           m_config.enableHotReload,
                           false,
                           "ResourceManager is shut down.");

    StopAsyncWorkers();
    DrainPreparedLoadCompletions(true);
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        m_inFlightLoads.clear();
    }
    DrainLegacyAsyncWaiters();
    ProcessCompletedLoads();

    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    m_loaders.clear();
    m_cache->Clear();
    m_registry->Clear();
    m_dependencyGraph->Clear();

    m_initialized = false;
    DrainLifecycleEvents();
    SetLifecycleEventCallback({});
    m_reloadCallback = {};
    RVX_RESOURCE_INFO("ResourceManager shutdown");
}

IResource* ResourceManager::LoadResource(const std::string& path)
{
    return LoadResource(path, ResourceType::Unknown);
}

IResource* ResourceManager::LoadResource(const std::string& path,
                                         ResourceType requestedType)
{
    if (!m_initialized)
    {
        RVX_RESOURCE_ERROR("ResourceManager not initialized");
        return nullptr;
    }

    Diagnostics::TraceSpan resolveSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "AssetResolve",
        {{"requestedPath", path}});
    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, path);
    resolveSpan.SetAttribute("resolvedPath", resolution.resolvedPath);
    resolveSpan.SetAttribute("allowed", resolution.allowed);
    if (!resolution.allowed)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution, false, resolution.failure, resolution.diagnosticMessage));
        RVX_RESOURCE_ERROR("Resource load denied for '{}': {} ({})",
                           path,
                           resolution.diagnosticMessage,
                           GetResourceLoadFailureCodeName(resolution.failure));
        return nullptr;
    }

    const ResourceType detectedType = DetectResourceType(resolution.resolvedPath);
    const ResourceType type = requestedType != ResourceType::Unknown
                                  ? requestedType
                                  : detectedType;

    // Snapshot the loader before deriving the identity. Stateful loaders must
    // contribute their immutable import profile to synchronous cache lookup as
    // well, otherwise sync and prepared paths would create two root meanings.
    std::shared_ptr<IResourceLoader> loader;
    uint64 canonicalImportOptionsHash = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        const auto loaderIt = m_loaders.find(type);
        if (loaderIt != m_loaders.end())
        {
            loader = loaderIt->second;
        }
    }
    if (!loader || !loader->CanLoad(resolution.resolvedPath))
    {
        RVX_RESOURCE_ERROR("No compatible loader registered for resource type: {}",
                           GetResourceTypeName(type));
        return nullptr;
    }

    ResourceLoadPreparationStateRef loaderState;
    ResourceLoadError captureError;
    if (!loader->CapturePreparationState(0,
                                         loaderState,
                                         canonicalImportOptionsHash,
                                         captureError))
    {
        RVX_RESOURCE_ERROR("Resource loader rejected the active import profile for '{}': {}",
                           path,
                           captureError.message);
        return nullptr;
    }
    const AssetKey assetKey = MakeAssetKey(resolution.resolvedPath,
                                           type,
                                           canonicalImportOptionsHash,
                                           0,
                                           1);
    const ResourceId id = GetResourceIdForAssetKey(assetKey);

    // Check cache first
    IResource* cached = m_cache->Get(id);
    Diagnostics::RecordTraceInstant(
        m_config.startupTraceContext,
        "CacheLookup",
        {{"requestedPath", path},
         {"resourceId", id},
         {"cacheHit", cached != nullptr}});
    if (cached != nullptr)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               true,
                               ResourceLoadFailureCode::None,
                               "Resource returned from cache."));
        Diagnostics::RecordTraceInstant(
            m_config.startupTraceContext,
            "CacheHit",
            {{"requestedPath", path}, {"resourceId", id}});
        return cached;
    }

    if (loader->SupportsPreparedLoading())
    {
        return LoadPreparedSynchronously(resolution,
                                         path,
                                         assetKey,
                                         std::move(loader),
                                         std::move(loaderState));
    }

    return LoadInternal(path, resolution, type, id, std::move(loader));
}

IResource* ResourceManager::LoadResource(ResourceId id)
{
    if (!m_initialized) return nullptr;

    Diagnostics::TraceSpan resolveSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "AssetResolve",
        {{"resourceId", id}});

    // Check cache first
    IResource* cached = m_cache->Get(id);
    Diagnostics::RecordTraceInstant(
        m_config.startupTraceContext,
        "CacheLookup",
        {{"resourceId", id}, {"cacheHit", cached != nullptr}});
    if (cached != nullptr)
    {
        Diagnostics::RecordTraceInstant(
            m_config.startupTraceContext,
            "CacheHit",
            {{"resourceId", id}});
        return cached;
    }

    // Look up path in registry
    auto metadata = m_registry->FindById(id);
    if (!metadata)
    {
        RVX_RESOURCE_ERROR("Resource not found in registry: {}", id);
        return nullptr;
    }

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, metadata->path);
    resolveSpan.SetAttribute("requestedPath", metadata->path);
    resolveSpan.SetAttribute("resolvedPath", resolution.resolvedPath);
    resolveSpan.SetAttribute("allowed", resolution.allowed);
    if (!resolution.allowed)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution, false, resolution.failure, resolution.diagnosticMessage));
        RVX_RESOURCE_ERROR("Resource load denied for '{}': {} ({})",
                           metadata->path,
                           resolution.diagnosticMessage,
                           GetResourceLoadFailureCodeName(resolution.failure));
        return nullptr;
    }

    // ResourceId identifies an exact AssetKey variant, not merely a source
    // path. Registry metadata from older schemas does not retain the immutable
    // loader payload for arbitrary import/platform profiles. Recreate the
    // currently active loader identity and fail closed when it cannot produce
    // the requested id; silently applying today's options to an old variant id
    // would corrupt the cache's identity contract.
    std::shared_ptr<IResourceLoader> loader;
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        const auto loaderIt = m_loaders.find(metadata->type);
        if (loaderIt != m_loaders.end())
        {
            loader = loaderIt->second;
        }
    }
    if (!loader || !loader->CanLoad(resolution.resolvedPath))
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderUnavailable,
                               "No compatible loader is available for the registered resource."));
        return nullptr;
    }

    ResourceLoadPreparationStateRef loaderState;
    uint64 canonicalImportOptionsHash = 0;
    ResourceLoadError captureError;
    if (!loader->CapturePreparationState(0,
                                         loaderState,
                                         canonicalImportOptionsHash,
                                         captureError))
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::AssetIdentityMismatch,
                               "The registered resource import profile cannot be reconstructed: " +
                                   captureError.message));
        return nullptr;
    }

    const AssetKey currentAssetKey = MakeAssetKey(resolution.resolvedPath,
                                                   metadata->type,
                                                   canonicalImportOptionsHash,
                                                   0,
                                                   1);
    if (GetResourceIdForAssetKey(currentAssetKey) != id)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::AssetIdentityMismatch,
                               "The registered ResourceId belongs to an import/profile variant "
                               "that is not reproducible by the active loader configuration."));
        RVX_RESOURCE_ERROR("Refusing to reload resource {} from '{}' with a different AssetKey profile",
                           id,
                           metadata->path);
        return nullptr;
    }

    if (loader->SupportsPreparedLoading())
    {
        return LoadPreparedSynchronously(resolution,
                                         metadata->path,
                                         currentAssetKey,
                                         std::move(loader),
                                         std::move(loaderState));
    }

    return LoadInternal(metadata->path,
                        resolution,
                        metadata->type,
                        id,
                        std::move(loader));
}

IResource* ResourceManager::LoadPreparedSynchronously(
    const ResourcePathResolution& resolution,
    const std::string& requestedPath,
    const AssetKey& assetKey,
    std::shared_ptr<IResourceLoader> loader,
    ResourceLoadPreparationStateRef loaderState)
{
    if (!IsOwnerThread())
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderFailed,
                               "Prepared synchronous publication must run on the ResourceManager owner thread."));
        RVX_RESOURCE_ERROR("Prepared synchronous load for '{}' was requested off the owner thread",
                           requestedPath);
        return nullptr;
    }

    ResourceLoadPreparationContext context;
    context.assetKey = assetKey;
    context.resourceIdentityPath = BuildAssetResourceIdentity(assetKey);
    context.requestedPath = requestedPath;
    context.resolvedPath = resolution.resolvedPath;
    context.rootResourceId = GetResourceIdForAssetKey(assetKey);
    context.traceContext = m_config.startupTraceContext;
    context.loaderState = std::move(loaderState);

    PreparedResourceBundle bundle;
    ResourceLoadError error;
    if (!loader || !loader->Prepare(context, bundle, error))
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderFailed,
                               error.message.empty()
                                   ? "The loader failed to prepare the resource."
                                   : error.message));
        return nullptr;
    }

    const ResourceHandle<IResource> root = bundle.GetRoot();
    if (!root || !PublishPreparedBundle(resolution,
                                        requestedPath,
                                        assetKey,
                                        bundle,
                                        error))
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderFailed,
                               error.message.empty()
                                   ? "The prepared resource could not be published."
                                   : error.message));
        return nullptr;
    }

    return root.Get();
}

IResource* ResourceManager::LoadInternal(const std::string& path,
                                         const ResourcePathResolution& resolution,
                                         ResourceType type,
                                         ResourceId managerId,
                                         std::shared_ptr<IResourceLoader> loader)
{
    Diagnostics::TraceSpan loadSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "ResourceLoad",
        {{"requestedPath", path},
         {"resolvedPath", resolution.resolvedPath},
         {"resourceType", GetResourceTypeName(type)}});
    const std::string& resolvedPath = resolution.resolvedPath;

    // Snapshot the loader under the manager lock, then perform IO/parse/decode
    // after releasing it. The shared ownership keeps the loader alive through
    // synchronous work even if shutdown or replacement begins elsewhere.
    if (!loader)
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        const auto loaderIt = m_loaders.find(type);
        if (loaderIt != m_loaders.end())
        {
            loader = loaderIt->second;
        }
    }
    if (!loader)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderUnavailable,
                               "No loader registered for resource type."));
        RVX_RESOURCE_ERROR("No loader registered for resource type: {}", GetResourceTypeName(type));
        loadSpan.SetAttribute("result", "loader-unavailable");
        return nullptr;
    }

    // Load the resource
    IResource* resource = loader->Load(resolvedPath);
    if (!resource)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderFailed,
                               "Loader failed to create the resource."));
        RVX_RESOURCE_ERROR("Failed to load resource: {}", path);
        loadSpan.SetAttribute("result", "loader-failed");
        return nullptr;
    }

    Diagnostics::TraceSpan publishSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "CPUPublish",
        {{"requestedPath", path},
         {"resourceType", GetResourceTypeName(type)}});

    // Some loaders cache specialized variants internally while loading. The
    // ResourceManager path API owns the generic path identity, so remove any
    // loader-assigned cache entry before rewriting the ID to avoid one object
    // being stored under two keys.
    const ResourceId loaderAssignedId = resource->GetId();
    bool releaseLoaderCacheRetainAfterStore = false;
    if (loaderAssignedId != InvalidResourceId && loaderAssignedId != managerId &&
        m_cache->Contains(loaderAssignedId))
    {
        resource->AddRef();
        m_cache->Remove(loaderAssignedId);
        releaseLoaderCacheRetainAfterStore = true;
    }

    resource->SetId(managerId);
    resource->SetPath(path);
    resource->SetName(std::filesystem::path(path).stem().string());

    // Load dependencies
    LoadDependencies(resource);

    // Register in registry
    ResourceMetadata metadata;
    metadata.id = resource->GetId();
    metadata.path = path;
    metadata.name = resource->GetName();
    metadata.type = type;
    metadata.dependencies = resource->GetAllDependencies();
    m_registry->Register(metadata);

    // Add to dependency graph
    m_dependencyGraph->AddResource(resource->GetId(), metadata.dependencies);

    // Store in cache
    m_cache->Store(resource);
    if (releaseLoaderCacheRetainAfterStore)
    {
        resource->Release();
    }

    // Mark as loaded
    resource->NotifyLoaded();
    QueueLifecycleEvent(ResourceLifecycleEventType::Ready, resource);
    publishSpan.SetAttribute("resourceId", resource->GetId());
    publishSpan.SetAttribute("result", "published");

    SetLastLoadDiagnostic(
        MakeLoadDiagnostic(resolution,
                           true,
                           ResourceLoadFailureCode::None,
                           "Resource loaded successfully."));
    RegisterHotReloadResource(resource, resolution);

    loadSpan.SetAttribute("result", "loaded");
    Diagnostics::RecordTraceInstant(
        m_config.startupTraceContext,
        "CPUReady",
        {{"requestedPath", path}, {"resourceId", resource->GetId()}});
    RVX_RESOURCE_DEBUG("Loaded resource: {} (type: {})", path, GetResourceTypeName(type));
    return resource;
}

ResourceLoadDiagnostic ResourceManager::GetLastLoadDiagnostic() const
{
    std::lock_guard<std::mutex> lock(m_diagnosticMutex);
    return m_lastLoadDiagnostic;
}

std::string ResourceManager::ExportLastLoadDiagnosticJson() const
{
    return ExportResourceLoadDiagnosticJson(GetLastLoadDiagnostic());
}

bool ResourceManager::SaveLastLoadDiagnosticJson(const char* filename) const
{
    return SaveResourceLoadDiagnosticJson(filename, GetLastLoadDiagnostic());
}

void ResourceManager::SetLastLoadDiagnostic(const ResourceLoadDiagnostic& diagnostic)
{
    std::lock_guard<std::mutex> lock(m_diagnosticMutex);
    m_lastLoadDiagnostic = diagnostic;
}

bool ResourceManager::IsHotReloadSupportedByPolicy() const
{
    return m_config.runtimePolicy.allowSourceAssetReads &&
           !m_config.runtimePolicy.requireCookedArtifacts &&
           !m_config.runtimePolicy.requireRuntimePackage;
}

void ResourceManager::SetHotReloadDiagnostic(ResourceHotReloadStatus status,
                                             bool requested,
                                             bool enabled,
                                             const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_hotReloadMutex);

    m_hotReloadDiagnostic.requested = requested;
    m_hotReloadDiagnostic.enabled = enabled;
    m_hotReloadDiagnostic.sourceAssetAccessRequired = true;
    m_hotReloadDiagnostic.status = status;
    m_hotReloadDiagnostic.message = message;
    m_hotReloadDiagnostic.watcherInitialized = HotReloadManager::Get().IsInitialized();

    if (HotReloadManager::Get().IsInitialized())
    {
        const HotReloadManager::ReloadStats stats = HotReloadManager::Get().GetStats();
        m_hotReloadDiagnostic.watchedFileCount = stats.watchedFiles;
        m_hotReloadDiagnostic.registeredResourceCount = stats.registeredResources;
        m_hotReloadDiagnostic.pendingReloadCount = HotReloadManager::Get().GetPendingReloadCount();
        m_hotReloadDiagnostic.totalReloadCount = stats.totalReloads;
        m_hotReloadDiagnostic.successfulReloadCount = stats.successfulReloads;
        m_hotReloadDiagnostic.failedReloadCount = stats.failedReloads;
    }
    else
    {
        m_hotReloadDiagnostic.watchedFileCount = 0;
        m_hotReloadDiagnostic.registeredResourceCount = 0;
        m_hotReloadDiagnostic.pendingReloadCount = 0;
        m_hotReloadDiagnostic.totalReloadCount = 0;
        m_hotReloadDiagnostic.successfulReloadCount = 0;
        m_hotReloadDiagnostic.failedReloadCount = 0;
    }
}

void ResourceManager::ConfigureHotReload(bool enable)
{
    const bool supported = IsHotReloadSupportedByPolicy();
    m_config.enableHotReload = enable && supported;

    if (!enable)
    {
        if (HotReloadManager::Get().IsInitialized())
        {
            HotReloadManager::Get().SetEnabled(false);
        }
        SetHotReloadDiagnostic(ResourceHotReloadStatus::Disabled,
                               false,
                               false,
                               "Hot reload is disabled by configuration.");
        return;
    }

    if (!supported)
    {
        if (HotReloadManager::Get().IsInitialized())
        {
            HotReloadManager::Get().SetEnabled(false);
        }
        SetHotReloadDiagnostic(ResourceHotReloadStatus::UnsupportedRuntimePolicy,
                               true,
                               false,
                               "Hot reload requires source asset reads and is disabled for cooked/package runtime policies.");
        RVX_RESOURCE_WARN("ResourceManager: hot reload requested but rejected by runtime resource policy");
        return;
    }

    HotReloadConfig hotReloadConfig;
    hotReloadConfig.enabled = true;
    if (!HotReloadManager::Get().IsInitialized())
    {
        HotReloadManager::Get().Initialize(this, hotReloadConfig);
        m_hotReloadInitializedByManager = true;
    }
    else
    {
        HotReloadManager::Get().SetEnabled(true);
    }

    if (m_hotReloadCallbackId == 0)
    {
        m_hotReloadCallbackId = HotReloadManager::Get().OnReload(
            [this](const ReloadEvent& event)
            {
                if (event.success && event.newResource != nullptr)
                {
                    QueueLifecycleEvent(ResourceLifecycleEventType::Reloaded,
                                        event.newResource);
                }
            });
    }

    SetHotReloadDiagnostic(ResourceHotReloadStatus::Active,
                           true,
                           true,
                           "Hot reload is active for source asset resources.");
}

void ResourceManager::RegisterHotReloadResource(IResource* resource, const ResourcePathResolution& resolution)
{
    if (!resource || !m_config.enableHotReload || resolution.domain != ResourceLoadDomain::SourceAsset ||
        !HotReloadManager::Get().IsInitialized())
    {
        return;
    }

    HotReloadManager& hotReload = HotReloadManager::Get();
    hotReload.RegisterResource(resource, resolution.resolvedPath);

    const ResourceId id = resource->GetId();
    {
        std::lock_guard<std::mutex> lock(m_hotReloadMutex);
        if (m_hotReloadWatchIds.find(id) != m_hotReloadWatchIds.end())
        {
            return;
        }
    }

    const uint32_t watchId = hotReload.WatchFile(resolution.resolvedPath);
    {
        std::lock_guard<std::mutex> lock(m_hotReloadMutex);
        if (watchId != 0)
        {
            m_hotReloadWatchIds[id] = watchId;
        }
    }

    SetHotReloadDiagnostic(ResourceHotReloadStatus::Active,
                           true,
                           true,
                           watchId == 0
                               ? "Hot reload is active, but watcher registration failed for the resource path."
                               : "Hot reload is active and tracking loaded source assets.");
}

void ResourceManager::LoadDependencies(IResource* resource)
{
    auto dependencies = resource->GetRequiredDependencies();
    for (ResourceId depId : dependencies)
    {
        // Load each dependency (will be cached)
        LoadResource(depId);
    }
}

void ResourceManager::LoadBatch(const std::vector<std::string>& paths,
                                 std::function<void(float)> onProgress,
                                 std::function<void()> onComplete)
{
    size_t total = paths.size();
    size_t loaded = 0;

    for (const auto& path : paths)
    {
        LoadResource(path);
        loaded++;

        if (onProgress)
        {
            onProgress(static_cast<float>(loaded) / static_cast<float>(total));
        }
    }

    if (onComplete)
    {
        onComplete();
    }
}

bool ResourceManager::IsLoaded(const std::string& path) const
{
    if (!m_initialized || !m_cache)
    {
        return false;
    }

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, path);
    return resolution.allowed &&
           m_cache->ContainsLoaded(GetCanonicalResolvedResourceId(resolution));
}

bool ResourceManager::IsLoaded(const AssetKey& assetKey) const
{
    return m_initialized && m_cache && assetKey.IsValid() &&
           m_cache->ContainsLoaded(GetResourceIdForAssetKey(assetKey));
}

bool ResourceManager::IsLoaded(ResourceId id) const
{
    return m_cache && m_cache->ContainsLoaded(id);
}

void ResourceManager::Unload(const std::string& path)
{
    if (!m_initialized)
    {
        return;
    }

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, path);
    if (resolution.allowed)
    {
        Unload(GetCanonicalResolvedResourceId(resolution));
    }
}

void ResourceManager::Unload(ResourceId id)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);

    if (m_cache) m_cache->Remove(id);
    if (m_registry) m_registry->Unregister(id);
    if (m_dependencyGraph) m_dependencyGraph->RemoveResource(id);
}

void ResourceManager::UnloadUnused()
{
    if (m_cache)
    {
        m_cache->EvictUnused();
    }
}

void ResourceManager::Clear()
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);

    if (m_cache) m_cache->Clear();
    if (m_registry) m_registry->Clear();
    if (m_dependencyGraph) m_dependencyGraph->Clear();
}

void ResourceManager::EnableHotReload(bool enable)
{
    ConfigureHotReload(enable);
}

void ResourceManager::Unload(const AssetKey& assetKey)
{
    if (assetKey.IsValid())
    {
        Unload(GetResourceIdForAssetKey(assetKey));
    }
}

bool ResourceManager::IsHotReloadEnabled() const
{
    return m_config.enableHotReload &&
           HotReloadManager::Get().IsInitialized() &&
           HotReloadManager::Get().IsEnabled();
}

ResourceHotReloadDiagnostic ResourceManager::GetHotReloadDiagnostic() const
{
    ResourceHotReloadDiagnostic diagnostic;
    {
        std::lock_guard<std::mutex> lock(m_hotReloadMutex);
        diagnostic = m_hotReloadDiagnostic;
    }

    diagnostic.watcherInitialized = HotReloadManager::Get().IsInitialized();
    if (HotReloadManager::Get().IsInitialized())
    {
        const HotReloadManager::ReloadStats stats = HotReloadManager::Get().GetStats();
        diagnostic.watchedFileCount = stats.watchedFiles;
        diagnostic.registeredResourceCount = stats.registeredResources;
        diagnostic.pendingReloadCount = HotReloadManager::Get().GetPendingReloadCount();
        diagnostic.totalReloadCount = stats.totalReloads;
        diagnostic.successfulReloadCount = stats.successfulReloads;
        diagnostic.failedReloadCount = stats.failedReloads;
    }

    return diagnostic;
}

std::string ResourceManager::ExportHotReloadDiagnosticJson() const
{
    return ExportResourceHotReloadDiagnosticJson(GetHotReloadDiagnostic());
}

bool ResourceManager::SaveHotReloadDiagnosticJson(const char* filename) const
{
    return SaveResourceHotReloadDiagnosticJson(filename, GetHotReloadDiagnostic());
}

void ResourceManager::CheckForChanges()
{
    if (!IsHotReloadEnabled())
    {
        return;
    }

    HotReloadManager::Get().Update();
    SetHotReloadDiagnostic(ResourceHotReloadStatus::Active,
                           true,
                           true,
                           "Hot reload update processed.");
}

void ResourceManager::OnResourceReloaded(std::function<void(ResourceId, IResource*)> callback)
{
    m_reloadCallback = std::move(callback);
}

void ResourceManager::SetCacheLimit(size_t bytes)
{
    if (m_cache)
    {
        m_cache->SetMemoryLimit(bytes);
    }
}

void ResourceManager::ClearCache()
{
    if (m_cache)
    {
        m_cache->Clear();
    }
}

void ResourceManager::RegisterLoader(ResourceType type, std::unique_ptr<IResourceLoader> loader)
{
    if (!loader)
    {
        RVX_RESOURCE_WARN("ResourceManager rejected null loader registration for {}",
                          GetResourceTypeName(type));
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    m_loaders[type] = std::shared_ptr<IResourceLoader>(std::move(loader));
}

IResourceLoader* ResourceManager::GetLoader(ResourceType type)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    auto it = m_loaders.find(type);
    return it != m_loaders.end() ? it->second.get() : nullptr;
}

void ResourceManager::ProcessCompletedLoads()
{
    if (m_initialized && !IsOwnerThread())
    {
        RVX_RESOURCE_ERROR("ResourceManager::ProcessCompletedLoads must run on its owner/update thread");
        return;
    }
    JobSystem::Get().ProcessMainThreadCompletions();
    DrainPreparedLoadCompletions();
    DrainLegacyAsyncWaiters();
    DrainLifecycleEvents();
}

ResourceLoadOperationOwner ResourceManager::RequestPreparedLoad(
    const std::string& path,
    ResourceLoadOptions options,
    ResourceType requestedType,
    ResourceLoadPreparationStateRef preparationState)
{
    if (!m_acceptAsyncRequests.load(std::memory_order_acquire))
    {
        return {};
    }

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, path);
    if (!resolution.allowed)
    {
        RVX_RESOURCE_WARN("ResourceManager rejected async request '{}': {}",
                          path,
                          resolution.diagnosticMessage);
        return {};
    }

    const ResourceType detectedType = DetectResourceType(resolution.resolvedPath);
    const ResourceType resourceType = requestedType != ResourceType::Unknown
                                          ? requestedType
                                          : detectedType;
    if (!options.traceContext.IsEnabled())
    {
        options.traceContext = m_config.startupTraceContext;
    }

    AssetKey assetKey;
    std::string resourceIdentityPath;
    ResourceId rootResourceId = InvalidResourceId;
    std::shared_ptr<IResourceLoader> loader;
    ResourceLoadPreparationStateRef loaderState;
    ResourceLoadOperationOwner operation;
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        if (!m_initialized || !m_acceptAsyncRequests.load(std::memory_order_acquire) ||
            !m_cache)
        {
            return {};
        }

        const auto loaderIt = m_loaders.find(resourceType);
        if (loaderIt == m_loaders.end() || !loaderIt->second)
        {
            return {};
        }
        loader = loaderIt->second;
        if (!loader->CanLoad(resolution.resolvedPath))
        {
            RVX_RESOURCE_WARN("Resource loader for {} rejected path '{}'",
                              GetResourceTypeName(resourceType),
                              resolution.resolvedPath);
            return {};
        }

        uint64 canonicalImportOptionsHash = options.importOptionsHash;
        ResourceLoadError captureError;
        try
        {
            const bool captured = preparationState
                                      ? loader->ValidatePreparationState(
                                            options.importOptionsHash,
                                            std::move(preparationState),
                                            loaderState,
                                            canonicalImportOptionsHash,
                                            captureError)
                                      : loader->CapturePreparationState(
                                            options.importOptionsHash,
                                            loaderState,
                                            canonicalImportOptionsHash,
                                            captureError);
            if (!captured)
            {
                RVX_RESOURCE_WARN("ResourceManager rejected async import profile for '{}': {}",
                                  path,
                                  captureError.message);
                return {};
            }
        }
        catch (const std::exception& error)
        {
            RVX_RESOURCE_ERROR("Resource loader failed to capture import state for '{}': {}",
                               path,
                               error.what());
            return {};
        }
        catch (...)
        {
            RVX_RESOURCE_ERROR("Resource loader failed to capture import state for '{}'",
                               path);
            return {};
        }
        options.importOptionsHash = canonicalImportOptionsHash;
        assetKey = MakeAssetKey(resolution.resolvedPath,
                                resourceType,
                                options.importOptionsHash,
                                options.platformProfileHash,
                                options.loaderSchemaVersion);
        if (!assetKey.IsValid())
        {
            RVX_RESOURCE_WARN("ResourceManager cannot create an async asset key for '{}'", path);
            return {};
        }
        resourceIdentityPath = BuildAssetResourceIdentity(assetKey);
        rootResourceId = GetResourceIdForAssetKey(assetKey);

        // Prefer the operation until its owner has completed publication and
        // removed it. This keeps every concurrent subscriber on one request id
        // even during the short cache-visible/AwaitingPublish interval.
        const auto inFlight = m_inFlightLoads.find(assetKey);
        if (inFlight != m_inFlightLoads.end())
        {
            return inFlight->second;
        }

        // Cache and in-flight lookup share the same admission critical section
        // as owner publication, so a request cannot miss both meanings.
        if (options.allowCacheLookup)
        {
            if (IResource* cached = m_cache->Get(rootResourceId))
            {
                ResourceLoadOperationOwner cachedOperation =
                    ResourceLoadOperationOwner::Create(assetKey, std::move(options));
                if (cachedOperation && cachedOperation.BeginLoading() &&
                    cachedOperation.BeginAwaitingPublish())
                {
                    cachedOperation.CompleteReady(ResourceHandle<IResource>(cached));
                }
                return cachedOperation;
            }
        }

        operation = ResourceLoadOperationOwner::Create(assetKey, std::move(options));
        if (!operation || !operation.BeginLoading())
        {
            return {};
        }
        m_inFlightLoads.emplace(assetKey, operation);
    }

    ResourceLoadPreparationContext context;
    context.assetKey = assetKey;
    context.resourceIdentityPath = resourceIdentityPath;
    context.requestedPath = path;
    context.resolvedPath = resolution.resolvedPath;
    context.rootResourceId = rootResourceId;
    context.traceContext = operation.GetSnapshot().options.traceContext;
    context.loaderState = std::move(loaderState);
    context.isCancellationRequested = [operation]()
    {
        return operation.IsCancellationRequested();
    };

    m_pendingAsyncJobCount.fetch_add(1, std::memory_order_relaxed);
    JobSubmissionDesc desc;
    desc.category = "Resource.LoadAsync";
    desc.priority = operation.GetSnapshot().options.priority == ResourceLoadPriority::High
                        ? JobPriority::High
                        : JobPriority::Normal;

    // JobSystem intentionally executes inline when no worker system exists.
    // Even then ExecutePreparedLoad only stages a completion; publication
    // remains owned by ProcessCompletedLoads().
    {
        std::lock_guard<std::mutex> lock(m_preparedJobMutex);
        if (!m_acceptAsyncRequests.load(std::memory_order_acquire))
        {
            m_pendingAsyncJobCount.fetch_sub(1, std::memory_order_relaxed);
            operation.CompleteCancelled("ResourceManager stopped accepting requests.");
            RemoveInFlightLoad(assetKey, operation.GetRequestId());
            return operation;
        }
        JobHandle job = JobSystem::Get().Submit(
            [this,
             operation,
             loader = std::move(loader),
             context = std::move(context),
             resolution]() mutable
            {
                ExecutePreparedLoad(std::move(operation),
                                    std::move(loader),
                                    std::move(context),
                                    resolution);
            },
            desc);
        m_preparedJobs.push_back(std::move(job));
    }
    return operation;
}

bool IResourceLoader::ValidatePreparationState(
    uint64 requestedImportOptionsHash,
    ResourceLoadPreparationStateRef suppliedState,
    ResourceLoadPreparationStateRef& outState,
    uint64& outCanonicalImportOptionsHash,
    ResourceLoadError& outError) const
{
    outState.reset();
    outCanonicalImportOptionsHash = requestedImportOptionsHash;
    if (suppliedState)
    {
        outError = {
            ResourceLoadErrorCode::InvalidRequest,
            "This resource loader does not accept caller-supplied preparation state."};
        return false;
    }
    return CapturePreparationState(requestedImportOptionsHash,
                                   outState,
                                   outCanonicalImportOptionsHash,
                                   outError);
}

void ResourceManager::ExecutePreparedLoad(ResourceLoadOperationOwner operation,
                                          std::shared_ptr<IResourceLoader> loader,
                                          ResourceLoadPreparationContext context,
                                          ResourcePathResolution resolution)
{
    struct PendingJobGuard
    {
        std::atomic<size_t>& counter;
        ~PendingJobGuard()
        {
            counter.fetch_sub(1, std::memory_order_relaxed);
        }
    } guard{m_pendingAsyncJobCount};

    PreparedLoadCompletion completion;
    completion.assetKey = context.assetKey;
    completion.operation = operation;
    completion.resolution = std::move(resolution);
    completion.requestedPath = context.requestedPath;

    if (!operation || operation.IsCancellationRequested())
    {
        completion.cancelled = true;
    }
    else if (!loader)
    {
        completion.error = {ResourceLoadErrorCode::LoaderUnavailable,
                            "The resource loader was unavailable before preparation began."};
    }
    else
    {
        Diagnostics::TraceSpan prepareSpan = Diagnostics::BeginTraceSpan(
            context.traceContext,
            "ResourcePrepare",
            {{"requestedPath", context.requestedPath},
             {"resolvedPath", context.resolvedPath},
             {"resourceType", GetResourceTypeName(context.assetKey.resourceType)}});

        try
        {
            operation.UpdateProgress(ResourceLoadStage::Resolve, 1.0f);
            const bool prepared = loader->Prepare(context,
                                                   completion.bundle,
                                                   completion.error);
            if (!prepared && !completion.error.HasError())
            {
                completion.error = {ResourceLoadErrorCode::LoaderFailure,
                                    "The loader rejected the resource preparation request."};
            }
            if (operation.IsCancellationRequested() || context.IsCancellationRequested())
            {
                completion.bundle = {};
                completion.cancelled = true;
            }
            else if (prepared)
            {
                operation.UpdateProgress(ResourceLoadStage::Finalize, 1.0f);
                operation.BeginAwaitingPublish();
                prepareSpan.SetAttribute("result", "prepared");
            }
            else
            {
                prepareSpan.SetAttribute("result", "failed");
            }
        }
        catch (const std::exception& error)
        {
            completion.bundle = {};
            completion.error = {ResourceLoadErrorCode::LoaderFailure, error.what()};
            prepareSpan.SetAttribute("result", "exception");
        }
        catch (...)
        {
            completion.bundle = {};
            completion.error = {ResourceLoadErrorCode::LoaderFailure,
                                "The loader raised an unknown exception while preparing the resource."};
            prepareSpan.SetAttribute("result", "unknown-exception");
        }
    }

    m_pendingAsyncCompletionCount.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_preparedCompletionMutex);
        m_preparedCompletions.push_back(std::move(completion));
    }
}

void ResourceManager::DrainPreparedLoadCompletions(bool shutdownCancellation)
{
    std::vector<PreparedLoadCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(m_preparedCompletionMutex);
        completions.swap(m_preparedCompletions);
    }

    for (PreparedLoadCompletion& completion : completions)
    {
        m_pendingAsyncCompletionCount.fetch_sub(1, std::memory_order_relaxed);
        if (shutdownCancellation || completion.cancelled ||
            completion.error.code == ResourceLoadErrorCode::Cancelled ||
            completion.operation.IsCancellationRequested())
        {
            completion.operation.CompleteCancelled(
                shutdownCancellation ? "ResourceManager is shutting down."
                                      : "All resource-load subscribers cancelled their request.");
            RemoveInFlightLoad(completion.assetKey, completion.operation.GetRequestId());
            continue;
        }

        if (completion.error.HasError())
        {
            completion.operation.CompleteFailed(std::move(completion.error));
            RemoveInFlightLoad(completion.assetKey, completion.operation.GetRequestId());
            continue;
        }

        if (!completion.operation.BeginOwnerPublication())
        {
            completion.operation.CompleteCancelled(
                "Resource publication was cancelled before owner commit.");
            RemoveInFlightLoad(completion.assetKey, completion.operation.GetRequestId());
            continue;
        }

        ResourceLoadError publishError;
        if (PublishPreparedBundle(completion.resolution,
                                  completion.requestedPath,
                                  completion.assetKey,
                                  completion.bundle,
                                  publishError))
        {
            completion.operation.CompleteReady(completion.bundle.GetRoot());
        }
        else
        {
            if (!publishError.HasError())
            {
                publishError = {ResourceLoadErrorCode::PublishFailure,
                                "Prepared resource publication failed without a diagnostic."};
            }
            completion.operation.CompleteFailed(std::move(publishError));
        }
        RemoveInFlightLoad(completion.assetKey, completion.operation.GetRequestId());
    }

    {
        std::lock_guard<std::mutex> lock(m_preparedJobMutex);
        std::erase_if(m_preparedJobs,
                      [](const JobHandle& job)
                      {
                          return job.IsComplete();
                      });
    }
}

void ResourceManager::DrainLegacyAsyncWaiters()
{
    std::vector<LegacyAsyncWaiter> waiters;
    {
        std::lock_guard<std::mutex> lock(m_legacyAsyncWaiterMutex);
        waiters.swap(m_legacyAsyncWaiters);
    }

    std::vector<LegacyAsyncWaiter> pending;
    pending.reserve(waiters.size());
    for (LegacyAsyncWaiter& waiter : waiters)
    {
        const ResourceLoadSnapshot snapshot = waiter.subscription.GetSnapshot();
        if (!waiter.readyWithoutSubscription && !snapshot.IsTerminal())
        {
            pending.push_back(std::move(waiter));
            continue;
        }

        ResourceHandle<IResource> resource = waiter.subscription.TryGet();
        if (waiter.completion)
        {
            waiter.completion(std::move(resource));
        }
        waiter.subscription.Cancel();
    }

    if (!pending.empty())
    {
        std::lock_guard<std::mutex> lock(m_legacyAsyncWaiterMutex);
        m_legacyAsyncWaiters.insert(m_legacyAsyncWaiters.end(),
                                    std::make_move_iterator(pending.begin()),
                                    std::make_move_iterator(pending.end()));
    }
}

bool ResourceManager::ValidatePreparedBundle(const PreparedResourceBundle& bundle,
                                             ResourceLoadError& outError) const
{
    if (!bundle.IsValid())
    {
        outError = {ResourceLoadErrorCode::PublishFailure, bundle.GetValidationError()};
        return false;
    }

    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        if (!entry.resource || entry.resource->IsLoaded())
        {
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "A prepared bundle contains an already-published or null resource."};
            return false;
        }
    }
    return true;
}

bool ResourceManager::PublishPreparedBundle(const ResourcePathResolution& resolution,
                                            const std::string& requestedPath,
                                            const AssetKey& assetKey,
                                            PreparedResourceBundle& bundle,
                                            ResourceLoadError& outError)
{
    Diagnostics::TraceSpan publishSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "CPUPublish",
        {{"requestedPath", requestedPath},
         {"resourceType", GetResourceTypeName(assetKey.resourceType)}});
    // Publication is short, owner-thread work, but it must be serialized with
    // cache/in-flight admission so concurrent RequestAsync callers cannot miss
    // both the operation and its newly published cache object.
    std::unique_lock<std::recursive_mutex> publicationLock(m_loadMutex);
    if (!m_initialized || !ValidatePreparedBundle(bundle, outError))
    {
        return false;
    }

    ResourceHandle<IResource> root = bundle.GetRoot();
    if (!root || root->GetType() != assetKey.resourceType)
    {
        outError = {ResourceLoadErrorCode::PublishFailure,
                    "Prepared bundle root does not match the requested resource type."};
        return false;
    }

    // The AssetKey is the authority for a coalesced root identity.  A loader
    // may use an absolute source path internally, but aliases must resolve to
    // the same cache record before owner-thread publication.
    root->SetId(GetResourceIdForAssetKey(assetKey));
    root->SetPath(requestedPath);
    root->SetName(std::filesystem::path(requestedPath).stem().string());

    // Loader bundle bookkeeping was created before the manager asserted the
    // canonical root identity. Revalidate effective ids after that rebind so a
    // malicious or stale dependency cannot alias the root at publication.
    std::unordered_set<ResourceId> effectiveResourceIds;
    effectiveResourceIds.reserve(bundle.GetEntries().size());
    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        IResource* effective = entry.isRoot ? root.Get() : entry.resource.Get();
        if (effective == nullptr || effective->GetId() == InvalidResourceId ||
            !effectiveResourceIds.insert(effective->GetId()).second)
        {
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "Prepared bundle contains duplicate or invalid effective resource ids."};
            return false;
        }
    }

    // Resolve any dependency that was already published to its canonical
    // cache object before registering this transaction. A prepared worker owns
    // duplicate temporary objects by design; allowing Model/Material to retain
    // them would create two live meanings for one ResourceId.
    std::unordered_map<ResourceId, ResourceHandle<IResource>> cachedResources;
    cachedResources.reserve(bundle.GetEntries().size());
    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        IResource* prepared = entry.isRoot ? root.Get() : entry.resource.Get();
        if (IResource* cached = m_cache->Get(prepared->GetId()))
        {
            if (cached->GetType() != prepared->GetType())
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared resource id collides with a cached resource of another type."};
                return false;
            }
            if (entry.isRoot)
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared root collides with an existing cache entry; its AssetKey identity is stale."};
                return false;
            }
            cachedResources.emplace(prepared->GetId(), ResourceHandle<IResource>(cached));
        }
    }

    auto lookupCached = [&cachedResources](ResourceId resourceId,
                                            ResourceType expectedType,
                                            ResourceHandle<IResource>& outResource) -> bool
    {
        const auto cached = cachedResources.find(resourceId);
        if (cached == cachedResources.end())
        {
            return true;
        }
        if (!cached->second || cached->second->GetType() != expectedType)
        {
            return false;
        }
        outResource = cached->second;
        return true;
    };

    // Materials first: Model resources may refer to a Material which itself
    // refers to a cached Texture. Copy slots before SetTexture because the
    // setter can update the backing unordered map.
    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        auto* material = dynamic_cast<MaterialResource*>(entry.resource.Get());
        if (!material || cachedResources.contains(material->GetId()))
        {
            continue;
        }

        std::vector<std::pair<std::string, ResourceHandle<TextureResource>>> textures;
        textures.reserve(material->GetTextures().size());
        for (const auto& [slot, texture] : material->GetTextures())
        {
            textures.emplace_back(slot, texture);
        }
        for (const auto& [slot, texture] : textures)
        {
            if (!texture)
            {
                continue;
            }
            ResourceHandle<IResource> canonical;
            if (!lookupCached(texture.GetId(), ResourceType::Texture, canonical))
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared material texture does not match its cached ResourceId type."};
                return false;
            }
            if (canonical)
            {
                material->SetTexture(slot,
                                     ResourceHandle<TextureResource>(
                                         static_cast<TextureResource*>(canonical.Get())));
            }
        }
    }

    // Environment roots own strong handles to their four IBL textures just as
    // materials own texture handles.  If those dependencies survived an
    // earlier root eviction, rebind the newly prepared root to the canonical
    // cache objects before publication.  Otherwise the root would retain
    // duplicate, never-published TextureResource instances with the same ids.
    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        auto* environment = dynamic_cast<EnvironmentResource*>(
            entry.isRoot ? root.Get() : entry.resource.Get());
        if (!environment || cachedResources.contains(environment->GetId()))
        {
            continue;
        }

        EnvironmentResourceData data = environment->GetData();
        auto rebindTexture = [&lookupCached](TextureHandle& texture) -> bool
        {
            if (!texture)
            {
                return false;
            }

            ResourceHandle<IResource> canonical;
            if (!lookupCached(texture.GetId(), ResourceType::Texture, canonical))
            {
                return false;
            }
            if (canonical)
            {
                texture = TextureHandle(static_cast<TextureResource*>(canonical.Get()));
            }
            return true;
        };

        if (!rebindTexture(data.environment) ||
            !rebindTexture(data.irradiance) ||
            !rebindTexture(data.prefiltered) ||
            !rebindTexture(data.brdfLUT) ||
            !environment->SetPreparedData(std::move(data)))
        {
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "A prepared environment could not bind its canonical texture dependencies."};
            return false;
        }
    }

    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        auto* model = dynamic_cast<ModelResource*>(entry.isRoot ? root.Get() : entry.resource.Get());
        if (!model || cachedResources.contains(model->GetId()))
        {
            continue;
        }

        std::vector<ResourceHandle<MeshResource>> meshes = model->GetMeshes();
        for (ResourceHandle<MeshResource>& mesh : meshes)
        {
            if (!mesh)
            {
                continue;
            }
            ResourceHandle<IResource> canonical;
            if (!lookupCached(mesh.GetId(), ResourceType::Mesh, canonical))
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared model mesh does not match its cached ResourceId type."};
                return false;
            }
            if (canonical)
            {
                mesh = ResourceHandle<MeshResource>(static_cast<MeshResource*>(canonical.Get()));
            }
        }
        model->SetMeshes(std::move(meshes));

        std::vector<ResourceHandle<MaterialResource>> materials = model->GetMaterials();
        for (ResourceHandle<MaterialResource>& material : materials)
        {
            if (!material)
            {
                continue;
            }
            ResourceHandle<IResource> canonical;
            if (!lookupCached(material.GetId(), ResourceType::Material, canonical))
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared model material does not match its cached ResourceId type."};
                return false;
            }
            if (canonical)
            {
                material = ResourceHandle<MaterialResource>(
                    static_cast<MaterialResource*>(canonical.Get()));
            }
        }
        model->SetMaterials(std::move(materials));
    }

    struct StagedPublication
    {
        ResourceHandle<IResource> resource;
        ResourceMetadata metadata;
        std::optional<ResourceMetadata> previousMetadata;
        std::vector<ResourceId> previousDependencies;
        bool graphExisted = false;
        bool registryChanged = false;
        bool graphChanged = false;
    };

    // Complete every fallible loader-defined metadata query before changing
    // owner state. In particular, GetAllDependencies() may be implemented by
    // third-party resources and is not allowed inside the commit boundary.
    std::vector<StagedPublication> staged;
    try
    {
        staged.reserve(bundle.GetEntries().size());
        for (const PreparedResourceEntry& entry : bundle.GetEntries())
        {
            IResource* resource = entry.resource.Get();
            if (entry.isRoot)
            {
                resource = root.Get();
            }
            if (cachedResources.contains(resource->GetId()))
            {
                continue;
            }

            StagedPublication publication;
            publication.resource = ResourceHandle<IResource>(resource);
            publication.metadata.id = resource->GetId();
            publication.metadata.path = resource->GetPath();
            publication.metadata.name = resource->GetName();
            publication.metadata.type = resource->GetType();
            publication.metadata.dependencies = resource->GetAllDependencies();
            for (ResourceId dependencyId : publication.metadata.dependencies)
            {
                if (dependencyId == InvalidResourceId ||
                    (!effectiveResourceIds.contains(dependencyId) &&
                     !m_cache->Contains(dependencyId)))
                {
                    outError = {ResourceLoadErrorCode::PublishFailure,
                                "Prepared resource dependency closure is incomplete."};
                    return false;
                }
            }
            publication.previousMetadata = m_registry->FindById(publication.metadata.id);
            publication.graphExisted = m_dependencyGraph->Contains(publication.metadata.id);
            if (publication.graphExisted)
            {
                publication.previousDependencies =
                    m_dependencyGraph->GetDependencies(publication.metadata.id);
            }
            staged.push_back(std::move(publication));
        }
    }
    catch (const std::exception& error)
    {
        outError = {ResourceLoadErrorCode::PublishFailure, error.what()};
        return false;
    }
    catch (...)
    {
        outError = {ResourceLoadErrorCode::PublishFailure,
                    "An unknown error interrupted prepared resource validation."};
        return false;
    }

    auto rollbackMetadata = [&]() noexcept
    {
        for (auto it = staged.rbegin(); it != staged.rend(); ++it)
        {
            try
            {
                if (it->graphChanged)
                {
                    if (it->graphExisted)
                    {
                        m_dependencyGraph->UpdateDependencies(it->metadata.id,
                                                              it->previousDependencies);
                    }
                    else
                    {
                        m_dependencyGraph->RemoveResource(it->metadata.id);
                    }
                }
                if (it->registryChanged)
                {
                    if (it->previousMetadata)
                    {
                        m_registry->Update(*it->previousMetadata);
                    }
                    else
                    {
                        m_registry->Unregister(it->metadata.id);
                    }
                }
            }
            catch (...)
            {
                RVX_RESOURCE_ERROR("Prepared resource metadata rollback failed for id {}",
                                   it->metadata.id);
            }
        }
    };

    try
    {
        std::vector<IResource*> cacheBatch;
        cacheBatch.reserve(staged.size());
        for (StagedPublication& publication : staged)
        {
            m_registry->Register(publication.metadata);
            publication.registryChanged = true;
            if (publication.graphExisted)
            {
                m_dependencyGraph->UpdateDependencies(publication.metadata.id,
                                                      publication.metadata.dependencies);
            }
            else
            {
                m_dependencyGraph->AddResource(publication.metadata.id,
                                               publication.metadata.dependencies);
            }
            publication.graphChanged = true;
            cacheBatch.push_back(publication.resource.Get());
        }
        if (!m_cache->StorePreparedBatch(cacheBatch))
        {
            rollbackMetadata();
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "Resource cache rejected the prepared publication batch."};
            return false;
        }
    }
    catch (const std::exception& error)
    {
        rollbackMetadata();
        outError = {ResourceLoadErrorCode::PublishFailure, error.what()};
        return false;
    }
    catch (...)
    {
        rollbackMetadata();
        outError = {ResourceLoadErrorCode::PublishFailure,
                    "An unknown error interrupted the prepared resource transaction."};
        return false;
    }

    // No rollback-capable work remains. Release admission serialization before
    // calling arbitrary observers so callbacks may safely enqueue another load
    // without deadlocking on m_loadMutex.
    publicationLock.unlock();

    // The owner databases and cache are now committed. Resource callbacks are
    // observers, not transaction participants: isolate exceptions so a bad
    // callback cannot roll back visible resources or leak contradictory Ready
    // events. Dependencies are still announced before the root.
    auto notifyCommitted = [this](const ResourceHandle<IResource>& resource)
    {
        if (!resource)
            return;
        try
        {
            resource->NotifyLoadedObserver();
        }
        catch (const std::exception& error)
        {
            RVX_RESOURCE_ERROR("Resource {} on-loaded observer failed: {}",
                               resource.GetId(),
                               error.what());
        }
        catch (...)
        {
            RVX_RESOURCE_ERROR("Resource {} on-loaded observer failed with an unknown exception",
                               resource.GetId());
        }

        try
        {
            QueueLifecycleEvent(ResourceLifecycleEventType::Ready, resource.Get());
        }
        catch (...)
        {
            RVX_RESOURCE_ERROR("Resource {} Ready lifecycle enqueue failed",
                               resource.GetId());
        }
    };
    for (const StagedPublication& publication : staged)
    {
        if (publication.resource.Get() != root.Get())
        {
            notifyCommitted(publication.resource);
        }
    }
    notifyCommitted(root);

    try
    {
        SetLastLoadDiagnostic(MakeLoadDiagnostic(
            resolution,
            true,
            ResourceLoadFailureCode::None,
            "Prepared resource bundle published on the owner thread."));
        RegisterHotReloadResource(root.Get(), resolution);
        Diagnostics::RecordTraceInstant(
            m_config.startupTraceContext,
            "CPUPublishCommitted",
            {{"requestedPath", requestedPath}, {"resourceId", root.GetId()}});
    }
    catch (const std::exception& error)
    {
        RVX_RESOURCE_ERROR("Committed resource {} post-publish observer failed: {}",
                           root.GetId(),
                           error.what());
    }
    catch (...)
    {
        RVX_RESOURCE_ERROR("Committed resource {} post-publish observer failed with an unknown exception",
                           root.GetId());
    }
    publishSpan.SetAttribute("resourceId", root.GetId());
    publishSpan.SetAttribute("result", "published");
    return true;
}

void ResourceManager::RemoveInFlightLoad(const AssetKey& assetKey,
                                         ResourceLoadRequestId requestId)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    const auto it = m_inFlightLoads.find(assetKey);
    if (it != m_inFlightLoads.end() && it->second.GetRequestId() == requestId)
    {
        m_inFlightLoads.erase(it);
    }
}

bool ResourceManager::IsOwnerThread() const
{
    return m_ownerThreadToken != 0 && m_ownerThreadToken == GetCurrentThreadToken();
}

void ResourceManager::SetLifecycleEventCallback(
    ResourceLifecycleEventCallback callback)
{
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    m_lifecycleEventCallback = std::move(callback);
}

void ResourceManager::QueueLifecycleEvent(ResourceLifecycleEventType type,
                                          IResource* resource)
{
    if (resource == nullptr || resource->GetId() == InvalidResourceId)
        return;

    ResourceLifecycleEvent event;
    event.type = type;
    event.resourceId = resource->GetId();
    event.resource = ResourceHandle<IResource>(resource);
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    m_lifecycleEvents.push_back(std::move(event));
}

void ResourceManager::DrainLifecycleEvents()
{
    std::vector<ResourceLifecycleEvent> events;
    ResourceLifecycleEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        events.swap(m_lifecycleEvents);
        callback = m_lifecycleEventCallback;
    }

    std::unordered_set<IResource*> reloadedResources;
    reloadedResources.reserve(events.size());
    for (const ResourceLifecycleEvent& event : events)
    {
        if (event.type == ResourceLifecycleEventType::Reloaded)
        {
            reloadedResources.insert(event.resource.Get());
        }
    }

    for (const ResourceLifecycleEvent& event : events)
    {
        if (event.type == ResourceLifecycleEventType::Ready &&
            reloadedResources.contains(event.resource.Get()))
        {
            continue;
        }
        if (callback)
        {
            callback(event);
        }
        if (event.type == ResourceLifecycleEventType::Reloaded &&
            m_reloadCallback)
        {
            m_reloadCallback(event.resourceId, event.resource.Get());
        }
    }
}

ResourceManager::Stats ResourceManager::GetStats() const
{
    std::lock_guard<std::recursive_mutex> loadLock(m_loadMutex);

    Stats stats;

    if (m_registry)
    {
        stats.totalResources = m_registry->GetCount();
    }

    if (m_cache)
    {
        auto cacheStats = m_cache->GetStats();
        stats.loadedCount = cacheStats.totalResources;
        stats.cpuMemory = cacheStats.memoryUsage;
        stats.gpuMemory = cacheStats.gpuMemoryUsage;
    }

    stats.pendingLoads += m_pendingAsyncJobCount.load(std::memory_order_relaxed);
    stats.pendingLoads += m_pendingAsyncCompletionCount.load(std::memory_order_relaxed);

    return stats;
}

ResourceType ResourceManager::GetTypeFromExtension(const std::string& extension)
{
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Model formats (3D files with scene hierarchy)
    if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".model")
        return ResourceType::Model;

    // Mesh formats (standalone mesh files)
    if (ext == ".obj")
        return ResourceType::Mesh;

    // Texture formats
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" || 
        ext == ".tga" || ext == ".bmp" || ext == ".hdr" || ext == ".gif")
        return ResourceType::Texture;

    // Material
    if (ext == ".mat" || ext == ".material")
        return ResourceType::Material;

    // Shader
    if (ext == ".hlsl" || ext == ".glsl" || ext == ".shader")
        return ResourceType::Shader;

    // Animation
    if (ext == ".anim" || ext == ".animation")
        return ResourceType::Animation;

    // Audio
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
        return ResourceType::Audio;

    // Scene
    if (ext == ".scene")
        return ResourceType::Scene;

    return ResourceType::Unknown;
}

std::string ResourceManager::ResolvePath(const std::string& path) const
{
    if (m_config.basePath.empty())
    {
        return path;
    }

    std::filesystem::path basePath(m_config.basePath);
    std::filesystem::path resourcePath(path);

    if (resourcePath.is_absolute())
    {
        return path;
    }

    return (basePath / resourcePath).string();
}

void ResourceManager::StartAsyncWorkers()
{
    const int requestedCount = m_config.asyncThreadCount;
    if (requestedCount <= 0)
    {
        RVX_RESOURCE_INFO("ResourceManager async loading uses inline fallback because asyncThreadCount is disabled");
        return;
    }

    JobSystem& jobSystem = JobSystem::Get();
    if (jobSystem.IsInitialized())
    {
        RVX_RESOURCE_INFO("ResourceManager async loading using existing Core JobSystem with {} worker(s)",
                          jobSystem.GetWorkerCount());
        return;
    }

    jobSystem.Initialize(static_cast<size_t>(requestedCount));
    m_jobSystemInitializedByManager = true;
    RVX_RESOURCE_INFO("ResourceManager initialized Core JobSystem with {} worker(s) for async loading",
                      jobSystem.GetWorkerCount());
}

void ResourceManager::StopAsyncWorkers()
{
    std::vector<JobHandle> preparedJobs;
    {
        std::lock_guard<std::mutex> lock(m_preparedJobMutex);
        preparedJobs.swap(m_preparedJobs);
    }
    for (const JobHandle& job : preparedJobs)
    {
        job.Wait();
    }

    JobSystem& jobSystem = JobSystem::Get();
    if (m_jobSystemInitializedByManager)
    {
        jobSystem.Shutdown();
        m_jobSystemInitializedByManager = false;
    }
}

// IResourceLoader implementation
bool IResourceLoader::Prepare(const ResourceLoadPreparationContext& context,
                              PreparedResourceBundle& outBundle,
                              ResourceLoadError& outError)
{
    (void)context;
    (void)outBundle;
    outError = {ResourceLoadErrorCode::LoaderFailure,
                "This resource loader does not implement allocation-free prepared loading."};
    return false;
}

bool IResourceLoader::CapturePreparationState(
    uint64 requestedImportOptionsHash,
    ResourceLoadPreparationStateRef& outState,
    uint64& outCanonicalImportOptionsHash,
    ResourceLoadError& outError) const
{
    outState.reset();
    outCanonicalImportOptionsHash = requestedImportOptionsHash;
    outError = {};
    return true;
}

bool IResourceLoader::CanLoad(const std::string& path) const
{
    std::filesystem::path fsPath(path);
    std::string ext = fsPath.extension().string();
    
    auto supported = GetSupportedExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

} // namespace RVX::Resource
