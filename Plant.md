# RenderVerseX RHI 架构设计方案

## 1. 整体架构

```mermaid
graph TB
    subgraph AppLayer [Application Layer]
        App[Application]
        Scene[Scene Manager]
    end

    subgraph RenderLayer [Render Layer]
        RenderGraph[RenderGraph]
        FrameGraph[Frame Resource Manager]
    end

    subgraph RHILayer [RHI Abstraction]
        RHIDevice[RHIDevice]
        RHICommandContext[RHICommandContext]
        RHIResources[RHI Resources]
        RHIPipeline[RHIPipeline]
    end

    subgraph Backends [Backend Implementations]
        DX11[DX11 Backend]
        DX12[DX12 Backend]
        Vulkan[Vulkan Backend]
    end

    App --> RenderGraph
    Scene --> RenderGraph
    RenderGraph --> RHIDevice
    RenderGraph --> RHICommandContext
    FrameGraph --> RHIResources
    RHIDevice --> DX11
    RHIDevice --> DX12
    RHIDevice --> Vulkan
```

## 2. 目录结构

```
RenderVerseX/
├── CMakeLists.txt               # 根构建文件
├── CMakePresets.json            # CMake预设配置
├── vcpkg.json                   # vcpkg依赖清单
├── vcpkg-configuration.json     # vcpkg仓库配置
├── .gitignore
│
├── RHI/
│   ├── CMakeLists.txt
│   ├── Include/RHI/
│   │   ├── RHI.h                    # 统一头文件
│   │   ├── RHIDefinitions.h         # 枚举、常量、格式定义
│   │   ├── RHIDevice.h              # 设备抽象
│   │   ├── RHICommandContext.h      # 命令上下文
│   │   ├── RHIResources.h           # 资源基类
│   │   ├── RHIBuffer.h              # Buffer资源
│   │   ├── RHITexture.h             # Texture资源
│   │   ├── RHISampler.h             # 采样器
│   │   ├── RHIShader.h              # Shader抽象
│   │   ├── RHIPipeline.h            # PSO抽象
│   │   ├── RHIRenderPass.h          # RenderPass定义
│   │   ├── RHIDescriptor.h          # 描述符/绑定组
│   │   ├── RHIDescriptorAllocator.h # 描述符分配器 (新增)
│   │   ├── RHIPipelineLayoutCache.h # Pipeline Layout缓存 (新增)
│   │   ├── RHISwapChain.h           # 交换链
│   │   ├── RHISynchronization.h     # 同步原语
│   │   └── RHICapabilities.h        # 设备能力查询
│   ├── Private/
│   │   ├── RHIModule.cpp            # 后端动态加载
│   │   ├── RHIValidation.cpp        # 调试验证层
│   │   └── DescriptorCache.cpp      # 描述符缓存 (新增)
│   └── Shaders/
│       └── ShaderCompiler.h         # Shader编译接口
│
├── RHI_DX11/
│   ├── CMakeLists.txt
│   ├── Include/DX11/
│   │   ├── DX11Device.h
│   │   └── DX11Capabilities.h       # DX11 特有配置 (新增)
│   └── Private/
│       ├── DX11Device.cpp
│       ├── DX11CommandContext.cpp
│       ├── DX11Resources.cpp
│       ├── DX11Pipeline.cpp
│       └── DX11BindingRemapper.cpp  # Slot 映射器 (新增)
│
├── RHI_DX12/
│   ├── CMakeLists.txt
│   ├── Include/DX12/
│   │   └── DX12Device.h
│   └── Private/
│       ├── DX12Device.cpp
│       ├── DX12CommandContext.cpp
│       ├── DX12DescriptorHeap.cpp
│       ├── DX12Resources.cpp
│       └── DX12Pipeline.cpp
│
├── RHI_Vulkan/
│   ├── CMakeLists.txt
│   ├── Include/Vulkan/
│   │   └── VulkanDevice.h
│   └── Private/
│       ├── VulkanDevice.cpp
│       ├── VulkanCommandContext.cpp
│       ├── VulkanDescriptorPool.cpp
│       ├── VulkanResources.cpp
│       └── VulkanPipeline.cpp
│
├── RenderGraph/
│   ├── CMakeLists.txt
│   ├── Include/RenderGraph/
│   │   ├── RenderGraph.h            # 主入口
│   │   ├── RenderGraphPass.h        # Pass定义
│   │   ├── RenderGraphResource.h    # 虚拟资源
│   │   └── RenderGraphBuilder.h     # 构建器
│   └── Private/
│       ├── RenderGraph.cpp
│       ├── RenderGraphCompiler.cpp  # 依赖分析、屏障插入
│       └── RenderGraphExecutor.cpp  # 执行器
│
├── ShaderCompiler/
│   ├── CMakeLists.txt
│   ├── Include/ShaderCompiler/
│   │   ├── ShaderCompiler.h
│   │   ├── ShaderReflection.h
│   │   ├── ShaderPermutation.h      # 变体管理 (新增)
│   │   └── DX11SlotMapping.h        # DX11 Slot 映射定义
│   └── Private/
│       ├── DXCCompiler.cpp          # HLSL编译
│       ├── SPIRVReflect.cpp         # SPIR-V反射
│       ├── ShaderCache.cpp          # Shader缓存
│       ├── ShaderPermutationCache.cpp  # 变体缓存 (新增)
│       └── DX11SlotMapper.cpp       # DX11 Slot 映射生成
│
├── Core/
│   ├── Include/Core/
│   │   ├── Types.h                  # 基础类型
│   │   ├── RefCounted.h             # 引用计数基类
│   │   ├── Handle.h                 # 句柄系统
│   │   └── ThreadSafe.h             # 线程安全工具
│   └── Private/
│       └── Memory.cpp
│
├── Samples/
│   └── Triangle/
│       └── main.cpp
│
└── Tests/                           # 验证测试 (新增)
    ├── CMakeLists.txt
    ├── TestFramework/
    │   ├── TestRunner.h
    │   ├── Assertions.h
    │   └── ImageCompare.h
    ├── DX12Validation/
    │   └── main.cpp
    ├── VulkanValidation/
    │   └── main.cpp
    ├── DX11Validation/
    │   └── main.cpp
    ├── RenderGraphValidation/
    │   └── main.cpp
    └── Shaders/
        ├── Triangle.hlsl
        └── TexturedQuad.hlsl
```

## 3. 核心数据模型

### 3.1 侵入式引用计数基类 (Intrusive RefCounting)

采用侵入式设计，避免双重堆分配，引用计数与对象在同一缓存行，性能更优。

**关键设计：使用回调接口解耦 Core 与 RenderLayer**

