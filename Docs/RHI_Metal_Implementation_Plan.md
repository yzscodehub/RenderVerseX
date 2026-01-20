# RHI Metal 后端实现规划

> **文档版本**: 1.0  
> **创建日期**: 2026-01-16  
> **目标**: 为 RenderVerseX 引擎添加 Apple Metal 图形 API 后端支持

---

## 目录

1. [当前 RHI 层架构分析](#1-当前-rhi-层架构分析)
2. [Metal 后端兼容性评估](#2-metal-后端兼容性评估)
3. [需要修改的现有代码](#3-需要修改的现有代码)
4. [Metal 模块设计](#4-metal-模块设计)
5. [API 映射参考](#5-api-映射参考)
6. [Shader 编译策略](#6-shader-编译策略)
7. [实施路线图](#7-实施路线图)
8. [平台支持矩阵](#8-平台支持矩阵)

---

## 1. 当前 RHI 层架构分析

### 1.1 模块结构

```
RenderVerseX/
├── RHI/                    # 抽象接口层
│   ├── Include/RHI/
│   │   ├── RHI.h           # 统一头文件
│   │   ├── RHIDevice.h     # 设备接口 (IRHIDevice)
│   │   ├── RHIBuffer.h     # 缓冲区接口
│   │   ├── RHITexture.h    # 纹理接口
│   │   ├── RHIPipeline.h   # 管线状态接口
│   │   ├── RHICommandContext.h  # 命令上下文
│   │   ├── RHIDescriptor.h # 描述符绑定
│   │   ├── RHISwapChain.h  # 交换链
│   │   ├── RHIHeap.h       # 内存堆 (Placed Resources)
│   │   └── ...
│   └── Private/
│       └── RHIModule.cpp   # 后端工厂函数
│
├── RHI_DX11/               # DirectX 11 后端
├── RHI_DX12/               # DirectX 12 后端
├── RHI_Vulkan/             # Vulkan 后端
└── RHI_Metal/              # [待实现] Metal 后端
```

### 1.2 核心接口设计

#### IRHIDevice 接口

```cpp
class IRHIDevice
{
public:
    virtual ~IRHIDevice() = default;

    // 资源创建
    virtual RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) = 0;
    virtual RHITextureRef CreateTexture(const RHITextureDesc& desc) = 0;
    virtual RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc = {}) = 0;
    virtual RHISamplerRef CreateSampler(const RHISamplerDesc& desc) = 0;
    virtual RHIShaderRef CreateShader(const RHIShaderDesc& desc) = 0;

    // 内存堆管理 (Memory Aliasing)
    virtual RHIHeapRef CreateHeap(const RHIHeapDesc& desc) = 0;
    virtual RHITextureRef CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc) = 0;
    virtual RHIBufferRef CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc) = 0;
    virtual MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc& desc) = 0;
    virtual MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) = 0;

    // 管线创建
    virtual RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) = 0;
    virtual RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) = 0;
    virtual RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) = 0;
    virtual RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) = 0;

    // 描述符集
    virtual RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) = 0;

    // 命令上下文
    virtual RHICommandContextRef CreateCommandContext(RHICommandQueueType type) = 0;
    virtual void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence = nullptr) = 0;
    virtual void SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence = nullptr) = 0;

    // 交换链
    virtual RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) = 0;

    // 同步
    virtual RHIFenceRef CreateFence(uint64 initialValue = 0) = 0;
    virtual void WaitForFence(RHIFence* fence, uint64 value) = 0;
    virtual void WaitIdle() = 0;

    // 帧管理
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual uint32 GetCurrentFrameIndex() const = 0;

    // 能力查询
    virtual const RHICapabilities& GetCapabilities() const = 0;
    virtual RHIBackendType GetBackendType() const = 0;
};
```

### 1.3 后端工厂机制

当前工厂函数实现 (`RHI/Private/RHIModule.cpp`):

```cpp
#if RVX_ENABLE_DX12
namespace RVX { std::unique_ptr<IRHIDevice> CreateDX12Device(const RHIDeviceDesc& desc); }
#endif

#if RVX_ENABLE_VULKAN
namespace RVX { std::unique_ptr<IRHIDevice> CreateVulkanDevice(const RHIDeviceDesc& desc); }
#endif

#if RVX_ENABLE_DX11
namespace RVX { std::unique_ptr<IRHIDevice> CreateDX11Device(const RHIDeviceDesc& desc); }
#endif

std::unique_ptr<IRHIDevice> CreateRHIDevice(RHIBackendType backend, const RHIDeviceDesc& desc)
{
    switch (backend)
    {
        case RHIBackendType::DX12:   return CreateDX12Device(desc);
        case RHIBackendType::Vulkan: return CreateVulkanDevice(desc);
        case RHIBackendType::DX11:   return CreateDX11Device(desc);
        default: return nullptr;
    }
}
```

---

## 2. Metal 后端兼容性评估

### 2.1 设计优势 ✅

| 特性 | 评估 | 说明 |
|------|------|------|
| 接口抽象程度 | ⭐⭐⭐⭐⭐ | 接口与 Metal API 风格高度一致 |
| 资源模型 | ⭐⭐⭐⭐⭐ | Buffer/Texture/Sampler 1:1 映射 |
| 内存堆支持 | ⭐⭐⭐⭐⭐ | `RHIHeap` 完美匹配 `MTLHeap` |
| 命令模型 | ⭐⭐⭐⭐⭐ | CommandContext → CommandBuffer + Encoder |
| 管线状态 | ⭐⭐⭐⭐⭐ | PSO 设计与 Metal 一致 |
| 条件编译 | ⭐⭐⭐⭐⭐ | CMake option 机制完善 |

### 2.2 需注意的差异 ⚠️

| 差异点 | RHI 当前设计 | Metal 实现方式 | 影响级别 |
|--------|-------------|---------------|---------|
| Geometry Shader | 支持 | 不支持 | 🔴 高 - 需文档说明 |
| Tessellation | Hull/Domain | Tessellation Stage | 🟡 中 - 需映射 |
| Descriptor Set | 类似 Vulkan | Argument Buffer 或直接绑定 | 🟡 中 - 可选实现 |
| SwapChain | HWND | CAMetalLayer / NSWindow | 🟡 中 - 平台适配 |
| Shader 二进制 | DXBC/DXIL/SPIR-V | MTLLibrary | 🔴 高 - 需编译策略 |
| Push Constants | 支持 | setBytes() | 🟢 低 - 直接映射 |

### 2.3 Geometry Shader 限制

Metal 不支持 Geometry Shader。对于依赖 GS 的功能：

| 传统 GS 用例 | Metal 替代方案 |
|-------------|---------------|
| 点精灵扩展 | Vertex Amplification / Compute |
| 层渲染 | Render to Texture Array + Layered Rendering |
| 流输出 | Compute Shader + Buffer 写入 |
| Billboard | Vertex Shader 实例化 |

**建议**: 在 `RHICapabilities` 中添加 `supportsGeometryShader` 标志。

---

## 3. 需要修改的现有代码

### 3.1 RHI/Include/RHI/RHIDefinitions.h

```cpp
// 添加 Metal 后端类型
enum class RHIBackendType : uint8
{
    None = 0,
    DX11,
    DX12,
    Vulkan,
    Metal    // 新增
};

inline const char* ToString(RHIBackendType type)
{
    switch (type)
    {
        case RHIBackendType::DX11:   return "DirectX 11";
        case RHIBackendType::DX12:   return "DirectX 12";
        case RHIBackendType::Vulkan: return "Vulkan";
        case RHIBackendType::Metal:  return "Metal";  // 新增
        default:                     return "Unknown";
    }
}
```

### 3.2 RHI/Include/RHI/RHICapabilities.h

```cpp
struct RHICapabilities
{
    // 现有字段...
    
    // 新增 Metal 相关能力
    bool supportsGeometryShader = true;      // Metal = false
    bool supportsTessellation = true;
    bool supportsArgumentBuffers = false;    // Metal 特有
    bool supportsMetalHeaps = false;         // Metal 特有
};
```

### 3.3 RHI/Private/RHIModule.cpp

```cpp
#if RVX_ENABLE_METAL
namespace RVX { std::unique_ptr<IRHIDevice> CreateMetalDevice(const RHIDeviceDesc& desc); }
#endif

std::unique_ptr<IRHIDevice> CreateRHIDevice(RHIBackendType backend, const RHIDeviceDesc& desc)
{
    switch (backend)
    {
        // 现有 cases...
        
#if RVX_ENABLE_METAL
        case RHIBackendType::Metal:
            return CreateMetalDevice(desc);
#endif
        
        default:
            RVX_RHI_ERROR("Unsupported backend: {}", ToString(backend));
            return nullptr;
    }
}
```

### 3.4 RHI/CMakeLists.txt

```cmake
# 添加 Metal 编译定义
if(RVX_ENABLE_METAL)
    target_compile_definitions(RVX_RHI PUBLIC RVX_ENABLE_METAL=1)
endif()
```

### 3.5 CMakeLists.txt (根目录)

```cmake
# Build Options - 添加 Metal
option(RVX_ENABLE_METAL "Enable Metal backend (macOS/iOS)" OFF)

# Platform Detection - 添加 Apple 平台
if(APPLE)
    set(RVX_PLATFORM_APPLE ON)
    add_compile_definitions(RVX_PLATFORM_APPLE=1)
    
    if(IOS)
        set(RVX_PLATFORM_IOS ON)
        add_compile_definitions(RVX_PLATFORM_IOS=1)
    else()
        set(RVX_PLATFORM_MACOS ON)
        add_compile_definitions(RVX_PLATFORM_MACOS=1)
    endif()
    
    # Apple 平台禁用 DX 后端
    set(RVX_ENABLE_DX11 OFF)
    set(RVX_ENABLE_DX12 OFF)
    
    # 默认启用 Metal
    set(RVX_ENABLE_METAL ON)
endif()

# 添加 Metal 子目录
if(RVX_ENABLE_METAL)
    add_subdirectory(RHI_Metal)
endif()

# Status Summary - 添加 Metal
message(STATUS "  Metal:        ${RVX_ENABLE_METAL}")
```

### 3.6 RHI/Include/RHI/RHISwapChain.h

```cpp
struct RHISwapChainDesc
{
    void* windowHandle = nullptr;  // HWND (Windows) / NSWindow* (macOS) / UIWindow* (iOS)
    void* metalLayer = nullptr;    // 可选: 直接传入 CAMetalLayer* (Metal 专用)
    uint32 width = 0;
    uint32 height = 0;
    RHIFormat format = RHIFormat::BGRA8_UNORM;
    uint32 bufferCount = 3;
    bool vsync = true;
    const char* debugName = nullptr;
};
```

---

## 4. Metal 模块设计

### 4.1 目录结构

```
RHI_Metal/
├── CMakeLists.txt
├── Include/
│   └── Metal/
│       └── MetalDevice.h          # 公开接口 (可选导出)
└── Private/
    ├── MetalCommon.h              # 通用定义、ARC 桥接、类型转换
    ├── MetalConversions.h         # RHI → Metal 枚举转换
    ├── MetalDevice.h              # MetalDevice 类定义
    ├── MetalDevice.mm             # Device 实现
    ├── MetalCommandContext.h      # CommandBuffer + Encoder 封装
    ├── MetalCommandContext.mm
    ├── MetalResources.h           # Buffer, Texture, TextureView, Sampler
    ├── MetalResources.mm
    ├── MetalShader.h              # MTLLibrary + MTLFunction 封装
    ├── MetalShader.mm
    ├── MetalPipeline.h            # RenderPipelineState, ComputePipelineState
    ├── MetalPipeline.mm
    ├── MetalDescriptor.h          # DescriptorSetLayout, DescriptorSet (Argument Buffer)
    ├── MetalDescriptor.mm
    ├── MetalSwapChain.h           # CAMetalLayer 封装
    ├── MetalSwapChain.mm
    ├── MetalHeap.h                # MTLHeap 封装
    ├── MetalHeap.mm
    ├── MetalFence.h               # MTLFence / MTLEvent 封装
    └── MetalFence.mm
```

### 4.2 CMakeLists.txt

```cmake
# =============================================================================
# RHI_Metal Module - Apple Metal backend implementation
# =============================================================================
add_library(RVX_RHI_Metal STATIC)

target_sources(RVX_RHI_Metal PRIVATE
    Private/MetalDevice.mm
    Private/MetalCommandContext.mm
    Private/MetalResources.mm
    Private/MetalShader.mm
    Private/MetalPipeline.mm
    Private/MetalDescriptor.mm
    Private/MetalSwapChain.mm
    Private/MetalHeap.mm
    Private/MetalFence.mm
)

target_include_directories(RVX_RHI_Metal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RHI_Metal PUBLIC
    RVX::RHI
    RVX::ShaderCompiler
)

# Link Metal frameworks
target_link_libraries(RVX_RHI_Metal PRIVATE
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework Foundation"
)

if(RVX_PLATFORM_MACOS)
    target_link_libraries(RVX_RHI_Metal PRIVATE
        "-framework Cocoa"
    )
elseif(RVX_PLATFORM_IOS)
    target_link_libraries(RVX_RHI_Metal PRIVATE
        "-framework UIKit"
    )
endif()

# Enable ARC for Objective-C++ files
set_source_files_properties(
    Private/MetalDevice.mm
    Private/MetalCommandContext.mm
    Private/MetalResources.mm
    Private/MetalShader.mm
    Private/MetalPipeline.mm
    Private/MetalDescriptor.mm
    Private/MetalSwapChain.mm
    Private/MetalHeap.mm
    Private/MetalFence.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc"
)

add_library(RVX::RHI_Metal ALIAS RVX_RHI_Metal)
```

### 4.3 核心类设计

#### MetalDevice

```objc
// MetalDevice.h
#pragma once

#include "MetalCommon.h"
#include "RHI/RHIDevice.h"
#import <Metal/Metal.h>

namespace RVX
{
    class MetalDevice : public IRHIDevice
    {
    public:
        MetalDevice(const RHIDeviceDesc& desc);
        ~MetalDevice() override;

        // IRHIDevice 接口实现
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override;
        RHITextureRef CreateTexture(const RHITextureDesc& desc) override;
        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc) override;
        RHISamplerRef CreateSampler(const RHISamplerDesc& desc) override;
        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override;

        RHIHeapRef CreateHeap(const RHIHeapDesc& desc) override;
        RHITextureRef CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc) override;
        RHIBufferRef CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc) override;
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc& desc) override;
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) override;

        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) override;
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override;
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override;
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) override;

        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override;

        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override;
        void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override;
        void SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence) override;

        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) override;

        RHIFenceRef CreateFence(uint64 initialValue) override;
        void WaitForFence(RHIFence* fence, uint64 value) override;
        void WaitIdle() override;

        void BeginFrame() override;
        void EndFrame() override;
        uint32 GetCurrentFrameIndex() const override { return m_currentFrameIndex; }

        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::Metal; }

        // Metal 特有访问器
        id<MTLDevice> GetMTLDevice() const { return m_device; }
        id<MTLCommandQueue> GetCommandQueue() const { return m_commandQueue; }

    private:
        void QueryCapabilities();

        id<MTLDevice> m_device = nil;
        id<MTLCommandQueue> m_commandQueue = nil;
        
        RHICapabilities m_capabilities;
        uint32 m_currentFrameIndex = 0;
    };

} // namespace RVX
```

#### MetalBuffer

```objc
// MetalResources.h (部分)
class MetalBuffer : public RHIBuffer
{
public:
    MetalBuffer(id<MTLDevice> device, const RHIBufferDesc& desc);
    MetalBuffer(id<MTLBuffer> buffer, const RHIBufferDesc& desc); // For placed buffers
    ~MetalBuffer() override;

    uint64 GetSize() const override { return m_size; }
    RHIBufferUsage GetUsage() const override { return m_usage; }
    RHIMemoryType GetMemoryType() const override { return m_memoryType; }
    uint32 GetStride() const override { return m_stride; }

    void* Map() override;
    void Unmap() override;

    id<MTLBuffer> GetMTLBuffer() const { return m_buffer; }

private:
    id<MTLBuffer> m_buffer = nil;
    uint64 m_size = 0;
    RHIBufferUsage m_usage = RHIBufferUsage::None;
    RHIMemoryType m_memoryType = RHIMemoryType::Default;
    uint32 m_stride = 0;
};
```

#### MetalCommandContext

```objc
// MetalCommandContext.h
class MetalCommandContext : public RHICommandContext
{
public:
    MetalCommandContext(id<MTLCommandQueue> queue, RHICommandQueueType type);
    ~MetalCommandContext() override;

    void Begin() override;
    void End() override;
    void Reset() override;

    void BeginEvent(const char* name, uint32 color) override;
    void EndEvent() override;
    void SetMarker(const char* name, uint32 color) override;

    void BufferBarrier(const RHIBufferBarrier& barrier) override;
    void TextureBarrier(const RHITextureBarrier& barrier) override;
    void Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                  std::span<const RHITextureBarrier> textureBarriers) override;

    void BeginRenderPass(const RHIRenderPassDesc& desc) override;
    void EndRenderPass() override;

    void SetPipeline(RHIPipeline* pipeline) override;
    void SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset) override;
    void SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, std::span<const uint64> offsets) override;
    void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset) override;

    void SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets) override;
    void SetPushConstants(const void* data, uint32 size, uint32 offset) override;

    void SetViewport(const RHIViewport& viewport) override;
    void SetViewports(std::span<const RHIViewport> viewports) override;
    void SetScissor(const RHIRect& scissor) override;
    void SetScissors(std::span<const RHIRect> scissors) override;

    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance) override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset, uint32 firstInstance) override;
    void DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;
    void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;

    void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override;
    void DispatchIndirect(RHIBuffer* buffer, uint64 offset) override;

    void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size) override;
    void CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc) override;
    void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc) override;
    void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc) override;

    id<MTLCommandBuffer> GetCommandBuffer() const { return m_commandBuffer; }

private:
    id<MTLCommandQueue> m_commandQueue = nil;
    id<MTLCommandBuffer> m_commandBuffer = nil;
    id<MTLRenderCommandEncoder> m_renderEncoder = nil;
    id<MTLComputeCommandEncoder> m_computeEncoder = nil;
    id<MTLBlitCommandEncoder> m_blitEncoder = nil;

    RHICommandQueueType m_queueType;
    bool m_inRenderPass = false;
};
```

---

## 5. API 映射参考

### 5.1 资源类型映射

| RHI 类型 | Metal 类型 | 说明 |
|----------|-----------|------|
| `RHIBuffer` | `MTLBuffer` | 直接映射 |
| `RHITexture` | `MTLTexture` | 直接映射 |
| `RHITextureView` | `MTLTexture` (view) | 使用 `newTextureViewWithPixelFormat:` |
| `RHISampler` | `MTLSamplerState` | 直接映射 |
| `RHIShader` | `MTLLibrary` + `MTLFunction` | Library 包含多个 Function |
| `RHIPipeline` (Graphics) | `MTLRenderPipelineState` | 直接映射 |
| `RHIPipeline` (Compute) | `MTLComputePipelineState` | 直接映射 |
| `RHIDescriptorSetLayout` | Argument Buffer Reflection | 可选实现 |
| `RHIDescriptorSet` | Argument Buffer 或直接绑定 | 两种策略 |
| `RHIPipelineLayout` | N/A (Metal 无显式 layout) | 轻量封装 |
| `RHISwapChain` | `CAMetalLayer` | 通过 MetalKit |
| `RHIFence` | `MTLFence` / `MTLEvent` | 根据同步需求选择 |
| `RHIHeap` | `MTLHeap` | 直接映射 |
| `RHICommandContext` | `MTLCommandBuffer` + Encoder | 组合模式 |

### 5.2 格式映射

| RHIFormat | MTLPixelFormat |
|-----------|---------------|
| `RGBA8_UNORM` | `MTLPixelFormatRGBA8Unorm` |
| `RGBA8_UNORM_SRGB` | `MTLPixelFormatRGBA8Unorm_sRGB` |
| `BGRA8_UNORM` | `MTLPixelFormatBGRA8Unorm` |
| `BGRA8_UNORM_SRGB` | `MTLPixelFormatBGRA8Unorm_sRGB` |
| `RGBA16_FLOAT` | `MTLPixelFormatRGBA16Float` |
| `RGBA32_FLOAT` | `MTLPixelFormatRGBA32Float` |
| `D24_UNORM_S8_UINT` | `MTLPixelFormatDepth24Unorm_Stencil8` (macOS only) |
| `D32_FLOAT` | `MTLPixelFormatDepth32Float` |
| `D32_FLOAT_S8_UINT` | `MTLPixelFormatDepth32Float_Stencil8` |
| `BC1_UNORM` | `MTLPixelFormatBC1_RGBA` |
| `BC3_UNORM` | `MTLPixelFormatBC3_RGBA` |
| `BC7_UNORM` | `MTLPixelFormatBC7_RGBAUnorm` |

### 5.3 资源状态映射

Metal 使用显式资源追踪而非状态转换。主要考虑：

| RHIResourceState | Metal 处理方式 |
|-----------------|---------------|
| `RenderTarget` | `MTLRenderPassDescriptor` 设置 |
| `DepthWrite/Read` | `MTLRenderPassDescriptor` 设置 |
| `ShaderResource` | 通过 encoder `useResource:usage:` |
| `UnorderedAccess` | 通过 encoder `useResource:usage:` |
| `CopySource/Dest` | Blit encoder 自动处理 |
| `Present` | `CAMetalDrawable.present()` |

**注意**: Metal 在大多数情况下自动处理资源同步，但 Hazard Tracking 需要显式管理。

### 5.4 Shader Stage 映射

| RHIShaderStage | Metal Stage |
|---------------|-------------|
| `Vertex` | Vertex Function |
| `Pixel` | Fragment Function |
| `Compute` | Kernel Function |
| `Hull` | Tessellation Control (部分支持) |
| `Domain` | Tessellation Evaluation (部分支持) |
| `Geometry` | ❌ 不支持 |

---

## 6. Shader 编译策略

### 6.1 选项对比

| 策略 | 优点 | 缺点 | 推荐场景 |
|------|------|------|---------|
| **A. DXC + SPIRV-Cross** | 使用现有 HLSL 着色器 | 额外依赖, 可能有转换 bug | 跨平台项目 |
| **B. 直接 MSL** | 性能最优, 最新 Metal 特性 | 需维护两套着色器 | Apple 平台优化 |
| **C. 运行时编译** | 开发迭代快 | 首次加载慢, 无法预编译 | 调试/原型 |

### 6.2 推荐方案: DXC + SPIRV-Cross

```
HLSL Source
    │
    ▼
┌─────────────────┐
│      DXC        │
│  (HLSL → SPIR-V)│
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  SPIRV-Cross    │
│  (SPIR-V → MSL) │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Metal Compiler  │
│ (MSL → MTLLib)  │
└─────────────────┘
```

### 6.3 ShaderCompiler 扩展

```cpp
// ShaderCompiler/Include/ShaderCompiler/ShaderCompiler.h

struct ShaderCompileDesc
{
    // 现有字段...
    
    // Metal 特有选项
    bool generateMSL = false;           // 生成 MSL 源码
    bool compileToMetalLib = false;     // 预编译为 .metallib
    uint32 metalLanguageVersion = 30;   // Metal 3.0
};

struct ShaderCompileResult
{
    // 现有字段...
    
    std::string mslSource;              // Metal Shading Language 源码
    std::vector<uint8_t> metalLibrary;  // 预编译 metallib (可选)
};
```

---

## 7. 实施路线图

### Phase 1: 基础设施 (1-2 周)

- [ ] 修改 `RHIDefinitions.h` 添加 Metal 后端类型
- [ ] 修改 `CMakeLists.txt` 添加 Apple 平台支持
- [ ] 创建 `RHI_Metal` 目录结构
- [ ] 实现 `MetalDevice` 基础框架
- [ ] 实现 `MetalBuffer`, `MetalTexture` 基础资源

### Phase 2: 核心功能 (2-3 周)

- [ ] 实现 `MetalCommandContext` (Render/Compute/Blit)
- [ ] 实现 `MetalPipeline` (Graphics + Compute)
- [ ] 实现 `MetalSwapChain` (CAMetalLayer)
- [ ] 实现 `MetalFence` 同步
- [ ] 集成 ShaderCompiler SPIRV-Cross 支持

### Phase 3: 高级特性 (1-2 周)

- [ ] 实现 `MetalHeap` 和 Placed Resources
- [ ] 实现 `MetalDescriptor` (Argument Buffer)
- [ ] 添加 Tessellation 支持
- [ ] 性能优化 (Triple Buffering, Resource Hazard Tracking)

### Phase 4: 测试与优化 (1 周)

- [ ] Triangle Sample 验证
- [ ] 创建 MetalValidation 测试
- [ ] 性能基准测试
- [ ] 文档完善

---

## 8. 平台支持矩阵

### 8.1 系统要求

| 平台 | 最低版本 | Metal 版本 | 备注 |
|------|---------|-----------|------|
| macOS | 10.15 (Catalina) | Metal 2.2 | 完整支持 |
| macOS | 11.0 (Big Sur) | Metal 2.4 | 推荐版本 |
| macOS | 14.0 (Sonoma) | Metal 3 | 最新特性 |
| iOS | 13.0 | Metal 2.2 | 完整支持 |
| iOS | 17.0 | Metal 3 | 最新特性 |

### 8.2 功能支持矩阵

| 功能 | macOS Intel | macOS Apple Silicon | iOS |
|------|-------------|-------------------|-----|
| 基础渲染 | ✅ | ✅ | ✅ |
| Compute Shader | ✅ | ✅ | ✅ |
| Tessellation | ✅ | ✅ | ✅ |
| Geometry Shader | ❌ | ❌ | ❌ |
| Raytracing | ❌ | ✅ (M1+) | ✅ (A14+) |
| Mesh Shaders | ❌ | ✅ (M3+) | ✅ (A17+) |
| Argument Buffers | ✅ | ✅ | ✅ |
| MTLHeap | ✅ | ✅ | ✅ |
| BC 纹理压缩 | ✅ | ✅ | ❌ (使用 ASTC) |

### 8.3 后端选择建议

| 平台 | 推荐后端 | 备选 |
|------|---------|------|
| Windows | DX12 | Vulkan, DX11 |
| Linux | Vulkan | - |
| macOS | Metal | Vulkan (MoltenVK) |
| iOS | Metal | - |
| Android | Vulkan | - |

---

## 附录 A: 参考资源

- [Metal Programming Guide](https://developer.apple.com/documentation/metal)
- [Metal Best Practices Guide](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/)
- [SPIRV-Cross Documentation](https://github.com/KhronosGroup/SPIRV-Cross)
- [Metal Shading Language Specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)

---

## 附录 B: 代码示例

### 创建 Metal Device

```objc
// MetalDevice.mm
#include "MetalDevice.h"
#import <Metal/Metal.h>

namespace RVX
{
    std::unique_ptr<IRHIDevice> CreateMetalDevice(const RHIDeviceDesc& desc)
    {
        return std::make_unique<MetalDevice>(desc);
    }

    MetalDevice::MetalDevice(const RHIDeviceDesc& desc)
    {
        // 获取默认 Metal 设备
        m_device = MTLCreateSystemDefaultDevice();
        if (!m_device)
        {
            RVX_RHI_ERROR("Failed to create Metal device");
            return;
        }

        RVX_RHI_INFO("Created Metal device: {}", [[m_device name] UTF8String]);

        // 创建命令队列
        m_commandQueue = [m_device newCommandQueue];
        if (!m_commandQueue)
        {
            RVX_RHI_ERROR("Failed to create command queue");
            return;
        }

        // 查询设备能力
        QueryCapabilities();
    }

    void MetalDevice::QueryCapabilities()
    {
        m_capabilities.adapterName = [[m_device name] UTF8String];
        m_capabilities.supportsBindless = [m_device supportsFamily:MTLGPUFamilyApple6];
        m_capabilities.supportsRaytracing = [m_device supportsRaytracing];
        m_capabilities.supportsGeometryShader = false;  // Metal 不支持
        m_capabilities.supportsTessellation = true;
        m_capabilities.maxTextureSize = 16384;
        m_capabilities.maxTextureLayers = 2048;
        m_capabilities.maxColorAttachments = 8;
    }

} // namespace RVX
```

### 创建 Buffer

```objc
// MetalResources.mm (部分)
MetalBuffer::MetalBuffer(id<MTLDevice> device, const RHIBufferDesc& desc)
    : m_size(desc.size)
    , m_usage(desc.usage)
    , m_memoryType(desc.memoryType)
    , m_stride(desc.stride)
{
    MTLResourceOptions options = MTLResourceStorageModeShared;
    
    switch (desc.memoryType)
    {
        case RHIMemoryType::Default:
            options = MTLResourceStorageModePrivate;
            break;
        case RHIMemoryType::Upload:
            options = MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
            break;
        case RHIMemoryType::Readback:
            options = MTLResourceStorageModeShared;
            break;
    }

    m_buffer = [device newBufferWithLength:desc.size options:options];
    
    if (desc.debugName)
    {
        [m_buffer setLabel:[NSString stringWithUTF8String:desc.debugName]];
        SetDebugName(desc.debugName);
    }
}

void* MetalBuffer::Map()
{
    if (m_memoryType == RHIMemoryType::Default)
    {
        RVX_RHI_ERROR("Cannot map GPU-only buffer");
        return nullptr;
    }
    return [m_buffer contents];
}

void MetalBuffer::Unmap()
{
    // Metal shared buffers 不需要显式 unmap
    // 如果需要同步到 GPU，使用 didModifyRange:
}
```

---

*文档结束*
