---
name: RHI Layer Design Review
overview: 基于行业最佳实践对RHI层进行全面完善，采用Capabilities驱动+分层实现的策略。
todos:
  - id: phase1-fix-dead-code
    content: "Phase 1.1: 修复 SelectBestBackend() 死代码"
    status: completed
  - id: phase1-dynamic-state
    content: "Phase 1.2: 添加完整动态渲染状态接口 (6个方法，所有后端)"
    status: completed
  - id: phase1-cleanup-caps
    content: "Phase 1.3: 清理Capabilities冗余字段"
    status: completed
  - id: phase2-extend-caps
    content: "Phase 2.1: 扩展RHICapabilities特性标志"
    status: completed
  - id: phase2-staging-buffer
    content: "Phase 2.2: 添加高效资源上传机制 (StagingBuffer)"
    status: completed
  - id: phase2-ring-buffer
    content: "Phase 2.3: 添加帧临时数据分配器 (RingBuffer)"
    status: completed
  - id: phase3-memory-stats
    content: "Phase 3.1: 添加GPU内存统计接口"
    status: completed
  - id: phase3-split-barrier
    content: "Phase 3.2: 添加Split Barrier (DX12/Vulkan)"
    status: completed
  - id: phase3-debug-groups
    content: "Phase 3.3: 添加资源调试分组"
    status: completed
  - id: phase4-deferred
    content: "Phase 4: 按需添加 (Secondary Context/Bindless/RT/MeshShader)"
    status: completed
---

# RHI层设计审查与改进计划 (完善版)

## 设计策略

### 核心原则

参考 Unreal Engine、Unity、bgfx、The Forge、Diligent Engine 的设计实践：

1. **Capabilities驱动**: 通过能力查询让上层感知后端差异
2. **核心接口完整**: 常用功能所有后端都必须实现
3. **优雅降级**: 不支持的特性提供空实现，不影响编译
4. **按需扩展**: 特别不常用的高级特性（RT/MeshShader）后续再添加

### 架构概览

```
IRHIDevice (核心接口 - 所有后端必须实现)
├── 资源创建 (Buffer, Texture, Pipeline, Sampler...)
├── 资源上传 (StagingBuffer, RingBuffer) ← 新增
├── 命令录制 (CommandContext + 动态状态)
├── 同步原语 (Fence, Barrier, SplitBarrier)
├── 调试支持 (DebugName, DebugGroup, MemoryStats) ← 新增
└── 能力查询 (GetCapabilities)

后续按需扩展 (暂不实现)
├── Secondary Command Buffer
├── Bindless资源表
├── Raytracing
└── Mesh Shader
```

---

## Phase 1: 基础完善 (所有后端)

### 1.1 修复死代码

**文件**: `RHI/Include/RHI/RHIDefinitions.h` (行 40-50)

```cpp
// 当前代码 - 存在死代码
inline RHIBackendType SelectBestBackend()
{
#if defined(_WIN32)
    return RHIBackendType::Vulkan;   // ← 永远执行
    return RHIBackendType::DX12;     // ← 永远不执行 (死代码)
#elif defined(__APPLE__)
    return RHIBackendType::Metal;
#else
    return RHIBackendType::Vulkan;
#endif
}
```

**修改为**:

```cpp
inline RHIBackendType SelectBestBackend()
{
#if defined(_WIN32)
    return RHIBackendType::DX12;  // Windows优先DX12
#elif defined(__APPLE__)
    return RHIBackendType::Metal;
#else
    return RHIBackendType::Vulkan;
#endif
}
```

### 1.2 添加完整动态渲染状态

**文件**: `RHI/Include/RHI/RHICommandContext.h`

在 `RHICommandContext` 类中添加：

