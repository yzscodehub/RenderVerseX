# Resource Management System Design

## Design Philosophy: Separated Storage + Composite Loading

**核心设计决策**：采用"分离存储 + 组合加载"的混合式架构

```mermaid
graph TB
    subgraph import [Import Phase - 导入阶段]
        SourceFile[Source File<br/>FBX/glTF/USD] --> Importer[Asset Importer]
        Importer --> |Extract| MeshAsset[MeshAsset<br/>几何数据]
        Importer --> |Extract| MaterialAsset[MaterialAsset<br/>材质参数]
        Importer --> |Extract| TextureAsset[TextureAsset<br/>纹理数据]
        Importer --> |Extract| SkeletonAsset[SkeletonAsset<br/>骨骼层级]
        Importer --> |Extract| AnimationAsset[AnimationAsset<br/>动画片段]
        Importer --> |Generate| ModelAsset[ModelAsset<br/>清单/引用]
    end
    
    subgraph runtime [Runtime - 运行时]
        LoadModel[Load ModelAsset] --> |Auto-resolve| DepGraph[Dependency Graph]
        DepGraph --> |Required| LoadMesh[Load MeshAsset]
        DepGraph --> |Required| LoadMat[Load MaterialAsset]
        DepGraph --> |Required| LoadTex[Load TextureAsset]
        DepGraph --> |Optional| LoadSkel[Load SkeletonAsset]
        DepGraph --> |Lazy| LoadAnim[Load AnimationAsset]
    end
```

### Key Benefits

| 特性 | 说明 |

|------|------|

| **资源复用** | 多个角色共享同一动画/材质/纹理 |

| **按需加载** | 动画等可选资源延迟加载 |

| **精细内存管理** | 每种资源独立卸载 |

| **热重载** | 只重载变化的子资源 |

| **简洁API** | 用户只需 `Load<ModelAsset>("x.fbx")` |

### ModelAsset as Manifest

```cpp
// ModelAsset 不存储实际数据，只存储引用（清单）
class ModelAsset : public Asset {
public:
    // 必需组件 - 加载 Model 时自动加载
    AssetHandle<MeshAsset> mesh;
    std::vector<AssetHandle<MaterialAsset>> materials;
    
    // 可选组件 - 有骨骼时才加载
    AssetHandle<SkeletonAsset> skeleton;
    
    // 延迟加载 - 只存储ID，按需加载
    std::vector<AssetId> animationIds;
    
    // 便捷方法
    AssetHandle<AnimationAsset> LoadAnimation(const std::string& name);
    AssetHandle<AnimationAsset> LoadAnimation(size_t index);
    
    // 获取所有依赖
    std::vector<AssetId> GetRequiredDependencies() const override;
    std::vector<AssetId> GetOptionalDependencies() const override;
};
```

### Memory Efficiency Example

```
场景：100个士兵角色，3种动画，2种材质变体

整体式加载：
  100 × (Mesh + Materials + Animations) = 100 × 50MB = 5GB

分离式加载（共享资源）：
  1 × MeshAsset (30MB)
  2 × MaterialAsset (5MB each)
  3 × AnimationAsset (10MB each)
  100 × ModelAsset (引用只，~1KB each)
  = 30 + 10 + 30 + 0.1 = ~70MB

节省：98.6%
```

## Architecture Overview

```mermaid
graph TB
    subgraph app [Application Layer]
        GameCode[Game Code]
        Editor[Editor]
    end
    
    subgraph res [Resource System]
        ResourceManager[ResourceManager]
        AssetRegistry[AssetRegistry]
        LoaderDispatcher[LoaderDispatcher]
    end
    
    subgraph loaders [Resource Loaders]
        MeshLoader[MeshLoader]
        TextureLoader[TextureLoader]
        MaterialLoader[MaterialLoader]
        ShaderLoader[ShaderLoader]
        AnimLoader[AnimationLoader]
        AudioLoader[AudioLoader]
        SceneLoader[SceneLoader]
        ScriptLoader[ScriptLoader]
    end
    
    subgraph cache [Caching Layer]
        AssetCache[AssetCache]
        GPUResourcePool[GPUResourcePool]
        StreamingManager[StreamingManager]
    end
    
    subgraph storage [Storage Layer]
        FileSystem[FileSystem]
        AssetBundle[AssetBundle]
        VirtualFS[VirtualFileSystem]
    end
    
    GameCode --> ResourceManager
    Editor --> ResourceManager
    ResourceManager --> AssetRegistry
    ResourceManager --> LoaderDispatcher
    ResourceManager --> AssetCache
    
    LoaderDispatcher --> MeshLoader
    LoaderDispatcher --> TextureLoader
    LoaderDispatcher --> MaterialLoader
    LoaderDispatcher --> ShaderLoader
    LoaderDispatcher --> AnimLoader
    LoaderDispatcher --> AudioLoader
    LoaderDispatcher --> SceneLoader
    LoaderDispatcher --> ScriptLoader
    
    AssetCache --> GPUResourcePool
    AssetCache --> StreamingManager
    
    MeshLoader --> VirtualFS
    TextureLoader --> VirtualFS
    VirtualFS --> FileSystem
    VirtualFS --> AssetBundle
```

## Module Structure

### 依赖的外部模块

Asset 模块依赖以下模块的数据类型：

| 依赖模块 | 使用的类型 | 用途 |
|----------|-----------|------|
| **Core** | `RefCounted`, `Math types` | 基础设施 |
| **Scene** | `Mesh`, `MaterialData`, `Node` | 数据定义 |
| **Spatial** | `BoundingBox`, `BoundingSphere` | 边界体积 |
| **Animation** | `Skeleton`, `AnimationClip` | 动画数据 |
| **RHI** | `BufferHandle`, `TextureHandle` | GPU 资源 |
| **ShaderCompiler** | `CompiledShader` | 编译后着色器 |

### 目录结构