```cpp
// Core/Include/Core/RefCounted.h

// 延迟删除接口 - 解决 Core 依赖 RenderLayer 的循环依赖问题
class IDeferredDeleter {
public:
    virtual ~IDeferredDeleter() = default;
    virtual void DeferredDelete(const class RefCounted* object) = 0;
};

// 全局注册点 (在引擎初始化时由 FrameResourceManager 注册)
class DeferredDeleterRegistry {
    static IDeferredDeleter* s_deleter;
    
public:
    static void Register(IDeferredDeleter* deleter) { s_deleter = deleter; }
    static IDeferredDeleter* Get() { return s_deleter; }
};

// 侵入式引用计数基类
class RefCounted {
    mutable std::atomic<uint32_t> m_refCount{0};
    
protected:
    virtual ~RefCounted() = default;
    
public:
    void AddRef() const {
        m_refCount.fetch_add(1, std::memory_order_relaxed);
    }
    
    // Release 返回 true 表示引用归零，由调用方决定如何处理
    bool Release() const {
        return m_refCount.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }
    
    uint32_t GetRefCount() const { return m_refCount.load(std::memory_order_relaxed); }
};

// 智能指针 - 负责调用延迟删除
template<typename T>
class RHIRef {
    static_assert(std::is_base_of_v<RefCounted, T>, "T must inherit from RefCounted");
    T* m_ptr = nullptr;
    
    void TryDelete() {
        if (m_ptr && m_ptr->Release()) {
            // 引用归零，加入延迟删除队列
            if (auto* deleter = DeferredDeleterRegistry::Get()) {
                deleter->DeferredDelete(m_ptr);
            } else {
                // Fallback: 无延迟删除器时直接删除 (用于测试/工具)
                delete m_ptr;
            }
            m_ptr = nullptr;
        }
    }
    
public:
    RHIRef() = default;
    explicit RHIRef(T* ptr) : m_ptr(ptr) { if (m_ptr) m_ptr->AddRef(); }
    RHIRef(const RHIRef& other) : m_ptr(other.m_ptr) { if (m_ptr) m_ptr->AddRef(); }
    RHIRef(RHIRef&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
    ~RHIRef() { TryDelete(); }
    
    RHIRef& operator=(const RHIRef& other);
    RHIRef& operator=(RHIRef&& other) noexcept;
    
    T* Get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T& operator*() const { return *m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }
    
    void Reset(T* ptr = nullptr);
};

// RHI 资源基类
class RHIResource : public RefCounted {
protected:
    std::string m_debugName;
    
public:
    void SetDebugName(const char* name) { m_debugName = name ? name : ""; }
    const std::string& GetDebugName() const { return m_debugName; }
};

using RHIBufferRef = RHIRef<RHIBuffer>;
using RHITextureRef = RHIRef<RHITexture>;
using RHIPipelineRef = RHIRef<RHIPipeline>;
```

**初始化时注册：**

```cpp
// RenderGraph/Private/FrameResourceManager.cpp

class FrameResourceManager : public IDeferredDeleter {
public:
    void Initialize() {
        // 注册到 Core 层
        DeferredDeleterRegistry::Register(this);
    }
    
    void DeferredDelete(const RefCounted* object) override {
        // 加入当前帧的延迟删除队列
        m_frames[m_currentFrameIndex].pendingDeletes.push_back(object);
    }
};
```

### 3.2 核心枚举定义

```cpp
// RHI/Include/RHI/RHIDefinitions.h
enum class RHIBackendType { DX11, DX12, Vulkan };
enum class RHIFormat { R8_UNORM, RGBA8_UNORM, RGBA16_FLOAT, D24_UNORM_S8_UINT, ... };
enum class RHITextureUsage { ShaderResource, RenderTarget, DepthStencil, UnorderedAccess };
enum class RHIBufferUsage { Vertex, Index, Constant, Structured, IndirectArgs };
enum class RHIPrimitiveTopology { TriangleList, TriangleStrip, LineList, PointList };
enum class RHIResourceState { Common, RenderTarget, DepthWrite, ShaderResource, UnorderedAccess, CopyDest, CopySrc };
enum class RHICommandQueueType { Graphics, Compute, Copy };
```

### 3.3 设备接口

```cpp
// RHI/Include/RHI/RHIDevice.h
class IRHIDevice {
public:
    virtual ~IRHIDevice() = default;

    // 资源创建
    virtual RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) = 0;
    virtual RHITextureRef CreateTexture(const RHITextureDesc& desc) = 0;
    virtual RHISamplerRef CreateSampler(const RHISamplerDesc& desc) = 0;
    virtual RHIShaderRef CreateShader(const RHIShaderDesc& desc) = 0;
    virtual RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) = 0;
    virtual RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) = 0;

    // 命令上下文
    virtual RHICommandContextRef CreateCommandContext(RHICommandQueueType type) = 0;
    virtual void SubmitCommandContext(RHICommandContext* ctx, RHIFence* signalFence = nullptr) = 0;

    // 交换链
    virtual RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) = 0;

    // 同步
    virtual RHIFenceRef CreateFence(uint64_t initialValue = 0) = 0;
    virtual void WaitForFence(RHIFence* fence, uint64_t value) = 0;

    // 帧管理
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void WaitIdle() = 0;

    // 能力查询
    virtual const RHICapabilities& GetCapabilities() const = 0;
};

// 工厂函数
std::unique_ptr<IRHIDevice> CreateRHIDevice(RHIBackendType backend, const RHIDeviceDesc& desc);
```

### 3.4 命令上下文

