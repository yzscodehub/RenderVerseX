#include "Resource/ResourceManager.h"
#include "Resource/HotReloadManager.h"
#include "Resource/Loader/MeshLoader.h"
#include "Resource/Loader/ModelLoader.h"
#include "Resource/Loader/ShaderLoader.h"
#include "Resource/Loader/TextureLoader.h"
#include "Core/Diagnostics/JsonWriter.h"
#include "Core/Log.h"
#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

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
        diagnostic.message = message;
        diagnostic.sourceAssetRead = resolution.sourceAssetRead;
        diagnostic.cookedArtifactRead = resolution.cookedArtifactRead;
        diagnostic.runtimePackageRead = resolution.runtimePackageRead;
        return diagnostic;
    }
} // namespace

ResourceManager::ResourceManager() = default;

ResourceManager::~ResourceManager()
{
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
    m_registry = std::make_unique<ResourceRegistry>();
    m_cache = std::make_unique<ResourceCache>(config.cacheConfig);
    m_dependencyGraph = std::make_unique<DependencyGraph>();

    // Register default loaders
    RegisterDefaultLoaders();

    m_initialized = true;
    ConfigureHotReload(config.enableHotReload);
    StartAsyncWorkers();
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

    RVX_RESOURCE_INFO("Registered default resource loaders");
}

void ResourceManager::Shutdown()
{
    if (!m_initialized) return;

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
    ProcessCompletedLoads();

    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    m_loaders.clear();
    m_cache->Clear();
    m_registry->Clear();
    m_dependencyGraph->Clear();

    m_initialized = false;
    RVX_RESOURCE_INFO("ResourceManager shutdown");
}

IResource* ResourceManager::LoadResource(const std::string& path)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);

    if (!m_initialized)
    {
        RVX_RESOURCE_ERROR("ResourceManager not initialized");
        return nullptr;
    }

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, path);
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

    // Generate resource ID
    ResourceId id = GenerateResourceId(path);

    // Check cache first
    if (IResource* cached = m_cache->Get(id))
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               true,
                               ResourceLoadFailureCode::None,
                               "Resource returned from cache."));
        return cached;
    }

    // Determine resource type from extension
    std::filesystem::path fsPath(resolution.resolvedPath);
    std::string ext = fsPath.extension().string();
    ResourceType type = GetTypeFromExtension(ext);
    if (type == ResourceType::Unknown)
    {
        std::string lowerExt = ext;
        std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::tolower);
        if (lowerExt == ".rva")
        {
            type = DetectCookedArtifactType(resolution.resolvedPath);
        }
    }

    return LoadInternal(path, resolution, type);
}

IResource* ResourceManager::LoadResource(ResourceId id)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);

    if (!m_initialized) return nullptr;

    // Check cache first
    if (IResource* cached = m_cache->Get(id))
    {
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

    return LoadInternal(metadata->path, resolution, metadata->type);
}

IResource* ResourceManager::LoadInternal(const std::string& path,
                                         const ResourcePathResolution& resolution,
                                         ResourceType type)
{
    const std::string& resolvedPath = resolution.resolvedPath;

    // Get appropriate loader
    IResourceLoader* loader = GetLoader(type);
    if (!loader)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderUnavailable,
                               "No loader registered for resource type."));
        RVX_RESOURCE_ERROR("No loader registered for resource type: {}", GetResourceTypeName(type));
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
        return nullptr;
    }

    // Some loaders cache specialized variants internally while loading. The
    // ResourceManager path API owns the generic path identity, so remove any
    // loader-assigned cache entry before rewriting the ID to avoid one object
    // being stored under two keys.
    const ResourceId managerId = GenerateResourceId(path);
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

    SetLastLoadDiagnostic(
        MakeLoadDiagnostic(resolution,
                           true,
                           ResourceLoadFailureCode::None,
                           "Resource loaded successfully."));
    RegisterHotReloadResource(resource, resolution);

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
    ResourceId id = GenerateResourceId(path);
    return m_cache && m_cache->Contains(id);
}

bool ResourceManager::IsLoaded(ResourceId id) const
{
    return m_cache && m_cache->Contains(id);
}

void ResourceManager::Unload(const std::string& path)
{
    Unload(GenerateResourceId(path));
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
    if (m_reloadCallback && HotReloadManager::Get().IsInitialized())
    {
        HotReloadManager::Get().OnReload(
            [this](const ReloadEvent& event)
            {
                if (event.success && m_reloadCallback)
                {
                    m_reloadCallback(event.resourceId, event.newResource);
                }
            });
    }
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
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    m_loaders[type] = std::move(loader);
}

IResourceLoader* ResourceManager::GetLoader(ResourceType type)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    auto it = m_loaders.find(type);
    return it != m_loaders.end() ? it->second.get() : nullptr;
}

void ResourceManager::ProcessCompletedLoads()
{
    JobSystem::Get().ProcessMainThreadCompletions();
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
    JobSystem& jobSystem = JobSystem::Get();
    if (jobSystem.IsInitialized())
    {
        jobSystem.WaitAllPending();
    }

    if (m_jobSystemInitializedByManager)
    {
        jobSystem.Shutdown();
        m_jobSystemInitializedByManager = false;
    }
}

// IResourceLoader implementation
bool IResourceLoader::CanLoad(const std::string& path) const
{
    std::filesystem::path fsPath(path);
    std::string ext = fsPath.extension().string();
    
    auto supported = GetSupportedExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

} // namespace RVX::Resource