```
Asset/
  CMakeLists.txt
  Include/
    Asset/
      # ============ Core Framework ============
      Asset.h                    # Base asset class (uses Core::RefCounted)
      AssetHandle.h              # Smart handle with ref counting
      AssetId.h                  # Asset identification (GUID/hash)
      AssetRegistry.h            # Asset metadata registry
      AssetCache.h               # In-memory cache
      AssetLoader.h              # Loader interface
      ResourceManager.h          # Main facade API
      
      # ============ Loading & Streaming ============
      AsyncLoader.h              # Async loading queue with thread pool
      StreamingManager.h         # Mipmap/LOD streaming
      DependencyGraph.h          # Dependency tracking & resolution
      LoadingQueue.h             # Priority-based load queue
      
      # ============ Storage ============
      VirtualFileSystem.h        # Unified file access
      AssetBundle.h              # Packed asset format (.rvxpak)
      AssetDatabase.h            # Asset metadata DB (.rvxdb)
      FileWatcher.h              # File change detection
      
      # ============ Primitive Asset Types ============
      # 封装来自其他模块的数据类型，提供生命周期管理
      Types/
        MeshAsset.h              # 封装 Scene::Mesh + Spatial::BoundingBox
        TextureAsset.h           # 纹理数据 + GPU 资源
        MaterialAsset.h          # 封装 Scene::MaterialData + 纹理引用
        ShaderAsset.h            # 编译后的着色器
        SkeletonAsset.h          # 封装 Animation::Skeleton
        AnimationAsset.h         # 封装 Animation::AnimationClip
        AudioAsset.h             # 音频数据
        ScriptAsset.h            # 脚本代码
      
      # ============ Composite Asset Types ============
      # 存储引用/清单的资源类型
      Types/
        ModelAsset.h             # Mesh + Materials + Skeleton + Animations 引用
        SceneAsset.h             # 封装 Scene::Node 层级 + Models 引用
        PrefabAsset.h            # 可复用的实体模板
      
      # ============ Loaders (Importers) ============
      Loaders/
        IAssetLoader.h           # Loader interface
        MeshLoader.h             # FBX, glTF, OBJ → MeshAsset
        TextureLoader.h          # PNG, JPG, DDS, KTX → TextureAsset
        MaterialLoader.h         # .mat files → MaterialAsset
        ShaderLoader.h           # HLSL → ShaderAsset (via ShaderCompiler)
        AnimationLoader.h        # FBX, glTF → AnimationAsset
        AudioLoader.h            # WAV, OGG, MP3 → AudioAsset
        SceneLoader.h            # .scene files → SceneAsset
        ModelLoader.h            # FBX, glTF → ModelAsset (orchestrates sub-loaders)
  
  Private/
    # Core implementations
    Asset.cpp
    AssetHandle.cpp
    AssetRegistry.cpp
    AssetCache.cpp
    ResourceManager.cpp
    
    # Loading
    AsyncLoader.cpp
    DependencyGraph.cpp
    StreamingManager.cpp
    
    # Storage
    VirtualFileSystem.cpp
    AssetBundle.cpp
    AssetDatabase.cpp
    FileWatcher.cpp
    
    # Asset types
    Types/
      MeshAsset.cpp
      TextureAsset.cpp
      MaterialAsset.cpp
      ...
    
    # Loaders
    Loaders/
      MeshLoader.cpp
      TextureLoader.cpp
      ...
      
    # Format-specific importers (from geometry importer migration)
    Importers/
      Assimp/
        AssimpImporter.cpp
        AssimpAnimationExtractor.cpp
        AssimpMaterialParser.cpp
      Fbx/
        FbxImporter.cpp
        FbxAnimationExtractor.cpp
        FbxMaterialParser.cpp
      Alembic/
        AlembicImporter.cpp
        AlembicAnimationExtractor.cpp
      Usd/
        UsdImporter.cpp
        UsdAnimationExtractor.cpp
        UsdMaterialConverter.cpp
```

### Loader vs Importer Distinction

| 组件 | 职责 | 示例 |

|------|------|------|

| **Loader** | 高层抽象，根据文件类型分发到Importer | `MeshLoader` 根据扩展名选择 Assimp/FBX/USD |

| **Importer** | 具体格式解析实现 | `AssimpImporter` 处理 glTF, OBJ, STL |

| **Extractor** | 从场景中提取特定数据 | `FbxAnimationExtractor` 提取骨骼动画 |

```mermaid
graph LR
    ModelLoader[ModelLoader] --> |.gltf/.glb| AssimpImporter
    ModelLoader --> |.fbx| FbxImporter
    ModelLoader --> |.abc| AlembicImporter
    ModelLoader --> |.usd/.usdz| UsdImporter
    
    AssimpImporter --> MeshAsset
    AssimpImporter --> MaterialAsset
    AssimpImporter --> AnimationAsset
    
    FbxImporter --> MeshAsset
    FbxImporter --> MaterialAsset
    FbxImporter --> AnimationAsset
```

## Core Components

### 1. Asset Base Class

```cpp
// Asset.h
namespace RVX::Asset {

using AssetId = uint64_t;  // GUID or hash

enum class AssetState {
    Unloaded,
    Loading,
    Loaded,
    Failed,
    Unloading
};

enum class AssetType : uint32_t {
    Unknown = 0,
    Mesh,
    Texture,
    Material,
    Shader,
    Animation,
    Audio,
    Scene,
    Script,
    // Extensible...
};

class Asset : public RefCounted {
public:
    virtual ~Asset() = default;
    
    AssetId GetId() const { return m_id; }
    AssetType GetType() const { return m_type; }
    AssetState GetState() const { return m_state; }
    const std::string& GetPath() const { return m_path; }
    
    // Dependencies
    const std::vector<AssetId>& GetDependencies() const;
    
    // Memory
    virtual size_t GetMemoryUsage() const = 0;
    virtual size_t GetGPUMemoryUsage() const { return 0; }
    
protected:
    AssetId m_id{0};
    AssetType m_type{AssetType::Unknown};
    std::atomic<AssetState> m_state{AssetState::Unloaded};
    std::string m_path;
    std::vector<AssetId> m_dependencies;
};

} // namespace RVX::Asset
```

### 2. Asset Handle (Reference Counting)

```cpp
// AssetHandle.h
template<typename T>
class AssetHandle {
public:
    AssetHandle() = default;
    explicit AssetHandle(T* asset);
    AssetHandle(const AssetHandle& other);
    AssetHandle(AssetHandle&& other) noexcept;
    ~AssetHandle();
    
    // Access
    T* Get() const { return m_asset; }
    T* operator->() const { return m_asset; }
    T& operator*() const { return *m_asset; }
    
    // State
    bool IsValid() const { return m_asset != nullptr; }
    bool IsLoaded() const;
    AssetState GetState() const;
    
    // Async wait
    void WaitForLoad() const;
    bool TryWaitForLoad(uint32_t timeoutMs) const;
    
private:
    T* m_asset{nullptr};
};

// Type aliases
using MeshHandle = AssetHandle<MeshAsset>;
using TextureHandle = AssetHandle<TextureAsset>;
using MaterialHandle = AssetHandle<MaterialAsset>;
// ...
```