```cpp
// RHI/Include/RHI/RHICommandContext.h

// Subresource 范围描述，用于细粒度屏障
struct RHISubresourceRange {
    uint32_t baseMipLevel = 0;
    uint32_t mipLevelCount = RHI_ALL_MIPS;      // ~0u 表示所有 mip
    uint32_t baseArrayLayer = 0;
    uint32_t arrayLayerCount = RHI_ALL_LAYERS;  // ~0u 表示所有 array slice
    RHITextureAspect aspect = RHITextureAspect::Color;  // Color, Depth, Stencil
};

// 资源屏障描述
struct RHITextureBarrier {
    RHITexture* texture;
    RHIResourceState stateBefore;
    RHIResourceState stateAfter;
    RHISubresourceRange subresourceRange;  // 支持 Subresource 级别屏障
};

struct RHIBufferBarrier {
    RHIBuffer* buffer;
    RHIResourceState stateBefore;
    RHIResourceState stateAfter;
    uint64_t offset = 0;
    uint64_t size = RHI_WHOLE_SIZE;  // ~0ull 表示整个 buffer
};

class IRHICommandContext {
public:
    // 生命周期
    virtual void Begin() = 0;
    virtual void End() = 0;
    virtual void Reset() = 0;

    // === 调试标记 (PIX/RenderDoc 支持) ===
    virtual void BeginEvent(const char* name, uint32_t color = 0) = 0;
    virtual void EndEvent() = 0;
    virtual void SetMarker(const char* name, uint32_t color = 0) = 0;

    // === 资源屏障 (支持 Subresource 粒度) ===
    virtual void ResourceBarrier(const RHITextureBarrier& barrier) = 0;
    virtual void ResourceBarrier(const RHIBufferBarrier& barrier) = 0;
    virtual void ResourceBarriers(std::span<const RHITextureBarrier> textureBarriers,
                                  std::span<const RHIBufferBarrier> bufferBarriers) = 0;
    
    // 便捷版本：整个资源的屏障
    void ResourceBarrier(RHITexture* texture, RHIResourceState before, RHIResourceState after);
    void ResourceBarrier(RHIBuffer* buffer, RHIResourceState before, RHIResourceState after);

    // RenderPass
    virtual void BeginRenderPass(const RHIRenderPassDesc& desc) = 0;
    virtual void EndRenderPass() = 0;

    // === 绑定 ===
    virtual void SetPipeline(RHIPipeline* pipeline) = 0;
    virtual void SetVertexBuffer(uint32_t slot, RHIBuffer* buffer, uint64_t offset = 0) = 0;
    virtual void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64_t offset = 0) = 0;
    
    // 支持 Dynamic Offsets (用于 Uniform Buffer 动态偏移，减少 Descriptor Set 数量)
    virtual void SetDescriptorSet(uint32_t slot, RHIDescriptorSet* set,
                                  std::span<const uint32_t> dynamicOffsets = {}) = 0;
    virtual void SetPushConstants(const void* data, uint32_t size, uint32_t offset = 0) = 0;

    // 视口/裁剪
    virtual void SetViewport(const RHIViewport& viewport) = 0;
    virtual void SetViewports(std::span<const RHIViewport> viewports) = 0;
    virtual void SetScissor(const RHIRect& scissor) = 0;
    virtual void SetScissors(std::span<const RHIRect> scissors) = 0;

    // 绘制
    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount = 1,
                      uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1,
                             uint32_t firstIndex = 0, int32_t vertexOffset = 0,
                             uint32_t firstInstance = 0) = 0;
    virtual void DrawIndirect(RHIBuffer* buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;
    virtual void DrawIndexedIndirect(RHIBuffer* buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;

    // Compute
    virtual void Dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;
    virtual void DispatchIndirect(RHIBuffer* buffer, uint64_t offset) = 0;

    // 拷贝
    virtual void CopyBuffer(RHIBuffer* src, RHIBuffer* dst,
                            uint64_t srcOffset, uint64_t dstOffset, uint64_t size) = 0;
    virtual void CopyTexture(RHITexture* src, RHITexture* dst,
                             const RHITextureCopyDesc& desc = {}) = 0;
    virtual void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst,
                                     const RHIBufferTextureCopyDesc& desc) = 0;
    virtual void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst,
                                     const RHIBufferTextureCopyDesc& desc) = 0;
};
```

### 3.5 资源描述符

```cpp
// RHI/Include/RHI/RHIBuffer.h
struct RHIBufferDesc {
    uint64_t size;
    RHIBufferUsage usage;
    RHIMemoryType memoryType;  // Default, Upload, Readback
    const char* debugName = nullptr;
};

// RHI/Include/RHI/RHITexture.h
struct RHITextureDesc {
    uint32_t width, height, depth;
    uint32_t mipLevels;
    uint32_t arraySize;
    RHIFormat format;
    RHITextureUsage usage;
    RHITextureDimension dimension;  // 1D, 2D, 3D, Cube
    RHISampleCount sampleCount;
    const char* debugName = nullptr;
};

// RHI/Include/RHI/RHIPipeline.h
struct RHIGraphicsPipelineDesc {
    RHIShader* vertexShader;
    RHIShader* pixelShader;
    RHIShader* geometryShader = nullptr;
    RHIShader* hullShader = nullptr;
    RHIShader* domainShader = nullptr;

    RHIInputLayout inputLayout;
    RHIRasterState rasterState;
    RHIDepthStencilState depthStencilState;
    RHIBlendState blendState;
    RHIPrimitiveTopology topology;

    uint32_t numRenderTargets;
    RHIFormat renderTargetFormats[8];
    RHIFormat depthStencilFormat;
    RHISampleCount sampleCount;
};
```

### 3.6 描述符绑定模型

采用类似 WebGPU 的 BindGroup 模型，但需特别处理 DX11 的扁平 Slot 模型。

```cpp
// RHI/Include/RHI/RHIDescriptor.h

struct RHIBindingLayoutEntry {
    uint32_t binding;
    RHIShaderStage visibility;
    RHIBindingType type;  // UniformBuffer, StorageBuffer, Texture, Sampler, StorageTexture
    uint32_t count = 1;   // 数组大小 (Bindless 时可为大数)
    bool isDynamic = false;  // 是否支持 Dynamic Offset
};

struct RHIBindGroupLayoutDesc {
    uint32_t entryCount;
    const RHIBindingLayoutEntry* entries;
};

struct RHIBindGroupEntry {
    uint32_t binding;
    RHIBuffer* buffer = nullptr;
    uint64_t offset = 0;
    uint64_t size = 0;
    RHITextureView* textureView = nullptr;  // 使用 View 而非 Texture
    RHISampler* sampler = nullptr;
};

class IRHIBindGroupLayout : public RHIResource { /* ... */ };
class IRHIBindGroup : public RHIResource { /* ... */ };

// PipelineLayout = 多个 BindGroupLayout 的组合
struct RHIPipelineLayoutDesc {
    uint32_t bindGroupLayoutCount;
    RHIBindGroupLayout* bindGroupLayouts[4];  // 最多4个 Set
    uint32_t pushConstantSize;
    RHIShaderStage pushConstantStages;
};
```

### 3.7 DX11 Slot 映射机制

DX11 只有扁平的 Slot 模型 (t0-t127, s0-s15, b0-b13, u0-u63)，需要在 Shader 编译时生成映射表。

```cpp
// ShaderCompiler/Include/ShaderCompiler/DX11SlotMapping.h

// Slot 映射策略：Set * 32 + Binding (可配置)
struct DX11SlotMappingConfig {
    uint32_t slotsPerSet = 32;  // 每个 Set 占用的槽位数
    // Set 0: Slot 0-31, Set 1: Slot 32-63, Set 2: Slot 64-95, Set 3: Slot 96-127
};

// 编译时生成的映射表
struct DX11BindingSlot {
    uint32_t set;
    uint32_t binding;
    RHIBindingType type;
    uint32_t dx11Slot;  // 映射后的 DX11 槽位
};

struct DX11SlotMapping {
    std::vector<DX11BindingSlot> textureSlots;   // t registers
    std::vector<DX11BindingSlot> samplerSlots;   // s registers  
    std::vector<DX11BindingSlot> cbufferSlots;   // b registers
    std::vector<DX11BindingSlot> uavSlots;       // u registers
};

// Shader 编译结果中包含映射信息
struct ShaderCompileResult {
    // ... 原有字段 ...
    DX11SlotMapping dx11SlotMapping;  // DX11 专用
};
```

### 3.8 Descriptor Cache 机制 (DX12/Vulkan)

频繁创建销毁 Descriptor Set 开销很大，使用线性分配器优化瞬态绑定。

