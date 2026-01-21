#include "Resource/ResourceManager.h"
#include "Resource/Loader/ModelLoader.h"
#include "Resource/Loader/TextureLoader.h"
#include "Scene/ComponentFactory.h"
#include "Core/Log.h"
#include <algorithm>
#include <filesystem>

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

ResourceManager::ResourceManager() = default;

ResourceManager::~ResourceManager()
{
    Shutdown();
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

    // Register default component factories
    ComponentFactory::RegisterDefaults();

    m_initialized = true;
    RVX_RESOURCE_INFO("ResourceManager initialized");
}

void ResourceManager::RegisterDefaultLoaders()
{
    // Register TextureLoader
    auto textureLoader = std::make_unique<TextureLoader>(this);
    RegisterLoader(ResourceType::Texture, std::move(textureLoader));

    // Register ModelLoader
    auto modelLoader = std::make_unique<ModelLoader>(this);
    RegisterLoader(ResourceType::Model, std::move(modelLoader));

    RVX_RESOURCE_INFO("Registered default resource loaders");
}

void ResourceManager::Shutdown()
{
    if (!m_initialized) return;

    m_loaders.clear();
    m_cache->Clear();
    m_registry->Clear();
    m_dependencyGraph->Clear();

    m_initialized = false;
    RVX_RESOURCE_INFO("ResourceManager shutdown");
}

IResource* ResourceManager::LoadResource(const std::string& path)
{
    if (!m_initialized)
    {
        RVX_RESOURCE_ERROR("ResourceManager not initialized");
        return nullptr;
    }

    // Generate resource ID
    ResourceId id = GenerateResourceId(path);

    // Check cache first
    if (IResource* cached = m_cache->Get(id))
    {
        return cached;
    }

    // Determine resource type from extension
    std::filesystem::path fsPath(path);
    std::string ext = fsPath.extension().string();
    ResourceType type = GetTypeFromExtension(ext);

    return LoadInternal(path, type);
}

IResource* ResourceManager::LoadResource(ResourceId id)
{
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

    return LoadInternal(metadata->path, metadata->type);
}

IResource* ResourceManager::LoadInternal(const std::string& path, ResourceType type)
{
    std::string resolvedPath = ResolvePath(path);

    // Get appropriate loader
    IResourceLoader* loader = GetLoader(type);
    if (!loader)
    {
        RVX_RESOURCE_ERROR("No loader registered for resource type: {}", GetResourceTypeName(type));
        return nullptr;
    }

    // Load the resource
    IResource* resource = loader->Load(resolvedPath);
    if (!resource)
    {
        RVX_RESOURCE_ERROR("Failed to load resource: {}", path);
        return nullptr;
    }

    // Set resource properties
    resource->SetId(GenerateResourceId(path));
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

    // Mark as loaded
    resource->NotifyLoaded();

    RVX_RESOURCE_DEBUG("Loaded resource: {} (type: {})", path, GetResourceTypeName(type));
    return resource;
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
    if (m_cache) m_cache->Clear();
    if (m_registry) m_registry->Clear();
    if (m_dependencyGraph) m_dependencyGraph->Clear();
}

void ResourceManager::EnableHotReload(bool enable)
{
    m_config.enableHotReload = enable;
}

void ResourceManager::CheckForChanges()
{
    if (!m_config.enableHotReload) return;
    // TODO: Implement file watching and hot reload
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
    m_loaders[type] = std::move(loader);
}

IResourceLoader* ResourceManager::GetLoader(ResourceType type)
{
    auto it = m_loaders.find(type);
    return it != m_loaders.end() ? it->second.get() : nullptr;
}

void ResourceManager::ProcessCompletedLoads()
{
    // TODO: Process async load completions
}

ResourceManager::Stats ResourceManager::GetStats() const
{
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

// IResourceLoader implementation
bool IResourceLoader::CanLoad(const std::string& path) const
{
    std::filesystem::path fsPath(path);
    std::string ext = fsPath.extension().string();
    
    auto supported = GetSupportedExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

} // namespace RVX::Resource