### 3. Resource Manager (Main API)

```cpp
// ResourceManager.h
class ResourceManager {
public:
    static ResourceManager& Get();
    
    // Synchronous loading
    template<typename T>
    AssetHandle<T> Load(const std::string& path);
    
    template<typename T>
    AssetHandle<T> Load(AssetId id);
    
    // Asynchronous loading
    template<typename T>
    std::future<AssetHandle<T>> LoadAsync(const std::string& path);
    
    template<typename T>
    void LoadAsync(const std::string& path, 
                   std::function<void(AssetHandle<T>)> callback);
    
    // Batch loading
    void LoadBatch(const std::vector<std::string>& paths,
                   std::function<void(float progress)> onProgress = nullptr,
                   std::function<void()> onComplete = nullptr);
    
    // Unloading
    void Unload(AssetId id);
    void UnloadUnused();  // GC pass
    
    // Hot reload
    void EnableHotReload(bool enable);
    void CheckForChanges();  // Poll file changes
    
    // Cache control
    void SetCacheLimit(size_t bytes);
    void ClearCache();
    
    // Statistics
    struct Stats {
        size_t totalAssets;
        size_t loadedAssets;
        size_t pendingLoads;
        size_t memoryUsage;
        size_t gpuMemoryUsage;
    };
    Stats GetStats() const;
    
private:
    std::unique_ptr<AssetRegistry> m_registry;
    std::unique_ptr<AssetCache> m_cache;
    std::unique_ptr<AsyncLoader> m_asyncLoader;
    std::unique_ptr<VirtualFileSystem> m_vfs;
    std::unordered_map<AssetType, std::unique_ptr<AssetLoader>> m_loaders;
};
```

### 4. Async Loading System

```cpp
// AsyncLoader.h
class AsyncLoader {
public:
    struct LoadRequest {
        AssetId id;
        std::string path;
        AssetType type;
        int priority{0};  // Higher = more urgent
        std::function<void(Asset*)> callback;
    };
    
    void Submit(LoadRequest request);
    void Cancel(AssetId id);
    void SetThreadCount(int count);
    
    // Process completed loads on main thread
    void ProcessCallbacks();
    
private:
    std::priority_queue<LoadRequest> m_queue;
    std::vector<std::thread> m_workers;
    ThreadSafeQueue<std::pair<AssetId, Asset*>> m_completed;
};
```

### 5. Dependency Graph

```cpp
// DependencyGraph.h
class DependencyGraph {
public:
    void AddAsset(AssetId id, const std::vector<AssetId>& dependencies);
    void RemoveAsset(AssetId id);
    
    // Get load order (topological sort)
    std::vector<AssetId> GetLoadOrder(AssetId rootId) const;
    
    // Get all assets that depend on this one
    std::vector<AssetId> GetDependents(AssetId id) const;
    
    // Check for circular dependencies
    bool HasCircularDependency(AssetId id) const;
    
private:
    std::unordered_map<AssetId, std::vector<AssetId>> m_dependencies;
    std::unordered_map<AssetId, std::vector<AssetId>> m_dependents;
};
```

### 6. Virtual File System

```cpp
// VirtualFileSystem.h
class VirtualFileSystem {
public:
    // Mount points
    void Mount(const std::string& virtualPath, 
               const std::string& physicalPath);
    void MountBundle(const std::string& virtualPath,
                     const std::string& bundlePath);
    void Unmount(const std::string& virtualPath);
    
    // File operations
    bool Exists(const std::string& path) const;
    std::vector<uint8_t> ReadFile(const std::string& path) const;
    std::unique_ptr<IStream> OpenStream(const std::string& path) const;
    
    // Directory operations
    std::vector<std::string> ListDirectory(const std::string& path) const;
    
    // Watch for changes (hot reload)
    void Watch(const std::string& path,
               std::function<void(const std::string&)> callback);
    void StopWatching(const std::string& path);
    
private:
    struct MountPoint {
        std::string virtualPath;
        std::unique_ptr<IFileProvider> provider;
    };
    std::vector<MountPoint> m_mounts;
};
```

### 7. Asset Bundle Format

```cpp
// AssetBundle.h
struct BundleHeader {
    char magic[4] = {'R','V','X','B'};
    uint32_t version;
    uint32_t assetCount;
    uint32_t tocOffset;      // Table of contents
    uint32_t dataOffset;
    uint32_t flags;          // Compression, encryption
};

struct BundleEntry {
    AssetId id;
    AssetType type;
    uint64_t offset;
    uint64_t compressedSize;
    uint64_t uncompressedSize;
    uint32_t checksum;
    char path[256];
};

class AssetBundle {
public:
    static AssetBundle* Open(const std::string& path);
    static void Create(const std::string& outputPath,
                       const std::vector<std::string>& assetPaths);
    
    std::vector<uint8_t> ReadAsset(AssetId id) const;
    bool Contains(AssetId id) const;
    std::vector<AssetId> GetAllAssetIds() const;
    
private:
    std::unique_ptr<IStream> m_stream;
    std::unordered_map<AssetId, BundleEntry> m_entries;
};
```

## Resource Type Implementations

### Asset Type Hierarchy

```mermaid
graph TB
    Asset[Asset Base] --> PrimitiveAssets[Primitive Assets<br/>存储实际数据]
    Asset --> CompositeAssets[Composite Assets<br/>存储引用/清单]
    
    PrimitiveAssets --> MeshAsset[MeshAsset]
    PrimitiveAssets --> TextureAsset[TextureAsset]
    PrimitiveAssets --> MaterialAsset[MaterialAsset]
    PrimitiveAssets --> ShaderAsset[ShaderAsset]
    PrimitiveAssets --> SkeletonAsset[SkeletonAsset]
    PrimitiveAssets --> AnimationAsset[AnimationAsset]
    PrimitiveAssets --> AudioAsset[AudioAsset]
    PrimitiveAssets --> ScriptAsset[ScriptAsset]
    
    CompositeAssets --> ModelAsset[ModelAsset<br/>Mesh+Mat+Skel+Anim refs]
    CompositeAssets --> SceneAsset[SceneAsset<br/>Node hierarchy + Model refs]
    CompositeAssets --> PrefabAsset[PrefabAsset<br/>Reusable entity template]
```

### Model Asset (Composite - 组合资源)