```cpp
// RHI/Include/RHI/RHIDescriptorAllocator.h

// 线性描述符分配器 - 每帧重置
class LinearDescriptorAllocator {
public:
    // 从当前帧的池中分配
    RHIDescriptorSet* AllocateTransient(RHIBindGroupLayout* layout);
    
    // 帧结束时重置所有分配
    void Reset();
    
private:
    // DX12: ID3D12DescriptorHeap ring buffer
    // Vulkan: VkDescriptorPool per frame
};

// 缓存的描述符 - 用于持久化绑定
class DescriptorCache {
public:
    // 根据绑定内容的 hash 查找或创建
    RHIDescriptorSet* GetOrCreate(const RHIBindGroupDesc& desc);
    
    // LRU 清理
    void Cleanup(uint32_t maxAge);
    
private:
    std::unordered_map<uint64_t, CachedDescriptor> m_cache;
};

// 每帧资源管理器集成
class FrameResourceManager {
    // ...
    std::array<LinearDescriptorAllocator, FRAME_COUNT> m_descriptorAllocators;
    
public:
    LinearDescriptorAllocator& GetCurrentFrameDescriptorAllocator();
};
```

### 3.9 Pipeline Layout 缓存与 Bindless 策略

```cpp
// RHI/Include/RHI/RHIPipelineLayoutCache.h

class PipelineLayoutCache {
public:
    // 根据 layout 描述的 hash 查找或创建
    RHIPipelineLayout* GetOrCreate(const RHIPipelineLayoutDesc& desc);
    
private:
    std::unordered_map<uint64_t, RHIPipelineLayoutRef> m_cache;
};

// Bindless 资源表 (可选，用于大规模场景)
struct BindlessTableDesc {
    uint32_t maxTextures = 1000000;
    uint32_t maxBuffers = 100000;
    uint32_t maxSamplers = 2048;
};

class IBindlessResourceTable {
public:
    // 分配 Bindless 索引
    uint32_t AllocateTextureIndex(RHITextureView* view);
    uint32_t AllocateBufferIndex(RHIBuffer* buffer);
    
    // 释放索引 (延迟到帧结束)
    void FreeTextureIndex(uint32_t index);
    void FreeBufferIndex(uint32_t index);
    
    // 获取绑定的描述符表 (Shader 中通过索引访问)
    RHIDescriptorSet* GetBindlessDescriptorSet() const;
};
```

### 3.10 设备能力查询与 Bindless 回退

```cpp
// RHI/Include/RHI/RHICapabilities.h

struct RHICapabilities {
    RHIBackendType backendType;
    std::string adapterName;
    
    // 基础能力
    uint32_t maxTextureSize;
    uint32_t maxTextureLayers;
    uint32_t maxColorAttachments;
    uint32_t maxComputeWorkGroupSize[3];
    
    // Bindless 支持
    bool supportsBindless;              // 是否支持 Bindless 资源
    uint32_t maxBindlessTextures;       // 最大 Bindless 纹理数
    uint32_t maxBindlessBuffers;        // 最大 Bindless 缓冲区数
    
    // 高级特性
    bool supportsRaytracing;
    bool supportsMeshShaders;
    bool supportsVariableRateShading;
    bool supportsAsyncCompute;
    
    // DX11 特有
    struct DX11Caps {
        ThreadingMode threadingMode;
        uint32_t minDrawCallsForMultithread;
        bool supportsDeferredContext;
    } dx11;
    
    // DX12 特有
    struct DX12Caps {
        uint32_t resourceBindingTier;  // 1, 2, 3
        bool supportsRootSignature1_1;
    } dx12;
    
    // Vulkan 特有
    struct VulkanCaps {
        bool supportsDescriptorIndexing;
        bool supportsBufferDeviceAddress;
        uint32_t maxPushConstantSize;
    } vulkan;
};
```

### 3.11 Bindless 回退策略

DX11 不支持真正的 Bindless，需要在材质系统层面提供回退。

```cpp
// 材质系统使用示例

class Material {
public:
    void Apply(RHICommandContext& ctx, const RHICapabilities& caps) {
        if (caps.supportsBindless) {
            // 现代路径：使用 Bindless 索引
            // Shader: Texture2D textures[] : register(t0, space1);
            //         uint textureIndex = pushConstants.albedoIndex;
            //         float4 color = textures[textureIndex].Sample(...);
            
            ctx.SetDescriptorSet(0, m_bindlessTable->GetBindlessDescriptorSet());
            
            MaterialPushConstants pc;
            pc.albedoIndex = m_albedoTextureIndex;
            pc.normalIndex = m_normalTextureIndex;
            ctx.SetPushConstants(&pc, sizeof(pc));
            
        } else {
            // DX11 回退路径：传统绑定
            // Shader: Texture2D albedoTex : register(t0);
            //         Texture2D normalTex : register(t1);
            
            ctx.SetDescriptorSet(0, m_traditionalDescriptorSet);
        }
    }
    
private:
    // Bindless 模式
    IBindlessResourceTable* m_bindlessTable;
    uint32_t m_albedoTextureIndex;
    uint32_t m_normalTextureIndex;
    
    // 传统模式 (DX11 回退)
    RHIDescriptorSetRef m_traditionalDescriptorSet;
};

// Shader 变体自动选择
class MaterialShader {
public:
    RHIPipelineRef GetPipeline(const RHICapabilities& caps) {
        std::vector<ShaderMacro> macros;
        
        if (caps.supportsBindless) {
            macros.push_back({"USE_BINDLESS", "1"});
        }
        
        return m_pipelineCache.GetOrCreate(m_basePipelineDesc, macros);
    }
};
```

## 4. RenderGraph 设计

```mermaid
flowchart LR
    subgraph Setup [Setup Phase]
        AddPass1[AddPass: GBuffer]
        AddPass2[AddPass: Lighting]
        AddPass3[AddPass: PostProcess]
    end

    subgraph Compile [Compile Phase]
        BuildDAG[Build DAG]
        CullPasses[Cull Unused Passes]
        CalcBarriers[Calculate Barriers]
        AllocTransient[Allocate Transient Resources]
    end

    subgraph Execute [Execute Phase]
        ExecPass1[Execute GBuffer]
        ExecPass2[Execute Lighting]
        ExecPass3[Execute PostProcess]
    end

    AddPass1 --> BuildDAG
    AddPass2 --> BuildDAG
    AddPass3 --> BuildDAG
    BuildDAG --> CullPasses --> CalcBarriers --> AllocTransient
    AllocTransient --> ExecPass1 --> ExecPass2 --> ExecPass3
```
```cpp
// RenderGraph/Include/RenderGraph/RenderGraph.h

// 虚拟资源句柄 - Subresource 粒度
struct RGTextureHandle {
    uint32_t index;
    
    // Subresource 访问
    RGTextureHandle Subresource(uint32_t mipLevel, uint32_t arraySlice = 0) const;
    RGTextureHandle MipRange(uint32_t baseMip, uint32_t mipCount) const;
};

struct RGBufferHandle {
    uint32_t index;
    
    // 范围访问
    RGBufferHandle Range(uint64_t offset, uint64_t size) const;
};

// Pass 资源访问声明
class RenderGraphBuilder {
public:
    // 读取资源 (自动推断 ShaderResource 状态)
    RGTextureHandle Read(RGTextureHandle texture, 
                         RHIShaderStage stages = RHIShaderStage::AllGraphics);
    
    // 写入资源 (RenderTarget / UAV)
    RGTextureHandle Write(RGTextureHandle texture, 
                          RHIResourceState state = RHIResourceState::RenderTarget);
    
    // 读写资源 (UAV)
    RGTextureHandle ReadWrite(RGTextureHandle texture);
    
    // Subresource 级别的精细控制
    RGTextureHandle ReadMip(RGTextureHandle texture, uint32_t mipLevel);
    RGTextureHandle WriteMip(RGTextureHandle texture, uint32_t mipLevel);
    
    // 声明深度缓冲
    void SetDepthStencil(RGTextureHandle texture, 
                         bool depthWrite = true,
                         bool stencilWrite = false);
};

class RenderGraph {
public:
    // 声明虚拟资源
    RGTextureHandle CreateTexture(const RGTextureDesc& desc);
    RGBufferHandle CreateBuffer(const RGBufferDesc& desc);

    // 导入外部资源
    RGTextureHandle ImportTexture(RHITexture* texture, RHIResourceState initialState);
    RGBufferHandle ImportBuffer(RHIBuffer* buffer, RHIResourceState initialState);

    // 添加Pass
    template<typename Data>
    RenderGraphPass& AddPass(
        const char* name,
        RenderGraphPassType type,  // Graphics, Compute, Copy
        std::function<void(RenderGraphBuilder&, Data&)> setup,
        std::function<void(const Data&, RHICommandContext&)> execute
    );

    // 编译
    void Compile();
    
    // 执行 (自动处理 Subresource 级别的屏障)
    void Execute(RHICommandContext& ctx);
    
private:
    // 内部：Subresource 状态追踪
    struct SubresourceState {
        uint32_t mipLevel;
        uint32_t arraySlice;
        RHIResourceState state;
        uint32_t lastWritePass;
    };
    
    // 编译器计算精确的 Subresource 屏障
    void ComputeSubresourceBarriers();
};
```