```cpp
// =========================================================================
// Dynamic Render State (所有后端必须实现)
// =========================================================================

/**
 * @brief 设置Stencil参考值
 * @param reference 参考值 (0-255)
 */
virtual void SetStencilReference(uint32 reference) = 0;

/**
 * @brief 设置混合常量颜色
 * @param constants RGBA常量值 [0.0, 1.0]
 */
virtual void SetBlendConstants(const float constants[4]) = 0;

/**
 * @brief 设置深度偏移 (用于阴影/贴花防止Z-fighting)
 * @param constantFactor 常量偏移
 * @param slopeFactor 斜率偏移
 * @param clamp 偏移限制值 (0表示不限制)
 */
virtual void SetDepthBias(float constantFactor, float slopeFactor, float clamp = 0.0f) = 0;

/**
 * @brief 设置深度边界测试范围 (需检查 caps.supportsDepthBounds)
 * @param minDepth 最小深度 [0.0, 1.0]
 * @param maxDepth 最大深度 [0.0, 1.0]
 */
virtual void SetDepthBounds(float minDepth, float maxDepth) = 0;

/**
 * @brief 设置前后Stencil参考值 (分离)
 * @param frontRef 正面参考值
 * @param backRef 背面参考值
 */
virtual void SetStencilReferenceSeparate(uint32 frontRef, uint32 backRef) = 0;

/**
 * @brief 设置线宽 (OpenGL/Vulkan, 其他后端忽略)
 * @param width 线宽像素
 */
virtual void SetLineWidth(float width) = 0;
```

**后端实现映射**:

| 方法 | DX11 | DX12 | Vulkan | OpenGL | Metal |

|------|------|------|--------|--------|-------|

| SetStencilReference | OMSetStencilRef | OMSetStencilRef | vkCmdSetStencilReference | glStencilFunc | setStencilReferenceValue |

| SetBlendConstants | OMSetBlendFactor | OMSetBlendFactor | vkCmdSetBlendConstants | glBlendColor | setBlendColor |

| SetDepthBias | RSSetState(动态创建) | OMSetDepthBias | vkCmdSetDepthBias | glPolygonOffset | setDepthBias |

| SetDepthBounds | 空实现 | OMSetDepthBounds | vkCmdSetDepthBounds | 空实现 | 空实现 |

| SetStencilReferenceSeparate | OMSetStencilRef(合并) | 同上 | vkCmdSetStencilReference | glStencilFuncSeparate | setStencilFrontReferenceValue |

| SetLineWidth | 空实现(always 1.0) | 空实现 | vkCmdSetLineWidth | glLineWidth | 空实现 |

### 1.3 清理Capabilities冗余字段

**文件**: `RHI/Include/RHI/RHICapabilities.h`

```cpp
// 删除冗余字段 (保留数组版本即可)
// uint32 maxComputeWorkGroupSizeX = 1024;  // 删除
// uint32 maxComputeWorkGroupSizeY = 1024;  // 删除  
// uint32 maxComputeWorkGroupSizeZ = 64;    // 删除

// 保留
uint32 maxComputeWorkGroupSize[3] = {1024, 1024, 64};
```

---

## Phase 2: 资源上传与Capabilities扩展

### 2.1 扩展RHICapabilities

**文件**: `RHI/Include/RHI/RHICapabilities.h`

```cpp
struct RHICapabilities
{
    // === 现有字段保留 ===
    // ...

    // === 新增特性标志 ===
    
    // 动态状态支持
    bool supportsDepthBounds = false;           // DX12/Vulkan
    bool supportsDynamicLineWidth = false;      // Vulkan/OpenGL
    bool supportsSeparateStencilRef = false;    // 所有现代API
    
    // 高级渲染特性
    bool supportsSplitBarrier = false;          // DX12/Vulkan
    bool supportsSecondaryCommandBuffer = false;// DX12/Vulkan/Metal
    
    // 内存特性
    bool supportsMemoryBudgetQuery = false;     // DX12(DXGI)/Vulkan(VK_EXT_memory_budget)
    bool supportsPersistentMapping = false;     // Vulkan/DX12/OpenGL4.4+
};
```

### 2.2 高效资源上传机制

**新文件**: `RHI/Include/RHI/RHIUpload.h`