```cpp
class ModelAsset : public Asset {
public:
    // ========== 必需依赖 (Required Dependencies) ==========
    // 加载 ModelAsset 时自动加载这些资源
    
    AssetHandle<MeshAsset> GetMesh() const { return m_mesh; }
    const std::vector<AssetHandle<MaterialAsset>>& GetMaterials() const { return m_materials; }
    
    // ========== 可选依赖 (Optional Dependencies) ==========
    // 存在时加载，不存在时为空
    
    bool HasSkeleton() const { return m_skeleton.IsValid(); }
    AssetHandle<SkeletonAsset> GetSkeleton() const { return m_skeleton; }
    
    // ========== 延迟加载 (Lazy Loading) ==========
    // 只存储ID，用户请求时才加载
    
    size_t GetAnimationCount() const { return m_animationIds.size(); }
    std::string GetAnimationName(size_t index) const;
    
    // 按需加载动画
    AssetHandle<AnimationAsset> LoadAnimation(size_t index);
    AssetHandle<AnimationAsset> LoadAnimation(const std::string& name);
    
    // 预加载所有动画
    void PreloadAllAnimations();
    
    // ========== 材质覆盖 (Material Override) ==========
    // 运行时替换材质，不影响原始资源
    
    void SetMaterialOverride(size_t slot, AssetHandle<MaterialAsset> material);
    void ClearMaterialOverrides();
    
    // ========== 依赖查询 ==========
    
    std::vector<AssetId> GetRequiredDependencies() const override {
        std::vector<AssetId> deps;
        deps.push_back(m_mesh.GetId());
        for (const auto& mat : m_materials) {
            deps.push_back(mat.GetId());
        }
        return deps;
    }
    
    std::vector<AssetId> GetOptionalDependencies() const override {
        std::vector<AssetId> deps;
        if (m_skeleton.IsValid()) {
            deps.push_back(m_skeleton.GetId());
        }
        deps.insert(deps.end(), m_animationIds.begin(), m_animationIds.end());
        return deps;
    }
    
private:
    // 必需
    AssetHandle<MeshAsset> m_mesh;
    std::vector<AssetHandle<MaterialAsset>> m_materials;
    
    // 可选
    AssetHandle<SkeletonAsset> m_skeleton;
    
    // 延迟加载
    std::vector<AssetId> m_animationIds;
    std::vector<std::string> m_animationNames;
    std::unordered_map<size_t, AssetHandle<AnimationAsset>> m_loadedAnimations;
    
    // 运行时覆盖
    std::unordered_map<size_t, AssetHandle<MaterialAsset>> m_materialOverrides;
};
```

### Mesh Asset (Primitive - 原始资源)

```cpp
// Asset/Include/Asset/Types/MeshAsset.h

// 注意：MeshAsset 封装 Scene::Mesh，提供资源生命周期和 GPU 资源管理
// Scene::Mesh 定义在 Scene/Mesh.h

class MeshAsset : public Asset {
public:
    // ========== CPU 数据（来自 Scene 模块）==========
    
    std::shared_ptr<Scene::Mesh> GetMesh() const { return m_mesh; }
    
    // ========== GPU 资源（延迟创建）==========
    
    RHI::BufferHandle GetVertexBuffer();
    RHI::BufferHandle GetIndexBuffer();
    
    // GPU 资源管理
    void UploadToGPU(RHI::Device* device);
    void ReleaseGPUResources();
    bool IsGPUResident() const;
    
    // ========== LOD 支持 ==========
    
    int GetLODCount() const;
    std::shared_ptr<Scene::Mesh> GetLOD(int level) const;
    void SetLODBias(float bias);
    
    // ========== 边界（来自 Spatial 模块）==========
    
    const Spatial::BoundingBox& GetBounds() const { return m_bounds; }
    
    // ========== 内存统计 ==========
    
    size_t GetMemoryUsage() const override;
    size_t GetGPUMemoryUsage() const override;
    
private:
    // 核心数据（来自 Scene 模块）
    std::shared_ptr<Scene::Mesh> m_mesh;
    std::vector<std::shared_ptr<Scene::Mesh>> m_lods;
    
    // 边界（来自 Spatial 模块）
    Spatial::BoundingBox m_bounds;
    
    // GPU 资源（惰性创建）
    RHI::BufferHandle m_vertexBuffer;
    RHI::BufferHandle m_indexBuffer;
    bool m_gpuDirty{true};
};
```

> **设计说明**：
> - `Scene::Mesh` 是纯几何数据（顶点、索引）
> - `Spatial::BoundingBox` 提供边界体积
> - `Asset::MeshAsset` 封装它们，管理 GPU 资源上传/卸载

### Material Asset (Primitive - 原始资源)

```cpp
// Asset/Include/Asset/Types/MaterialAsset.h

// 注意：MaterialAsset 封装 Scene::MaterialData，提供资源生命周期管理
// Scene::MaterialData 是纯数据结构，定义在 Scene/Material/MaterialData.h

class MaterialAsset : public Asset {
public:
    // ========== 数据访问 ==========
    
    // 获取底层材质数据（来自 Scene 模块）
    const Scene::MaterialData& GetMaterialData() const { return m_data; }
    Scene::MaterialData& GetMaterialData() { return m_data; }
    
    // 便捷访问
    const std::string& GetName() const { return m_data.name; }
    const std::string& GetTemplateName() const { return m_data.templateName; }
    
    // ========== 纹理引用（已解析为 Asset 句柄）==========
    
    AssetHandle<TextureAsset> GetTexture(const std::string& slot) const;
    void SetTexture(const std::string& slot, AssetHandle<TextureAsset> texture);
    
    // ========== 参数访问（委托给 MaterialData）==========
    
    template<typename T>
    T GetParameter(const std::string& name) const;
    
    void SetFloat(const std::string& name, float value);
    void SetVector(const std::string& name, const Vec4& value);
    
    // ========== 运行时实例创建 ==========
    
    // 创建可修改的运行时实例
    std::unique_ptr<Scene::MaterialInstance> CreateInstance() const;
    
    // ========== Shader 引用 ==========
    
    AssetHandle<ShaderAsset> GetShader() const { return m_shader; }
    
    // ========== 依赖管理 ==========
    
    std::vector<AssetId> GetRequiredDependencies() const override {
        std::vector<AssetId> deps;
        for (const auto& [slot, tex] : m_resolvedTextures) {
            if (tex.IsValid()) deps.push_back(tex.GetId());
        }
        if (m_shader.IsValid()) deps.push_back(m_shader.GetId());
        return deps;
    }
    
    size_t GetMemoryUsage() const override;
    
private:
    // 核心数据（来自 Scene 模块）
    Scene::MaterialData m_data;
    
    // 已解析的纹理引用（从 m_data.textures 路径解析）
    std::unordered_map<std::string, AssetHandle<TextureAsset>> m_resolvedTextures;
    
    // Shader 引用
    AssetHandle<ShaderAsset> m_shader;
};
```