**Subresource 屏障优化示例：**

```cpp
// 场景：Mip Chain 生成
graph.AddPass<MipGenData>("MipGen", RenderGraphPassType::Compute,
    [&](RenderGraphBuilder& builder, MipGenData& data) {
        // 精确声明每个 Mip 的读写关系
        for (uint32_t i = 1; i < mipCount; ++i) {
            data.srcMips[i-1] = builder.ReadMip(texture, i - 1);   // 读 Mip N-1
            data.dstMips[i] = builder.WriteMip(texture, i);         // 写 Mip N
        }
    },
    [](const MipGenData& data, RHICommandContext& ctx) {
        // 编译器会自动插入：Mip0 ShaderResource -> Mip1 UAV 的屏障
        // 而不是整个纹理的屏障
    }
);
```

## 5. Shader 编译系统

### 5.1 编译接口

```cpp
// ShaderCompiler/Include/ShaderCompiler/ShaderCompiler.h

struct ShaderMacro {
    std::string name;
    std::string value;
    
    bool operator==(const ShaderMacro& other) const;
    size_t Hash() const;
};

struct ShaderCompileOptions {
    RHIShaderStage stage;
    const char* entryPoint;
    const char* sourceCode;
    const char* sourcePath;  // 用于 #include 解析
    std::vector<ShaderMacro> defines;
    RHIBackendType targetBackend;
    bool enableDebugInfo;
};

struct ShaderCompileResult {
    bool success;
    std::vector<uint8_t> bytecode;
    ShaderReflection reflection;
    std::string errorMessage;
    uint64_t permutationHash;  // 变体唯一标识
};

class IShaderCompiler {
public:
    virtual ShaderCompileResult Compile(const ShaderCompileOptions& options) = 0;
};
```

### 5.2 Shader 变体管理 (Permutation System)

```cpp
// ShaderCompiler/Include/ShaderCompiler/ShaderPermutation.h

// 变体定义 - 声明 Shader 支持的所有宏组合
struct ShaderPermutationDef {
    std::string name;
    std::vector<std::string> options;  // 可能的值，空表示 0/1 布尔
};

// 变体集合
class ShaderPermutationSet {
public:
    void AddBoolPermutation(const char* name);  // 如 "USE_SOFT_SHADOW"
    void AddEnumPermutation(const char* name, std::initializer_list<const char*> options);
    
    // 计算总变体数
    uint32_t GetTotalPermutationCount() const;
    
    // 根据变体索引生成宏列表
    std::vector<ShaderMacro> GetMacrosForPermutation(uint32_t index) const;
    
    // 根据宏列表计算变体索引
    uint32_t GetPermutationIndex(const std::vector<ShaderMacro>& macros) const;
    
private:
    std::vector<ShaderPermutationDef> m_defs;
};

// 变体缓存 - 避免重复编译
class ShaderPermutationCache {
public:
    // 获取或编译变体
    RHIShaderRef GetOrCompile(
        const char* shaderPath,
        RHIShaderStage stage,
        const std::vector<ShaderMacro>& macros,
        RHIBackendType backend
    );
    
    // 预热：批量编译所有变体
    void Warmup(const char* shaderPath, const ShaderPermutationSet& permutations);
    
    // 持久化到磁盘
    void SaveToFile(const char* cachePath);
    void LoadFromFile(const char* cachePath);
    
private:
    // Key = Hash(shaderPath + stage + macros + backend)
    std::unordered_map<uint64_t, RHIShaderRef> m_cache;
};

// ShaderCompiler/Include/ShaderCompiler/ShaderReflection.h
struct ShaderReflection {
    struct ResourceBinding {
        std::string name;
        uint32_t set;
        uint32_t binding;
        RHIBindingType type;
        uint32_t count;
    };

    struct PushConstant {
        uint32_t offset;
        uint32_t size;
    };

    struct InputAttribute {
        std::string semantic;
        uint32_t location;
        RHIFormat format;
    };

    std::vector<ResourceBinding> resources;
    std::vector<PushConstant> pushConstants;
    std::vector<InputAttribute> inputs;
    std::vector<InputAttribute> outputs;
};
```

### 5.3 Pipeline 缓存与变体

```cpp
// RHI/Include/RHI/RHIPipelineCache.h

class RHIPipelineCache {
public:
    // Key = Hash(PipelineDesc + ShaderPermutationHash)
    RHIPipelineRef GetOrCreate(const RHIGraphicsPipelineDesc& desc);
    RHIPipelineRef GetOrCreate(const RHIComputePipelineDesc& desc);
    
    // 清理长时间未使用的 Pipeline
    void Cleanup(uint32_t maxAgeFrames);
    
private:
    struct CacheEntry {
        RHIPipelineRef pipeline;
        uint32_t lastUsedFrame;
    };
    
    std::unordered_map<uint64_t, CacheEntry> m_graphicsCache;
    std::unordered_map<uint64_t, CacheEntry> m_computeCache;
};
```

## 6. 多线程模型

```mermaid
sequenceDiagram
    participant Main as MainThread
    participant W1 as WorkerThread1
    participant W2 as WorkerThread2
    participant GPU as GPU Queue

    Main->>Main: BeginFrame()
    Main->>Main: RenderGraph.Compile()

    par Parallel Command Recording
        Main->>W1: RecordPass(GBuffer)
        Main->>W2: RecordPass(Shadow)
    end

    W1-->>Main: CommandContext1
    W2-->>Main: CommandContext2

    Main->>GPU: Submit(Context1, Context2)
    Main->>Main: EndFrame()
```

