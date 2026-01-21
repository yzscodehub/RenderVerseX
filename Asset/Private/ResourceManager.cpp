#include "Asset/ResourceManager.h"
#include "Core/Log.h"
#include <algorithm>
#include <filesystem>

namespace RVX::Asset
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
        RVX_ASSET_WARN("ResourceManager already initialized");
        return;
    }

    m_config = config;
    m_registry = std::make_unique<AssetRegistry>();
    m_cache = std::make_unique<AssetCache>(config.cacheConfig);
    m_dependencyGraph = std::make_unique<DependencyGraph>();

    m_initialized = true;
    RVX_ASSET_INFO("ResourceManager initialized");
}

void ResourceManager::Shutdown()
{
    if (!m_initialized) return;

    m_loaders.clear();
    m_cache->Clear();
    m_registry->Clear();
    m_dependencyGraph->Clear();

    m_initialized = false;
    RVX_ASSET_INFO("ResourceManager shutdown");
}

Asset* ResourceManager::LoadAsset(const std::string& path)
{
    if (!m_initialized)
    {
        RVX_ASSET_ERROR("ResourceManager not initialized");
        return nullptr;
    }

    // Generate asset ID
    AssetId id = GenerateAssetId(path);

    // Check cache first
    if (Asset* cached = m_cache->Get(id))
    {
        return cached;
    }

    // Determine asset type from extension
    std::filesystem::path fsPath(path);
    std::string ext = fsPath.extension().string();
    AssetType type = GetTypeFromExtension(ext);

    return LoadInternal(path, type);
}

Asset* ResourceManager::LoadAsset(AssetId id)
{
    if (!m_initialized) return nullptr;

    // Check cache first
    if (Asset* cached = m_cache->Get(id))
    {
        return cached;
    }

    // Look up path in registry
    auto metadata = m_registry->FindById(id);
    if (!metadata)
    {
        RVX_ASSET_ERROR("Asset not found in registry: {}", id);
        return nullptr;
    }

    return LoadInternal(metadata->path, metadata->type);
}

Asset* ResourceManager::LoadInternal(const std::string& path, AssetType type)
{
    std::string resolvedPath = ResolvePath(path);

    // Get appropriate loader
    IAssetLoader* loader = GetLoader(type);
    if (!loader)
    {
        RVX_ASSET_ERROR("No loader registered for asset type: {}", GetAssetTypeName(type));
        return nullptr;
    }

    // Load the asset
    Asset* asset = loader->Load(resolvedPath);
    if (!asset)
    {
        RVX_ASSET_ERROR("Failed to load asset: {}", path);
        return nullptr;
    }

    // Set asset properties
    asset->SetId(GenerateAssetId(path));
    asset->SetPath(path);
    asset->SetName(std::filesystem::path(path).stem().string());

    // Load dependencies
    LoadDependencies(asset);

    // Register in registry
    AssetMetadata metadata;
    metadata.id = asset->GetId();
    metadata.path = path;
    metadata.name = asset->GetName();
    metadata.type = type;
    metadata.dependencies = asset->GetAllDependencies();
    m_registry->Register(metadata);

    // Add to dependency graph
    m_dependencyGraph->AddAsset(asset->GetId(), metadata.dependencies);

    // Store in cache
    m_cache->Store(asset);

    // Mark as loaded
    asset->NotifyLoaded();

    RVX_ASSET_DEBUG("Loaded asset: {} (type: {})", path, GetAssetTypeName(type));
    return asset;
}

void ResourceManager::LoadDependencies(Asset* asset)
{
    auto dependencies = asset->GetRequiredDependencies();
    for (AssetId depId : dependencies)
    {
        // Load each dependency (will be cached)
        LoadAsset(depId);
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
        LoadAsset(path);
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

void ResourceManager::Unload(AssetId id)
{
    m_cache->Remove(id);
    m_registry->Unregister(id);
    m_dependencyGraph->RemoveAsset(id);
}

void ResourceManager::UnloadUnused()
{
    m_cache->EvictUnused();
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

void ResourceManager::OnAssetReloaded(std::function<void(AssetId, Asset*)> callback)
{
    m_reloadCallback = std::move(callback);
}

void ResourceManager::SetCacheLimit(size_t bytes)
{
    m_cache->SetMemoryLimit(bytes);
}

void ResourceManager::ClearCache()
{
    m_cache->Clear();
}

void ResourceManager::RegisterLoader(AssetType type, std::unique_ptr<IAssetLoader> loader)
{
    m_loaders[type] = std::move(loader);
}

IAssetLoader* ResourceManager::GetLoader(AssetType type)
{
    auto it = m_loaders.find(type);
    return it != m_loaders.end() ? it->second.get() : nullptr;
}

ResourceManager::Stats ResourceManager::GetStats() const
{
    Stats stats;

    if (m_registry)
    {
        stats.totalAssets = m_registry->GetCount();
    }

    if (m_cache)
    {
        auto cacheStats = m_cache->GetStats();
        stats.loadedAssets = cacheStats.totalAssets;
        stats.memoryUsage = cacheStats.memoryUsage;
        stats.gpuMemoryUsage = cacheStats.gpuMemoryUsage;
    }

    return stats;
}

AssetType ResourceManager::GetTypeFromExtension(const std::string& extension)
{
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Mesh formats
    if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb")
        return AssetType::Mesh;

    // Texture formats
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" || 
        ext == ".tga" || ext == ".bmp" || ext == ".hdr")
        return AssetType::Texture;

    // Material
    if (ext == ".mat" || ext == ".material")
        return AssetType::Material;

    // Shader
    if (ext == ".hlsl" || ext == ".glsl" || ext == ".shader")
        return AssetType::Shader;

    // Animation
    if (ext == ".anim" || ext == ".animation")
        return AssetType::Animation;

    // Audio
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
        return AssetType::Audio;

    // Scene
    if (ext == ".scene")
        return AssetType::Scene;

    // Model (composite)
    if (ext == ".model")
        return AssetType::Model;

    return AssetType::Unknown;
}

std::string ResourceManager::ResolvePath(const std::string& path) const
{
    if (m_config.basePath.empty())
    {
        return path;
    }

    std::filesystem::path basePath(m_config.basePath);
    std::filesystem::path assetPath(path);

    if (assetPath.is_absolute())
    {
        return path;
    }

    return (basePath / assetPath).string();
}

// IAssetLoader implementation
bool IAssetLoader::CanLoad(const std::string& path) const
{
    std::filesystem::path fsPath(path);
    std::string ext = fsPath.extension().string();
    
    auto supported = GetSupportedExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

} // namespace RVX::Asset