> **设计说明**：
> - `Scene::MaterialData` 是纯数据结构，存储参数和纹理路径
> - `Asset::MaterialAsset` 封装它，负责解析纹理路径为 `TextureAsset` 句柄
> - 这种分离确保 Scene 模块不依赖 Asset 系统

### Texture Asset (Primitive - 原始资源)

```cpp
class TextureAsset : public Asset {
public:
    // GPU 纹理
    RHI::TextureHandle GetTexture() const { return m_texture; }
    
    // 元数据
    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    RHI::Format GetFormat() const;
    uint32_t GetMipLevels() const;
    bool IsCubemap() const;
    bool IsArray() const;
    
    // Mipmap 流式加载
    bool IsStreaming() const;
    void RequestMipLevel(int level);
    int GetResidentMipLevel() const;
    int GetRequestedMipLevel() const;
    
    // 内存
    size_t GetMemoryUsage() const override { return 0; }  // CPU已释放
    size_t GetGPUMemoryUsage() const override;
    
private:
    RHI::TextureHandle m_texture;
    TextureMetadata m_metadata;
    
    // 流式加载状态
    int m_residentMipLevel{0};
    int m_requestedMipLevel{0};
};
```

### Skeleton Asset (Primitive - 原始资源)

```cpp
// Asset/Include/Asset/Types/SkeletonAsset.h

// 注意：SkeletonAsset 封装 Animation::Skeleton
// Animation::Skeleton 定义在 Animation/Data/Skeleton.h

class SkeletonAsset : public Asset {
public:
    // ========== 骨骼数据（来自 Animation 模块）==========
    
    const Animation::Skeleton& GetSkeleton() const { return *m_skeleton; }
    std::shared_ptr<Animation::Skeleton> GetSkeletonPtr() const { return m_skeleton; }
    
    // ========== 便捷查询 ==========
    
    int GetBoneCount() const;
    int FindBoneIndex(const std::string& name) const;
    const std::string& GetBoneName(int index) const;
    
    // 绑定姿态
    const Mat4& GetBindPose(int boneIndex) const;
    const Mat4& GetInverseBindPose(int boneIndex) const;
    
    // ========== 内存统计 ==========
    
    size_t GetMemoryUsage() const override;
    
private:
    // 核心数据（来自 Animation 模块）
    std::shared_ptr<Animation::Skeleton> m_skeleton;
};
```

### Animation Asset (Primitive - 原始资源)

```cpp
// Asset/Include/Asset/Types/AnimationAsset.h

// 注意：AnimationAsset 封装 Animation::AnimationClip
// Animation::AnimationClip 定义在 Animation/Data/AnimationClip.h

class AnimationAsset : public Asset {
public:
    // ========== 动画数据（来自 Animation 模块）==========
    
    const Animation::AnimationClip& GetClip() const { return *m_clip; }
    std::shared_ptr<Animation::AnimationClip> GetClipPtr() const { return m_clip; }
    
    // ========== 元数据 ==========
    
    const std::string& GetName() const;
    float GetDuration() const;  // 秒
    float GetFrameRate() const;
    int GetFrameCount() const;
    
    // ========== 兼容性检查 ==========
    
    bool IsCompatibleWith(const SkeletonAsset& skeleton) const;
    bool IsCompatibleWith(const Animation::Skeleton& skeleton) const;
    
    // ========== 内存统计 ==========
    
    size_t GetMemoryUsage() const override;
    
private:
    // 核心数据（来自 Animation 模块）
    std::shared_ptr<Animation::AnimationClip> m_clip;
};
```

> **设计说明**：
> - `Animation::Skeleton` 和 `Animation::AnimationClip` 是纯数据类
> - `Asset::SkeletonAsset` 和 `Asset::AnimationAsset` 封装它们，提供生命周期管理
> - 这种分离确保 Animation 模块可以独立于 Asset 系统使用

### Scene Asset (Composite - 组合资源)

```cpp
// Asset/Include/Asset/Types/SceneAsset.h

// 注意：SceneAsset 存储场景的序列化数据
// Scene::SceneManager 负责运行时管理，通过 LoadFromAsset() 加载 SceneAsset

class SceneAsset : public Asset {
public:
    // ========== 场景层级（使用 Scene 模块的 Node）==========
    
    std::shared_ptr<Scene::Node> GetRootNode() const { return m_rootNode; }
    
    // ========== 资源引用 ==========
    
    // 包含的模型引用
    const std::vector<AssetHandle<ModelAsset>>& GetModels() const;
    
    // 包含的材质引用（场景级材质覆盖）
    const std::vector<AssetHandle<MaterialAsset>>& GetMaterials() const;
    
    // ========== 场景设置 ==========
    
    const SceneSettings& GetSettings() const;  // 光照、天空盒、环境等
    
    // ========== 实例化 ==========
    
    // 创建节点树副本（用于运行时修改）
    std::shared_ptr<Scene::Node> Instantiate() const;
    
    // 加载到 SceneManager（推荐方式）
    // 使用: sceneManager.LoadFromAsset(sceneAsset.Get());
    
    // ========== 依赖管理 ==========
    
    std::vector<AssetId> GetRequiredDependencies() const override {
        std::vector<AssetId> deps;
        for (const auto& model : m_models) {
            if (model.IsValid()) deps.push_back(model.GetId());
        }
        for (const auto& mat : m_materials) {
            if (mat.IsValid()) deps.push_back(mat.GetId());
        }
        return deps;
    }
    
    size_t GetMemoryUsage() const override;
    
private:
    // 节点层级（使用 Scene 模块的类型）
    std::shared_ptr<Scene::Node> m_rootNode;
    
    // 资源引用
    std::vector<AssetHandle<ModelAsset>> m_models;
    std::vector<AssetHandle<MaterialAsset>> m_materials;
    
    // 场景设置
    SceneSettings m_settings;
};
```