```cpp
#pragma once
#include "RHI/RHIResources.h"

namespace RVX
{
    // =========================================================================
    // Staging Buffer - 用于CPU->GPU数据传输
    // =========================================================================
    struct RHIStagingBufferDesc
    {
        uint64 size = 0;
        const char* debugName = nullptr;
    };

    class RHIStagingBuffer : public RHIResource
    {
    public:
        virtual ~RHIStagingBuffer() = default;

        /**
         * @brief 映射缓冲区以写入数据
         * @param offset 起始偏移
         * @param size 映射大小 (RVX_WHOLE_SIZE = 全部)
         * @return 映射指针，失败返回nullptr
         */
        virtual void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) = 0;
        
        /**
         * @brief 取消映射并刷新到GPU可见
         */
        virtual void Unmap() = 0;

        /**
         * @brief 获取缓冲区大小
         */
        virtual uint64 GetSize() const = 0;
    };

    using RHIStagingBufferRef = Ref<RHIStagingBuffer>;

    // =========================================================================
    // Ring Buffer - 用于每帧临时数据 (Constant Buffer等)
    // =========================================================================
    struct RHIRingBufferDesc
    {
        uint64 size = 4 * 1024 * 1024;  // 默认4MB
        uint32 alignment = 256;          // 对齐要求 (Constant Buffer通常256)
        const char* debugName = nullptr;
    };

    struct RHIRingAllocation
    {
        void* cpuAddress = nullptr;      // CPU可写地址
        uint64 gpuOffset = 0;            // GPU缓冲区内偏移
        uint64 size = 0;                 // 分配大小
        RHIBuffer* buffer = nullptr;     // 底层缓冲区 (用于绑定)
        
        bool IsValid() const { return cpuAddress != nullptr; }
    };

    class RHIRingBuffer : public RHIResource
    {
    public:
        virtual ~RHIRingBuffer() = default;

        /**
         * @brief 分配临时内存
         * @param size 请求大小
         * @return 分配结果，失败时IsValid()返回false
         */
        virtual RHIRingAllocation Allocate(uint64 size) = 0;

        /**
         * @brief 重置分配器 (每帧结束时调用)
         * @param frameIndex 当前帧索引
         */
        virtual void Reset(uint32 frameIndex) = 0;

        /**
         * @brief 获取底层缓冲区 (用于绑定)
         */
        virtual RHIBuffer* GetBuffer() const = 0;
    };

    using RHIRingBufferRef = Ref<RHIRingBuffer>;
}
```

**IRHIDevice新增方法**:

```cpp
// 在 IRHIDevice 中添加
virtual RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc& desc) = 0;
virtual RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc& desc) = 0;
```

**使用示例**:

```cpp
// 上传纹理数据
auto staging = device->CreateStagingBuffer({textureDataSize, "TextureUpload"});
void* mapped = staging->Map();
memcpy(mapped, textureData, textureDataSize);
staging->Unmap();
cmdContext->CopyBufferToTexture(staging->GetBuffer(), texture, copyDesc);

// 每帧动态数据
auto ring = device->CreateRingBuffer({4*1024*1024, 256, "FrameConstants"});
// 在帧循环中
auto alloc = ring->Allocate(sizeof(PerFrameConstants));
memcpy(alloc.cpuAddress, &frameConstants, sizeof(PerFrameConstants));
cmdContext->SetDescriptorSet(0, descriptorSet); // 使用 alloc.gpuOffset
// 帧结束
ring->Reset(frameIndex);
```

---

## Phase 3: 调试与性能工具

### 3.1 GPU内存统计

**新增到**: `RHI/Include/RHI/RHIDevice.h`

```cpp
// =========================================================================
// Memory Statistics
// =========================================================================
struct RHIMemoryStats
{
    // 总体统计
    uint64 totalAllocated = 0;       // 总分配量 (字节)
    uint64 totalUsed = 0;            // 实际使用量
    uint64 peakUsage = 0;            // 峰值使用量
    uint32 allocationCount = 0;      // 分配次数
    
    // 按类型统计
    uint64 bufferMemory = 0;         // Buffer占用
    uint64 textureMemory = 0;        // Texture占用
    uint64 renderTargetMemory = 0;   // RT/DS占用
    
    // 预算信息 (需要 supportsMemoryBudgetQuery)
    uint64 budgetBytes = 0;          // GPU内存预算
    uint64 currentUsageBytes = 0;    // 当前使用量
};

// IRHIDevice 添加
virtual RHIMemoryStats GetMemoryStats() const = 0;
```