- 每个线程持有独立的 `RHICommandContext`
- 主线程负责 RenderGraph 编译和任务分发
- 工作线程并行录制命令
- 主线程统一提交到 GPU 队列

## 7. 帧资源管理

采用 Ring Buffer 模式管理帧资源，避免 GPU/CPU 资源竞争：

```cpp
constexpr uint32_t FRAME_COUNT = 3;  // 三重缓冲

class FrameResourceManager {
    struct FrameResources {
        RHIFenceRef frameFence;
        uint64_t fenceValue;
        std::vector<RHIResourceRef> pendingDeletes;  // 延迟删除队列
        // 每帧的动态资源池
    };

    std::array<FrameResources, FRAME_COUNT> frames;
    uint32_t currentFrameIndex;

public:
    void BeginFrame();
    void EndFrame();
    void DeferredDelete(RHIResourceRef resource);
};
```

## 8. 后端实现策略

| 特性 | DX11 | DX12 | Vulkan |

|------|------|------|--------|

| 命令录制 | Immediate Context + Deferred Context | Command List | Command Buffer |

| 资源屏障 | 自动 (驱动处理) | 手动 ResourceBarrier | 手动 Pipeline Barrier |

| 描述符 | 直接绑定 | Descriptor Heap | Descriptor Set |

| 内存管理 | 自动 | Committed/Placed Resource | VMA (Vulkan Memory Allocator) |

| Shader格式 | DXBC | DXIL | SPIR-V |

### DX11 特殊处理

**多线程降级策略：**

DX11 的 Deferred Context 性能开销通常比 DX12 的 CommandList 大，且驱动支持质量参差不齐。

```cpp
// RHI_DX11/Include/DX11/DX11Capabilities.h

struct DX11BackendConfig {
    enum class ThreadingMode {
        SingleThreaded,     // 始终单线程 (最安全)
        DeferredContext,    // 使用 Deferred Context
        Adaptive            // 根据 DrawCall 数量自适应
    };
    
    ThreadingMode threadingMode = ThreadingMode::Adaptive;
    uint32_t minDrawCallsForMultithread = 500;  // Adaptive 模式阈值
};
```

**Slot 映射实现 (预计算优化)：**

避免运行时遍历 vector 查表，在资源创建时预先扁平化。

```cpp
// RHI_DX11/Private/DX11DescriptorSet.h

// DX11 专用的扁平化描述符集
class DX11DescriptorSet : public RHIDescriptorSet {
public:
    // 在创建时预计算，将 Set/Binding 转换为 DX11 槽位的连续数组
    struct FlattenedBindings {
        // 按 DX11 槽位索引的连续数组，空槽位为 nullptr
        std::array<ID3D11ShaderResourceView*, 128> srvs{};
        std::array<ID3D11SamplerState*, 16> samplers{};
        std::array<ID3D11Buffer*, 14> cbuffers{};
        std::array<ID3D11UnorderedAccessView*, 64> uavs{};
        
        // 实际使用的范围，用于优化绑定调用
        uint32_t srvCount = 0;
        uint32_t samplerCount = 0;
        uint32_t cbufferCount = 0;
        uint32_t uavCount = 0;
    };
    
    FlattenedBindings m_flattened;
    
    // 创建时预计算扁平化绑定
    void Bake(const DX11SlotMapping& mapping);
};

// RHI_DX11/Private/DX11BindingRemapper.cpp

class DX11BindingRemapper {
public:
    // 高性能绑定 - 直接使用预计算的扁平数组
    void Apply(ID3D11DeviceContext* ctx, DX11DescriptorSet* set) {
        const auto& flat = set->m_flattened;
        
        // 单次调用绑定所有 SRV
        if (flat.srvCount > 0) {
            ctx->PSSetShaderResources(0, flat.srvCount, flat.srvs.data());
            ctx->VSSetShaderResources(0, flat.srvCount, flat.srvs.data());
        }
        
        // 单次调用绑定所有 Sampler
        if (flat.samplerCount > 0) {
            ctx->PSSetSamplers(0, flat.samplerCount, flat.samplers.data());
            ctx->VSSetSamplers(0, flat.samplerCount, flat.samplers.data());
        }
        
        // 单次调用绑定所有 CBV
        if (flat.cbufferCount > 0) {
            ctx->PSSetConstantBuffers(0, flat.cbufferCount, flat.cbuffers.data());
            ctx->VSSetConstantBuffers(0, flat.cbufferCount, flat.cbuffers.data());
        }
    }
};
```

**RenderGraph 执行策略自适应：**

```cpp
void RenderGraphExecutor::Execute(IRHIDevice* device, RenderGraph& graph) {
    const auto& caps = device->GetCapabilities();
    
    if (caps.backendType == RHIBackendType::DX11) {
        if (caps.dx11.threadingMode == ThreadingMode::Adaptive) {
            uint32_t totalDrawCalls = EstimateDrawCalls(graph);
            if (totalDrawCalls < caps.dx11.minDrawCallsForMultithread) {
                ExecuteSingleThreaded(device, graph);
                return;
            }
        }
    }
    
    ExecuteMultiThreaded(device, graph);
}
```

### DX12 特殊处理

- **Root Signature 缓存**：相同布局的 Pipeline 共享 Root Signature
- **Descriptor Heap 管理**：
  - Shader Visible Heap: CBV/SRV/UAV (GPU 可见)
  - Non-Shader Visible Heap: RTV/DSV
- **Residency 管理**：大型场景需要管理显存常驻

### Vulkan 特殊处理

- **Descriptor Pool 策略**：每帧一个 Pool，帧结束时整体 Reset
- **Pipeline Cache**：使用 VkPipelineCache 加速 PSO 创建
- **Memory Aliasing**：RenderGraph 瞬态资源使用内存别名

### DX12/Vulkan 共同点

- 显式资源屏障
- 基于描述符堆/描述符池
- 手动管理命令分配器/命令池
- 支持 Bindless (Descriptor Indexing)

## 9. 组件依赖关系

```mermaid
graph BT
    Core[Core] --> RHI[RHI Interface]
    RHI --> ShaderCompiler[ShaderCompiler]
    RHI --> RHI_DX11[RHI_DX11]
    RHI --> RHI_DX12[RHI_DX12]
    RHI --> RHI_Vulkan[RHI_Vulkan]
    ShaderCompiler --> RenderGraph[RenderGraph]
    RHI --> RenderGraph
    RenderGraph --> Samples[Samples]
```

## 10. 构建系统 (CMake + vcpkg)

### 10.1 vcpkg 配置

```json
// vcpkg.json
{
  "name": "renderversex",
  "version": "0.1.0",
  "dependencies": [
    "spdlog",
    "glfw3",
    "vulkan",
    "vulkan-memory-allocator",
    "spirv-reflect",
    "directx-dxc",
    {
      "name": "d3d12-memory-allocator",
      "platform": "windows"
    }
  ],
  "builtin-baseline": "2024.01.12"
}
```
```cmake
# vcpkg-configuration.json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "2024.01.12"
  }
}
```

### 10.2 CMake Presets