> **与 SceneManager 的关系**：
> - `SceneAsset` 是静态数据（可序列化、可缓存）
> - `SceneManager` 是运行时管理器（管理活动实体、空间查询）
> - 加载流程：`ResourceManager.Load<SceneAsset>()` → `SceneManager.LoadFromAsset()`

## Implementation Phases

> **注意**：本路线图与 Unified Scene and Spatial System 的实现阶段协调。
> Asset 系统依赖 Scene/Spatial 提供的数据类型。

### 前置依赖（来自 Scene/Spatial 设计）

Asset 模块开始前，需要以下模块就绪：

| 依赖模块 | 所需类型 | 对应 Scene/Spatial Phase |
|----------|---------|-------------------------|
| **Spatial** | `BoundingBox`, `BoundingSphere` | Phase 1 |
| **Scene** | `Mesh`, `Node`, `Transform` | Phase 2 |
| **Scene** | `MaterialData`, `MaterialTemplate` | Phase 3 |
| **Animation** | `Skeleton`, `AnimationClip` | 已存在 |

### Phase 1: Core Framework (基础架构)

**目标**：建立资源系统的核心骨架

**前置依赖**：Spatial Phase 1 完成

- `Asset` 基类 + 引用计数 (利用现有 `Core/RefCounted.h`)
- `AssetHandle<T>` 智能句柄
- `AssetId` 生成 (路径哈希 + GUID)
- `AssetRegistry` 元数据注册表
- `AssetCache` 内存缓存 (LRU策略)
- `ResourceManager` 单例门面
- `VirtualFileSystem` 统一文件访问 (仅文件系统)
- `IAssetLoader` 接口定义

**产出文件**：

- `Asset/Include/Asset/Asset.h`
- `Asset/Include/Asset/AssetHandle.h`
- `Asset/Include/Asset/AssetRegistry.h`
- `Asset/Include/Asset/AssetCache.h`
- `Asset/Include/Asset/ResourceManager.h`
- `Asset/Include/Asset/VirtualFileSystem.h`

### Phase 2: First Primitive Assets (首批原始资源)

**目标**：实现最常用的资源类型

**前置依赖**：Scene Phase 2-3 完成

- `TextureAsset` + `TextureLoader` (PNG, JPG, DDS)
- `MeshAsset` + `MeshLoader` - 封装 `Scene::Mesh` + `Spatial::BoundingBox`
- `MaterialAsset` + `MaterialLoader` - 封装 `Scene::MaterialData`
- `ShaderAsset` + `ShaderLoader` (集成 ShaderCompiler 模块)

**集成关系**：

```cpp
// MeshAsset 封装 Scene::Mesh
class MeshAsset : public Asset {
    std::shared_ptr<Scene::Mesh> m_mesh;
    Spatial::BoundingBox m_bounds;
};

// MaterialAsset 封装 Scene::MaterialData
class MaterialAsset : public Asset {
    Scene::MaterialData m_data;
    std::unordered_map<std::string, AssetHandle<TextureAsset>> m_resolvedTextures;
};
```

### Phase 3: Model Composite Asset (模型组合资源)

**目标**：实现分离存储+组合加载的核心

- `ModelAsset` (清单资源)
- `ModelLoader` (协调子加载器)
- `DependencyGraph` 依赖图
- 自动依赖解析
- 延迟加载机制 (动画)

**关键实现**：

```cpp
// 加载 ModelAsset 时自动触发依赖链
Load<ModelAsset>("x.fbx")
  → ModelLoader.Load()
    → 解析FBX获取子资源ID
    → DependencyGraph.GetLoadOrder()
    → 并行加载 MeshAsset, MaterialAsset[], TextureAsset[]
    → 组装 ModelAsset
```

### Phase 4: Scene/Spatial 集成

**目标**：实现与 SceneManager 的无缝集成

**前置依赖**：Scene Phase 4 完成

- `SceneAsset` - 封装 `Scene::Node` 层级
- `PrefabAsset` - 可复用实体模板
- `SceneLoader` - .scene 文件解析
- 与 `SceneManager` 的集成测试

**集成验证**：

```cpp
// 完整加载流程验证
auto sceneAsset = ResourceManager::Get().Load<SceneAsset>("level.scene");
sceneManager.LoadFromAsset(sceneAsset.Get());

// 验证空间查询正常工作
std::vector<SceneEntity*> visible;
sceneManager.QueryVisible(camera, visible);
```

### Phase 5: Animation Assets (动画资源)

**目标**：完善动画支持

- `SkeletonAsset` + `SkeletonLoader` - 封装 `Animation::Skeleton`
- `AnimationAsset` + `AnimationLoader` - 封装 `Animation::AnimationClip`
- Animation Extractors 集成
- 延迟加载动画验证

**集成关系**：

```cpp
// SkeletonAsset 封装 Animation::Skeleton
class SkeletonAsset : public Asset {
    std::shared_ptr<Animation::Skeleton> m_skeleton;
};

// AnimationAsset 封装 Animation::AnimationClip
class AnimationAsset : public Asset {
    std::shared_ptr<Animation::AnimationClip> m_clip;
};
```

### Phase 6: Geometry Importers (几何导入器)

**目标**：迁移现有导入器到资源系统

- Assimp 导入器 (glTF, GLB, OBJ, STL)
- FBX 导入器 (需要 FBX SDK)
- Alembic 导入器 (ABC)
- USD 导入器 (USD/USDZ)

**迁移来源**：`D:\Work\found\src\renderer\resouce\importer\geometry\`

### Phase 7: Async Loading (异步加载)

**目标**：支持非阻塞资源加载

- `AsyncLoader` 线程池
- 优先级加载队列
- 回调系统
- 进度追踪
- 批量加载

### Phase 8: Advanced Features (高级功能)

**目标**：生产级功能

- `FileWatcher` 文件监控
- 热重载系统
- `AssetBundle` 打包/解包
- 纹理 Mipmap 流式加载
- 内存预算管理
- `AudioAsset` + `ScriptAsset`

### Phase Summary

| Phase | 内容 | 依赖 | Scene/Spatial 依赖 |
|-------|------|------|-------------------|
| 1 | Core Framework | - | Spatial Phase 1 |
| 2 | Primitive Assets | Phase 1 | Scene Phase 2-3 |
| 3 | ModelAsset | Phase 2 | - |
| 4 | Scene 集成 | Phase 3 | Scene Phase 4 |
| 5 | Animation Assets | Phase 3 | Animation 模块 |
| 6 | Geometry Importers | Phase 3 | - |
| 7 | Async Loading | Phase 1-3 | - |
| 8 | Advanced Features | All | - |

### 统一实现顺序

```mermaid
gantt
    title Asset + Scene/Spatial 统一实现路线
    dateFormat  X
    axisFormat %s
    
    section Scene/Spatial
    Spatial Phase 1 - 边界体积  :s1, 0, 1
    Scene Phase 2 - 实体重构    :s2, after s1, 1
    Scene Phase 3 - 材质数据    :s3, after s2, 1
    Scene Phase 4 - Asset 集成  :s4, after a4, 1
    
    section Asset
    Asset Phase 1 - 核心框架    :a1, after s1, 1
    Asset Phase 2 - 原始资源    :a2, after s3, 1
    Asset Phase 3 - ModelAsset  :a3, after a2, 1
    Asset Phase 4 - Scene 集成  :a4, after a3, 1
    Asset Phase 5 - 动画资源    :a5, after a3, 1
    
    section 后续
    Phase 6 - 导入器            :p6, after a4, 1
    Phase 7 - 异步加载          :p7, after a4, 1
    Phase 8 - 高级功能          :p8, after p7, 1