### 3.2 Split Barrier (DX12/Vulkan)

**文件**: `RHI/Include/RHI/RHICommandContext.h`

```cpp
// =========================================================================
// Split Barriers (用于隐藏同步延迟)
// =========================================================================

/**
 * @brief 开始资源状态转换 (异步)
 * 
 * 调用后资源处于中间状态，必须在使用前调用EndBarrier完成转换。
 * 不支持的后端会忽略此调用，直接在EndBarrier执行完整转换。
 */
virtual void BeginBarrier(const RHITextureBarrier& barrier) = 0;
virtual void BeginBarrier(const RHIBufferBarrier& barrier) = 0;

/**
 * @brief 完成资源状态转换
 * 
 * 如果之前调用过BeginBarrier，完成转换；否则执行完整转换。
 */
virtual void EndBarrier(const RHITextureBarrier& barrier) = 0;
virtual void EndBarrier(const RHIBufferBarrier& barrier) = 0;
```

**后端实现**:

| 后端 | 实现方式 |

|------|----------|

| DX12 | D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY / END_ONLY |

| Vulkan | 使用VkDependencyInfo分离srcStageMask和dstStageMask |

| DX11/OpenGL/Metal | 忽略Begin，End执行完整barrier |

### 3.3 资源调试分组

**文件**: `RHI/Include/RHI/RHIDevice.h`

```cpp
// =========================================================================
// Debug Resource Groups (用于RenderDoc/PIX资源分组)
// =========================================================================

/**
 * @brief 开始资源创建分组
 * 
 * 在此分组内创建的资源会在调试工具中归类显示。
 * 
 * @param name 分组名称
 */
virtual void BeginResourceGroup(const char* name) = 0;

/**
 * @brief 结束资源创建分组
 */
virtual void EndResourceGroup() = 0;
```

**使用示例**:

```cpp
device->BeginResourceGroup("ShadowMap Resources");
auto shadowDepth = device->CreateTexture(shadowDepthDesc);
auto shadowSampler = device->CreateSampler(shadowSamplerDesc);
device->EndResourceGroup();
```

---

## Phase 4: 按需扩展 (暂不实现)

以下特性较为专业或不常用，待实际需要时再添加：

### 4.1 Secondary Command Buffer

```cpp
// 仅DX12/Vulkan/Metal支持
virtual RHICommandContextRef CreateSecondaryContext() = 0;
virtual void ExecuteSecondaryContexts(std::span<RHICommandContext* const> contexts) = 0;
```

**推迟原因**: RenderGraph层已解决大部分并行需求。

### 4.2 Bindless资源

```cpp
class RHIBindlessTable : public RHIResource
{
    virtual uint32 RegisterTexture(RHITextureView* view) = 0;
    virtual uint32 RegisterBuffer(RHIBuffer* buffer) = 0;
    virtual void Unregister(uint32 index) = 0;
};
```

**推迟原因**: 需要Shader配合，等材质系统完善后再添加。

### 4.3 Raytracing

```cpp
class RHIAccelerationStructure : public RHIResource { ... };
virtual RHIAccelerationStructureRef CreateBLAS(const RHIBLASDesc& desc) = 0;
virtual RHIAccelerationStructureRef CreateTLAS(const RHITLASDesc& desc) = 0;
```

**推迟原因**: 硬件覆盖率有限，需求不紧急。

### 4.4 Mesh Shader

```cpp
struct RHIMeshPipelineDesc
{
    RHIShader* amplificationShader = nullptr;
    RHIShader* meshShader = nullptr;
    RHIShader* pixelShader = nullptr;
};
```

**推迟原因**: 需要特殊的Shader编译流程支持。

---

## 各后端实现矩阵

| 特性 | DX11 | DX12 | Vulkan | OpenGL | Metal |

|------|------|------|--------|--------|-------|

| **Phase 1** |

| 动态状态 (6个方法) | 全部实现 | 全部实现 | 全部实现 | 全部实现 | 全部实现 |