```json
// CMakePresets.json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "vcpkg-base",
      "hidden": true,
      "cacheVariables": {
        "CMAKE_TOOLCHAIN_FILE": {
          "value": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
          "type": "FILEPATH"
        }
      }
    },
    {
      "name": "windows-x64-debug",
      "displayName": "Windows x64 Debug",
      "inherits": "vcpkg-base",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "architecture": { "value": "x64", "strategy": "external" },
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "RVX_ENABLE_DX11": "ON",
        "RVX_ENABLE_DX12": "ON",
        "RVX_ENABLE_VULKAN": "ON"
      }
    },
    {
      "name": "windows-x64-release",
      "displayName": "Windows x64 Release",
      "inherits": "windows-x64-debug",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "windows-x64-debug",
      "configurePreset": "windows-x64-debug"
    },
    {
      "name": "windows-x64-release",
      "configurePreset": "windows-x64-release"
    }
  ]
}
```

### 10.3 根目录 CMakeLists.txt

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.21)
project(RenderVerseX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 构建选项
option(RVX_ENABLE_DX11 "Enable DirectX 11 backend" ON)
option(RVX_ENABLE_DX12 "Enable DirectX 12 backend" ON)
option(RVX_ENABLE_VULKAN "Enable Vulkan backend" ON)
option(RVX_BUILD_SAMPLES "Build sample applications" ON)

# 查找依赖
find_package(spdlog CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

if(RVX_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(unofficial-spirv-reflect CONFIG REQUIRED)
endif()

# Windows SDK for DX11/DX12
if(WIN32 AND (RVX_ENABLE_DX11 OR RVX_ENABLE_DX12))
    # DXC comes from vcpkg
    find_package(directx-dxc CONFIG REQUIRED)
    if(RVX_ENABLE_DX12)
        find_package(D3D12MemoryAllocator CONFIG REQUIRED)
    endif()
endif()

# 子模块
add_subdirectory(Core)
add_subdirectory(RHI)
add_subdirectory(ShaderCompiler)

if(RVX_ENABLE_DX11)
    add_subdirectory(RHI_DX11)
endif()

if(RVX_ENABLE_DX12)
    add_subdirectory(RHI_DX12)
endif()

if(RVX_ENABLE_VULKAN)
    add_subdirectory(RHI_Vulkan)
endif()

add_subdirectory(RenderGraph)

if(RVX_BUILD_SAMPLES)
    add_subdirectory(Samples)
endif()
```

### 10.4 模块 CMakeLists.txt 示例

```cmake
# Core/CMakeLists.txt
add_library(RVX_Core STATIC)

target_sources(RVX_Core PRIVATE
    Private/Memory.cpp
)

target_include_directories(RVX_Core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Core PUBLIC
    spdlog::spdlog
)
```
```cmake
# RHI/CMakeLists.txt
add_library(RVX_RHI STATIC)

target_sources(RVX_RHI PRIVATE
    Private/RHIModule.cpp
    Private/RHIValidation.cpp
)

target_include_directories(RVX_RHI PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RHI PUBLIC
    RVX_Core
)

# 根据启用的后端设置编译定义
if(RVX_ENABLE_DX11)
    target_compile_definitions(RVX_RHI PUBLIC RVX_ENABLE_DX11=1)
endif()
if(RVX_ENABLE_DX12)
    target_compile_definitions(RVX_RHI PUBLIC RVX_ENABLE_DX12=1)
endif()
if(RVX_ENABLE_VULKAN)
    target_compile_definitions(RVX_RHI PUBLIC RVX_ENABLE_VULKAN=1)
endif()
```
```cmake
# RHI_Vulkan/CMakeLists.txt
add_library(RVX_RHI_Vulkan STATIC)

target_sources(RVX_RHI_Vulkan PRIVATE
    Private/VulkanDevice.cpp
    Private/VulkanCommandContext.cpp
    Private/VulkanDescriptorPool.cpp
    Private/VulkanResources.cpp
    Private/VulkanPipeline.cpp
)

target_include_directories(RVX_RHI_Vulkan PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
)

target_link_libraries(RVX_RHI_Vulkan PUBLIC
    RVX_RHI
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
)
```
```cmake
# Samples/Triangle/CMakeLists.txt
add_executable(Triangle main.cpp)

target_link_libraries(Triangle PRIVATE
    RVX_RenderGraph
    glfw
)

# 链接所有启用的后端
if(RVX_ENABLE_DX11)
    target_link_libraries(Triangle PRIVATE RVX_RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    target_link_libraries(Triangle PRIVATE RVX_RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    target_link_libraries(Triangle PRIVATE RVX_RHI_Vulkan)
endif()
```

### 10.5 构建命令

```bash
# 首次配置 (确保 VCPKG_ROOT 环境变量已设置)
cmake --preset windows-x64-debug

# 构建
cmake --build --preset windows-x64-debug

# 构建 Release
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
```

## 11. 第三方依赖 (通过 vcpkg 管理)

| 依赖 | vcpkg 包名 | 用途 |

|------|-----------|------|

| DXC | directx-dxc | HLSL 编译到 DXIL/SPIR-V |

| SPIRV-Reflect | spirv-reflect | SPIR-V 着色器反射 |

| VMA | vulkan-memory-allocator | Vulkan 内存管理 |

| D3D12MA | d3d12-memory-allocator | DX12 内存分配器 |

| spdlog | spdlog | 日志系统 |

| GLFW | glfw3 | 跨平台窗口管理 |

| Vulkan SDK | vulkan | Vulkan API |

## 12. 执行计划与验证流程

### 12.1 执行路线图

```mermaid
gantt
    title RenderVerseX RHI 开发路线图
    dateFormat  YYYY-MM-DD
    
    section Phase1_基础设施
    项目结构与构建系统      :p1a, 2026-01-13, 1d
    Core模块实现            :p1b, after p1a, 1d
    
    section Phase2_RHI核心
    枚举与类型定义          :p2a, after p1b, 1d
    资源接口设计            :p2b, after p2a, 2d
    Pipeline接口            :p2c, after p2b, 1d
    描述符系统              :p2d, after p2c, 2d
    CommandContext          :p2e, after p2d, 2d
    Device接口              :p2f, after p2e, 1d
    Shader编译器            :p2g, after p2f, 2d
    
    section Phase3_DX12后端
    DX12_Device             :p3a, after p2g, 2d
    DX12_Resources          :p3b, after p3a, 2d
    DX12_Descriptor         :p3c, after p3b, 2d
    DX12_Command            :p3d, after p3c, 2d
    DX12_Pipeline           :p3e, after p3d, 1d
    DX12_验证测试           :p3f, after p3e, 2d
    
    section Phase4_Vulkan后端
    Vulkan_Device           :p4a, after p3f, 2d
    Vulkan_Resources        :p4b, after p4a, 2d
    Vulkan_Descriptor       :p4c, after p4b, 2d
    Vulkan_Command          :p4d, after p4c, 2d
    Vulkan_Pipeline         :p4e, after p4d, 1d
    Vulkan_验证测试         :p4f, after p4e, 2d
    
    section Phase5_DX11后端
    DX11_Device             :p5a, after p4f, 1d
    DX11_Resources          :p5b, after p5a, 2d
    DX11_Binding            :p5c, after p5b, 2d
    DX11_Command            :p5d, after p5c, 1d
    DX11_Pipeline           :p5e, after p5d, 1d
    DX11_验证测试           :p5f, after p5e, 2d
    
    section Phase6_高级功能
    RenderGraph             :p6a, after p5f, 3d
    并行执行                :p6b, after p6a, 2d
    RenderGraph验证         :p6c, after p6b, 2d
    
    section Phase7_集成验证
    跨后端一致性            :p7a, after p6c, 2d
    压力测试                :p7b, after p7a, 2d
```

### 12.2 Phase 3: DX12 验证流程

```cpp
// Tests/DX12Validation/main.cpp

class DX12ValidationSuite {
public:
    void RunAll() {
        // 1. 基础验证
        Test_DeviceCreation();
        Test_SwapChainCreation();
        
        // 2. 资源验证
        Test_BufferCreation();
        Test_TextureCreation();
        Test_BufferUpload();
        Test_TextureUpload();
        
        // 3. 描述符验证
        Test_DescriptorHeapAllocation();
        Test_RootSignatureCreation();
        Test_DescriptorSetBinding();
        
        // 4. 渲染验证
        Test_TriangleRendering();      // 核心：绘制三角形
        Test_TexturedQuad();           // 纹理采样
        Test_MultipleRenderTargets();  // MRT
        
        // 5. 生命周期验证
        Test_ResourceLifecycle();      // 延迟删除
        Test_MultiFrameResources();    // 多帧资源管理
        Test_MemoryLeakDetection();    // 内存泄漏检测
        
        // 6. Debug Layer 验证
        Test_DebugLayerEnabled();
        Test_ResourceStateValidation();
    }
    
private:
    void Test_TriangleRendering() {
        // 创建顶点缓冲
        auto vb = device->CreateBuffer({
            .size = sizeof(vertices),
            .usage = RHIBufferUsage::Vertex,
            .memoryType = RHIMemoryType::Upload
        });
        
        // 录制命令
        auto ctx = device->CreateCommandContext(RHICommandQueueType::Graphics);
        ctx->Begin();
        ctx->BeginEvent("Triangle Rendering");
        ctx->BeginRenderPass(renderPassDesc);
        ctx->SetPipeline(pipeline.Get());
        ctx->SetVertexBuffer(0, vb.Get());
        ctx->Draw(3, 1, 0, 0);
        ctx->EndRenderPass();
        ctx->EndEvent();
        ctx->End();
        
        device->SubmitCommandContext(ctx.Get());
        device->WaitIdle();
        
        // 验证：读取渲染结果并比较
        ValidateRenderOutput(swapChain->GetCurrentBackBuffer());
    }
};
```

### 12.3 Phase 4: Vulkan 验证流程

```cpp
// Tests/VulkanValidation/main.cpp

class VulkanValidationSuite {
public:
    void RunAll() {
        // 1. 基础验证
        Test_InstanceCreation();
        Test_ValidationLayerEnabled();  // VK_LAYER_KHRONOS_validation
        Test_PhysicalDeviceSelection();
        Test_LogicalDeviceCreation();
        
        // 2. 资源验证
        Test_VMAIntegration();          // VulkanMemoryAllocator
        Test_BufferCreation();
        Test_ImageCreation();
        
        // 3. 描述符验证
        Test_DescriptorPoolCreation();
        Test_DescriptorSetLayoutCreation();
        Test_DescriptorSetAllocation();
        
        // 4. 渲染验证
        Test_RenderPassCreation();
        Test_FramebufferCreation();
        Test_TriangleRendering();
        
        // 5. 同步验证
        Test_PipelineBarriers();
        Test_SubresourceBarriers();     // Mip/ArraySlice 级别
        
        // 6. GPU 调试
        Test_DebugUtilsMessenger();
        Test_ObjectNaming();            // vkSetDebugUtilsObjectName
    }
};
```

### 12.4 Phase 5: DX11 验证流程

```cpp
// Tests/DX11Validation/main.cpp

class DX11ValidationSuite {
public:
    void RunAll() {
        // 1. 基础验证
        Test_DeviceCreation();
        Test_FeatureLevelCheck();       // D3D_FEATURE_LEVEL_11_0+
        Test_DebugLayerEnabled();
        
        // 2. Slot 映射验证
        Test_SlotMappingGeneration();   // Set/Binding -> DX11 Slot
        Test_FlattenedBindingBake();    // 预计算扁平化
        Test_BindingRemapperApply();    // 运行时应用
        
        // 3. 渲染验证
        Test_TriangleRendering();
        Test_MultiSlotBinding();        // 验证多 Set 映射正确
        
        // 4. 多线程验证
        Test_DeferredContextCreation();
        Test_AdaptiveThreadingDecision();  // 验证自适应降级
        
        // 5. Bindless 回退验证
        Test_BindlessCapabilityQuery();    // 应返回 false
        Test_TraditionalBindingFallback(); // 验证回退路径正确
    }
};
```

### 12.5 验证通过标准

| Phase | 验证项 | 通过标准 |
|-------|--------|----------|
| P3.6 DX12 | Triangle 渲染 | 窗口显示正确三角形，无 Debug Layer 错误 |
| P3.6 DX12 | 资源生命周期 | 延迟删除正确触发，无内存泄漏 |
| P3.6 DX12 | 多帧测试 | 连续渲染 1000 帧无错误 |
| P4.6 Vulkan | Triangle 渲染 | 窗口显示正确三角形，无 Validation Layer 错误 |
| P4.6 Vulkan | 屏障验证 | Subresource 屏障正确插入 |
| P4.6 Vulkan | GPU 调试 | RenderDoc 可正确捕获帧 |
| P5.6 DX11 | Triangle 渲染 | 窗口显示正确三角形，无 Debug Layer 错误 |
| P5.6 DX11 | Slot 映射 | 多 Space 绑定正确映射到 DX11 Slot |
| P5.6 DX11 | 多线程降级 | 低 DrawCall 场景正确降级为单线程 |
| P7.1 跨后端 | 一致性 | 同一场景在三个后端渲染结果像素级一致 |
| P7.2 压力测试 | 性能 | 10000 DrawCall 帧率 > 60 FPS |

### 12.6 测试目录结构

```
Tests/
├── CMakeLists.txt
├── TestFramework/
│   ├── TestRunner.h
│   ├── Assertions.h
│   └── ImageCompare.h          # 像素比较工具
│
├── DX12Validation/
│   ├── CMakeLists.txt
│   └── main.cpp
│
├── VulkanValidation/
│   ├── CMakeLists.txt
│   └── main.cpp
│
├── DX11Validation/
│   ├── CMakeLists.txt
│   └── main.cpp
│
├── RenderGraphValidation/
│   ├── CMakeLists.txt
│   └── main.cpp
│
├── CrossBackendValidation/
│   ├── CMakeLists.txt
│   └── main.cpp
│
└── Shaders/
    ├── Triangle.hlsl
    ├── TexturedQuad.hlsl
    └── Compute.hlsl
```