```

## Integration with Existing Modules

### 与 Scene/Spatial 系统的统一架构

本设计与 Unified Scene and Spatial System Design 协同工作：

```mermaid
graph TB
    subgraph data_layer [数据定义层]
        Mesh[Scene::Mesh]
        MaterialData[Scene::MaterialData]
        Skeleton[Animation::Skeleton]
        AnimClip[Animation::AnimationClip]
    end
    
    subgraph asset_layer [资源管理层]
        MeshAsset[Asset::MeshAsset]
        MaterialAsset[Asset::MaterialAsset]
        TextureAsset[Asset::TextureAsset]
        SkeletonAsset[Asset::SkeletonAsset]
        AnimationAsset[Asset::AnimationAsset]
        ModelAsset[Asset::ModelAsset]
        SceneAsset[Asset::SceneAsset]
    end
    
    subgraph runtime_layer [运行时层]
        SceneManager[Scene::SceneManager]
        SceneEntity[Scene::SceneEntity]
        MaterialInstance[Scene::MaterialInstance]
        SpatialIndex[Spatial::ISpatialIndex]
    end
    
    Mesh --> MeshAsset
    MaterialData --> MaterialAsset
    Skeleton --> SkeletonAsset
    AnimClip --> AnimationAsset
    
    MeshAsset --> ModelAsset
    MaterialAsset --> ModelAsset
    SkeletonAsset --> ModelAsset
    AnimationAsset --> ModelAsset
    
    ModelAsset --> SceneAsset
    
    SceneAsset --> |LoadFromAsset| SceneManager
    ModelAsset --> |InstantiateModel| SceneManager
    
    SceneManager --> SceneEntity
    SceneEntity --> SpatialIndex
    MaterialAsset --> MaterialInstance
```

### 模块依赖关系

```mermaid
graph LR
    Asset[Asset Module] --> Scene[Scene Module]
    Asset --> Spatial[Spatial Module]
    Asset --> Animation[Animation Module]
    Asset --> ShaderCompiler[ShaderCompiler]
    Asset --> RHI[RHI]
    
    Scene --> |Mesh, MaterialData, Node| Asset
    Spatial --> |BoundingBox| Asset
    Animation --> |Skeleton, Clips| Asset
    ShaderCompiler --> |Compiled Shaders| Asset
    RHI --> |GPU Resources| Asset
```

### 职责划分

| 模块 | 职责 | 提供给 Asset 的类型 |
|------|------|-------------------|
| **Scene** | 数据结构定义、运行时管理 | `Mesh`, `MaterialData`, `Node` |
| **Spatial** | 边界体积、空间查询 | `BoundingBox`, `BoundingSphere` |
| **Animation** | 骨骼/动画数据 | `Skeleton`, `AnimationClip` |
| **RHI** | GPU 资源抽象 | Buffer/Texture 句柄 |
| **ShaderCompiler** | 着色器编译 | 编译后的 SPIR-V/字节码 |

## Usage Examples

### Basic Loading with SceneManager Integration

```cpp
// ========== 资源加载（Asset 层）==========

// 加载模型 - 自动解析并加载所有必需依赖 (mesh, materials, textures)
auto modelAsset = ResourceManager::Get().Load<ModelAsset>("models/character.fbx");

// 访问子资源 - 已自动加载
auto meshAsset = modelAsset->GetMesh();           // Asset::MeshAsset
auto materials = modelAsset->GetMaterials();       // std::vector<Asset::MaterialAsset>
auto skeletonAsset = modelAsset->GetSkeleton();   // Asset::SkeletonAsset (如果存在)

// ========== 场景实例化（Scene 层）==========

// 方式 1：通过 SceneManager 实例化模型
auto entity = sceneManager.InstantiateModel(modelAsset.Get(), Transform::Identity());

// 方式 2：手动创建实体并设置
auto entity = sceneManager.CreateEntity<ModelEntity>("Player");
entity->SetMeshAsset(meshAsset);
entity->SetMaterials(materials);

// ========== 空间查询（Spatial 层）==========

// 实体自动注册到 ISpatialIndex
std::vector<SceneEntity*> visible;
sceneManager.QueryVisible(camera, visible);  // 视锥剔除

// ========== 渲染（Render 层）==========

for (auto* entity : visible) {
    auto* meshAsset = entity->GetMeshAsset();
    auto* material = entity->GetMaterial();
    
    // GPU 资源绑定
    materialBinder.Bind(ctx, material);
    ctx->DrawIndexed(meshAsset->GetVertexBuffer(), meshAsset->GetIndexBuffer());
}
```

### Scene Loading with SceneManager

```cpp
// ========== 加载场景资源 ==========

auto sceneAsset = ResourceManager::Get().Load<SceneAsset>("levels/level1.scene");

// SceneAsset 自动加载所有依赖的 ModelAsset、MaterialAsset、TextureAsset

// ========== 加载到 SceneManager ==========

// 方式 1：完全替换当前场景
sceneManager.LoadFromAsset(sceneAsset.Get());

// 方式 2：增量添加（保留现有实体）
auto rootNode = sceneAsset->Instantiate();  // 创建节点树副本
sceneManager.AddHierarchy(rootNode);

// ========== 场景已就绪，可以进行查询 ==========

// 视锥剔除
std::vector<SceneEntity*> visible;
sceneManager.QueryVisible(camera, visible);