| DepthBounds | 空实现 | 实现 | 实现 | 空实现 | 空实现 |

| LineWidth | 空实现 | 空实现 | 实现 | 实现 | 空实现 |

| **Phase 2** |

| StagingBuffer | 实现 | 实现 | 实现 | 实现 | 实现 |

| RingBuffer | 实现 | 实现 | 实现 | 实现 | 实现 |

| Caps扩展 | 填充标志 | 填充标志 | 填充标志 | 填充标志 | 填充标志 |

| **Phase 3** |

| MemoryStats | 基础 | 完整 | 完整 | 基础 | 完整 |

| Split Barrier | 空实现 | 实现 | 实现 | 空实现 | 空实现 |

| ResourceGroup | 实现 | 实现 | 实现 | 实现 | 实现 |

| **Phase 4 (按需)** |

| Secondary Context | N/A | 实现 | 实现 | N/A | 实现 |

| Bindless | N/A | 实现 | 实现 | 部分 | 实现 |

| Raytracing | N/A | 实现 | 实现 | N/A | N/A |

| Mesh Shader | N/A | 实现 | 实现 | N/A | 实现 |

---

## 实施路线图

```
Phase 1 (2-3天)              Phase 2 (3-5天)              Phase 3 (3-5天)
┌─────────────────┐      ┌─────────────────┐       ┌─────────────────┐
│ ✓ 死代码修复     │      │ ✓ Caps扩展       │       │ ✓ 内存统计       │
│ ✓ 动态状态(6个)  │ ──→  │ ✓ StagingBuffer │  ──→  │ ✓ Split Barrier │
│ ✓ Caps清理      │      │ ✓ RingBuffer    │       │ ✓ 资源调试分组   │
└─────────────────┘      └─────────────────┘       └─────────────────┘
   所有后端                   所有后端                  DX12/Vulkan完整
                                                      其他后端基础实现

                                Phase 4 (按需)
                          ┌─────────────────┐
                          │ Secondary Ctx   │
                     ──→  │ Bindless        │
                          │ Raytracing      │
                          │ Mesh Shader     │
                          └─────────────────┘
                            实际用到时添加
```

---

## 文件变更清单

### 修改文件

| 文件 | 变更内容 |

|------|----------|

| `RHI/Include/RHI/RHIDefinitions.h` | 修复死代码 |

| `RHI/Include/RHI/RHICommandContext.h` | 添加6个动态状态方法 + Split Barrier |

| `RHI/Include/RHI/RHICapabilities.h` | 删除冗余字段 + 添加新特性标志 |

| `RHI/Include/RHI/RHIDevice.h` | 添加 CreateStagingBuffer/RingBuffer + MemoryStats + ResourceGroup |

| `RHI_DX11/Private/*.cpp` | 实现新接口 |

| `RHI_DX12/Private/*.cpp` | 实现新接口 |

| `RHI_Vulkan/Private/*.cpp` | 实现新接口 |

| `RHI_OpenGL/Private/*.cpp` | 实现新接口 |

| `RHI_Metal/Private/*.mm` | 实现新接口 |

### 新增文件

| 文件 | 内容 |

|------|------|

| `RHI/Include/RHI/RHIUpload.h` | StagingBuffer + RingBuffer 定义 |

| 各后端的 Upload 实现文件 | StagingBuffer/RingBuffer 后端实现 |

---

## 设计决策记录

### 为什么采用这种分层策略？

1. **Phase 1-3 是常用功能**: 动态状态、资源上传、内存统计在日常开发中频繁使用
2. **Phase 4 是专业功能**: Raytracing/MeshShader覆盖率低，按需添加更务实
3. **避免过度设计**: 不引入复杂的QueryInterface机制，保持API简洁
4. **空实现兜底**: 不支持的特性通过空实现保证编译通过，运行时按Caps分支

### 为什么不用QueryInterface模式？

1. **运行时开销**: 每次调用需要类型检查
2. **生命周期复杂**: 扩展接口的所有权不清晰
3. **过度抽象**: 对于当前规模的项目，简单的Caps检查更直观
4. **编译期条件足够**: `#if RVX_ENABLE_RAYTRACING` 更简单可靠