// 光线拾取
RaycastHit hit;
if (sceneManager.Raycast(mouseRay, hit)) {
    LOG_INFO("Hit entity: {}", hit.entity->GetName());
}
```

### Lazy Animation Loading

```cpp
// 模型加载时动画不会自动加载
auto modelAsset = ResourceManager::Get().Load<ModelAsset>("models/character.fbx");

// 查看可用动画
for (size_t i = 0; i < modelAsset->GetAnimationCount(); ++i) {
    LOG_INFO("Animation {}: {}", i, modelAsset->GetAnimationName(i));
}

// 需要时才加载动画
auto walkAnim = modelAsset->LoadAnimation("walk");   // 返回 AssetHandle<AnimationAsset>
auto runAnim = modelAsset->LoadAnimation("run");

// 获取底层动画数据
const Animation::AnimationClip& walkClip = walkAnim->GetClip();

// 使用动画
animator->Play(walkAnim);
animator->CrossFade(runAnim, 0.3f);
```

### Resource Sharing

```cpp
// 100个士兵共享同一模型资源
std::vector<Entity> soldiers;
auto soldierModel = ResourceManager::Get().Load<ModelAsset>("models/soldier.fbx");

for (int i = 0; i < 100; ++i) {
    Entity soldier;
    soldier.model = soldierModel;  // 共享引用，不复制数据
    soldier.transform = RandomTransform();
    soldiers.push_back(soldier);
}

// 内存中只有 1 份 mesh、material、texture 数据
```

### Material Override

```cpp
auto model = ResourceManager::Get().Load<ModelAsset>("models/car.fbx");

// 创建材质变体 - 红色车
auto redPaint = ResourceManager::Get().Load<MaterialAsset>("materials/red_paint.mat");
model->SetMaterialOverride(0, redPaint);

// 另一辆车 - 蓝色
auto blueCarModel = ResourceManager::Get().Load<ModelAsset>("models/car.fbx");
auto bluePaint = ResourceManager::Get().Load<MaterialAsset>("materials/blue_paint.mat");
blueCarModel->SetMaterialOverride(0, bluePaint);

// 两辆车共享相同的 mesh，但使用不同的材质
```

### Async Loading with Progress

```cpp
// 异步加载 - 适合大型资源
ResourceManager::Get().LoadAsync<ModelAsset>("models/huge_building.fbx",
    [](AssetHandle<ModelAsset> model) {
        if (model.IsValid()) {
            scene->AddModel(model);
        }
    });

// 批量加载场景资源
std::vector<std::string> levelAssets = {
    "models/terrain.fbx",
    "models/buildings.fbx",
    "models/vegetation.fbx",
    "textures/skybox.hdr"
};

ResourceManager::Get().LoadBatch(levelAssets,
    [](float progress) {
        loadingUI->SetProgress(progress);
        loadingUI->SetText(fmt::format("Loading... {:.0f}%", progress * 100));
    },
    []() {
        loadingUI->Hide();
        gameState->StartLevel();
    }
);
```

### Hot Reload (Editor Mode)

```cpp
// 启用热重载
ResourceManager::Get().EnableHotReload(true);

// 游戏循环中检查文件变化
void EditorUpdate() {
    ResourceManager::Get().CheckForChanges();
    // 修改磁盘上的纹理/材质/shader后，自动重新加载
}

// 监听特定资源变化
ResourceManager::Get().OnAssetReloaded([](AssetId id, Asset* asset) {
    if (asset->GetType() == AssetType::Shader) {
        LOG_INFO("Shader reloaded: {}", asset->GetPath());
        renderer->InvalidatePipelines();
    }
});
```

### Direct Sub-Asset Access

```cpp
// 也可以直接加载子资源，用于高级用例

// 直接加载纹理
auto texture = ResourceManager::Get().Load<TextureAsset>("textures/grass_albedo.png");

// 直接加载材质
auto material = ResourceManager::Get().Load<MaterialAsset>("materials/water.mat");

// 直接加载动画（用于共享动画库）
auto genericWalk = ResourceManager::Get().Load<AnimationAsset>("animations/generic_walk.anim");

// 应用到任何兼容骨骼的模型
if (genericWalk->IsCompatibleWith(*model->GetSkeleton())) {
    animator->Play(genericWalk);
}
```

### Memory Management

```cpp
// 获取内存统计
auto stats = ResourceManager::Get().GetStats();
LOG_INFO("Assets: {}/{} loaded", stats.loadedAssets, stats.totalAssets);
LOG_INFO("CPU Memory: {} MB", stats.memoryUsage / (1024 * 1024));
LOG_INFO("GPU Memory: {} MB", stats.gpuMemoryUsage / (1024 * 1024));

// 设置内存预算
ResourceManager::Get().SetCacheLimit(512 * 1024 * 1024);  // 512 MB

// 卸载未使用的资源 (引用计数为0)
ResourceManager::Get().UnloadUnused();

// 强制卸载特定资源
ResourceManager::Get().Unload(model->GetId());
```

---

## Related Documents

### 与 Scene/Spatial System 的关系

本设计与 [Unified Scene and Spatial System Design](./Unified%20Scene%20and%20Spatial%20System%20Design.md) 紧密协作：

| Asset 系统封装 | 来自 Scene/Spatial |
|---------------|-------------------|
| `MeshAsset` | `Scene::Mesh` + `Spatial::BoundingBox` |
| `MaterialAsset` | `Scene::MaterialData` |
| `SceneAsset` | `Scene::Node` 层级 |
| `SkeletonAsset` | `Animation::Skeleton` |
| `AnimationAsset` | `Animation::AnimationClip` |

### 职责划分总结

| 层次 | 模块 | 职责 |
|------|------|------|
| **数据定义** | Scene, Spatial, Animation | 定义纯数据结构（Mesh, MaterialData, Skeleton） |
| **资源管理** | Asset | 封装数据，提供生命周期管理（加载、缓存、卸载） |
| **运行时管理** | Scene (SceneManager) | 管理活动实体，空间查询 |
| **GPU 绑定** | Render | 材质绑定，渲染管线 |

### 设计原则

1. **Scene/Spatial 模块不依赖 Asset 系统**：确保数据类型可独立使用
2. **Asset 系统封装 Scene/Spatial 类型**：提供资源生命周期管理
3. **SceneManager 可选择使用 Asset**：通过接口方法集成，不强制依赖
4. **命名清晰区分**：`MaterialData`（数据）vs `MaterialAsset`（资源）