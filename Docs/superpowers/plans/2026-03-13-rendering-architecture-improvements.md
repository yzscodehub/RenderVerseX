# RenderVerseX 渲染架构改进实施计划

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现4个核心渲染架构改进：异步Compute、StagingBuffer/RingBuffer统一实现、状态追踪增强、GBuffer Pass

**Architecture:**
- 异步Compute：在RenderGraphExecutor中实现fence信号/等待机制
- StagingBuffer/RingBuffer：参考Metal实现，为DX12/Vulkan/DX11/OpenGL创建后端实现
- 状态追踪增强：在RenderGraphBuilder::Read()中使用stages参数生成精细屏障
- GBuffer Pass：创建GBuffer生成Pass，支持延迟渲染

**Tech Stack:** C++20, DirectX 12, Vulkan, OpenGL, RenderGraph

---

## 文件结构

```
RHI_DX12/Private/DX12Upload.h       # 新建：DX12上传缓冲区实现
RHI_DX12/Private/DX12Upload.cpp     # 新建
RHI_Vulkan/Private/VulkanUpload.h   # 新建：Vulkan上传缓冲区实现
RHI_Vulkan/Private/VulkanUpload.cpp # 新建
RHI_DX11/Private/DX11Upload.h      # 新建：DX11上传缓冲区实现
RHI_DX11/Private/DX11Upload.cpp    # 新建
RHI_OpenGL/Private/OpenGLUpload.h  # 新建：OpenGL上传缓冲区实现
RHI_OpenGL/Private/OpenGLUpload.cpp # 新建
Render/Private/Graph/RenderGraphExecutor.cpp  # 修改：异步Compute实现
Render/Private/Graph/RenderGraph.cpp         # 修改：stages参数实现
Render/Include/Render/Passes/GBufferPass.h   # 新建：GBuffer Pass头文件
Render/Private/Passes/GBufferPass.cpp        # 新建：GBuffer Pass实现
```

---

## Chunk 1: 异步Compute实现 (P0)

### Task 1: 实现RenderGraphExecutor中的fence同步机制

**Files:**
- Modify: `Render/Private/Graph/RenderGraphExecutor.cpp:136-297`
- Reference: `RHI/Include/RHI/RHIDevice.h:105` (SubmitCommandContext with fence)

- [ ] **Step 1: 分析现有代码结构**

查看 ExecuteRenderGraphAsync 函数，理解当前的pass执行逻辑：
```cpp
// 位置: RenderGraphExecutor.cpp:136
void ExecuteRenderGraphAsync(RenderGraphImpl& graph,
                              RHICommandContext& graphicsCtx,
                              RHICommandContext* computeCtx,
                              RHIFence* computeFence)
```

- [ ] **Step 2: 实现fence信号逻辑**

在computeCtx执行完后添加信号：
```cpp
// 在compute pass执行完后
if (useCompute && computeFence) {
    // 为当前frame分配唯一的fence值
    uint64 fenceValue = frameIndex + 1;
    computeCtx->SignalFence(computeFence, fenceValue);
}
```

注意：需要检查RHICommandContext是否有SignalFence方法，如果没有，需要使用device的SubmitCommandContext

- [ ] **Step 3: 实现fence等待逻辑**

在graphicsCtx需要读取compute结果时添加等待：
```cpp
// 在graphics pass需要读取compute结果时
if (!useCompute && needsWaitOnCompute && computeFence) {
    graphicsCtx.WaitFence(computeFence, computeFenceValue);
}
```

- [ ] **Step 4: 添加fence值追踪**

在RenderGraphExecutor类中添加成员变量：
```cpp
std::atomic<uint64> m_computeFenceValue = 0;
```

- [ ] **Step 5: 测试验证**

编译项目，确保无错误：
```bash
cmake --build build --config Release
```

---

## Chunk 2: DX12 StagingBuffer/RingBuffer实现 (P0)

### Task 2: 创建DX12上传缓冲区头文件

**Files:**
- Create: `RHI_DX12/Private/DX12Upload.h`
- Reference: `RHI_Metal/Private/MetalUpload.h` (参考实现)
- Reference: `RHI/Include/RHI/RHIUpload.h` (接口定义)

- [ ] **Step 1: 创建DX12StagingBuffer类**

参考Metal实现，创建DX12版本：
```cpp
class DX12StagingBuffer : public RHIStagingBuffer
{
public:
    DX12StagingBuffer(DX12Device* device, const RHIStagingBufferDesc& desc);
    ~DX12StagingBuffer() override;

    void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override;
    void Unmap() override;
    uint64 GetSize() const override { return m_size; }
    RHIBuffer* GetBuffer() const override;

    ID3D12Resource* GetResource() const { return m_resource.Get(); }

private:
    DX12Device* m_device = nullptr;
    ComPtr<ID3D12Resource> m_resource;
    uint64 m_size = 0;
    void* m_mappedData = nullptr;
    bool m_isMapped = false;
    mutable RHIBufferRef m_wrapperBuffer;
};
```

- [ ] **Step 2: 创建DX12RingBuffer类**

```cpp
class DX12RingBuffer : public RHIRingBuffer
{
public:
    DX12RingBuffer(DX12Device* device, const RHIRingBufferDesc& desc);
    ~DX12RingBuffer() override;

    RHIRingAllocation Allocate(uint64 size) override;
    void Reset(uint32 frameIndex) override;
    RHIBuffer* GetBuffer() const override;
    uint64 GetSize() const override { return m_totalSize; }
    uint32 GetAlignment() const override { return m_alignment; }

    ID3D12Resource* GetResource() const { return m_resource.Get(); }

private:
    DX12Device* m_device = nullptr;
    ComPtr<ID3D12Resource> m_resource;

    uint64 m_totalSize = 0;
    uint32 m_alignment = 256;

    std::atomic<uint64> m_head = 0;
    std::atomic<uint64> m_tail = 0;
    void* m_mappedData = nullptr;

    static constexpr uint32 MAX_FRAMES_IN_FLIGHT = 3;
    std::array<uint64, MAX_FRAMES_IN_FLIGHT> m_frameFenceValues = {};
    std::mutex m_mutex;

    mutable RHIBufferRef m_wrapperBuffer;
};
```

- [ ] **Step 3: 添加工厂函数声明**

```cpp
RHIStagingBufferRef CreateDX12StagingBuffer(DX12Device* device, const RHIStagingBufferDesc& desc);
RHIRingBufferRef CreateDX12RingBuffer(DX12Device* device, const RHIRingBufferDesc& desc);
```

### Task 3: 实现DX12上传缓冲区

**Files:**
- Create: `RHI_DX12/Private/DX12Upload.cpp`

- [ ] **Step 1: 实现DX12StagingBuffer构造函数**

使用Upload Heap创建缓冲区：
```cpp
DX12StagingBuffer::DX12StagingBuffer(DX12Device* device, const RHIStagingBufferDesc& desc)
    : m_device(device), m_size(desc.size)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = m_size;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = device->GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_resource));

    if (SUCCEEDED(hr)) {
        m_resource->SetName(desc.debugName ? desc.debugName : L"DX12StagingBuffer");
    }
}
```

- [ ] **Step 2: 实现Map/Unmap**

```cpp
void* DX12StagingBuffer::Map(uint64 offset, uint64 size)
{
    if (m_isMapped) return m_mappedData;

    D3D12_RANGE range = {offset, offset + (size == RVX_WHOLE_SIZE ? m_size - offset : size)};
    HRESULT hr = m_resource->Map(0, &range, &m_mappedData);
    if (SUCCEEDED(hr)) {
        m_isMapped = true;
    }
    return m_mappedData;
}

void DX12StagingBuffer::Unmap()
{
    if (!m_isMapped) return;

    D3D12_RANGE emptyRange = {0, 0};
    m_resource->Unmap(0, &emptyRange);
    m_mappedData = nullptr;
    m_isMapped = false;
}
```

- [ ] **Step 3: 实现DX12RingBuffer::Allocate**

```cpp
RHIRingAllocation DX12RingBuffer::Allocate(uint64 size)
{
    // 对齐分配大小
    uint64 alignedSize = (size + m_alignment - 1) & ~(m_alignment - 1);

    uint64 currentHead = m_head.load(std::memory_order_acquire);

    // 检查是否需要等待（环形缓冲已满）
    while (true) {
        uint64 nextHead = currentHead + alignedSize;

        // 如果环绕，回到开始
        if (nextHead >= m_totalSize) {
            currentHead = 0;
            nextHead = alignedSize;
        }

        // 检查是否有足够空间（需要等待GPU完成）
        uint64 tail = m_tail.load(std::memory_order_acquire);
        if (nextHead > tail && nextHead > m_totalSize - tail) {
            // 空间不足，需要等待
            // 在实际实现中需要fence同步
            std::this_thread::yield();
            continue;
        }

        // 尝试原子更新head
        if (m_head.compare_exchange_weak(currentHead, nextHead, std::memory_order_release)) {
            RHIRingAllocation alloc;
            alloc.cpuAddress = static_cast<uint8*>(m_mappedData) + currentHead;
            alloc.gpuOffset = currentHead;
            alloc.size = alignedSize;
            alloc.buffer = m_wrapperBuffer.get();
            return alloc;
        }
    }
}
```

- [ ] **Step 4: 实现GetBuffer()包装器**

```cpp
RHIBuffer* DX12StagingBuffer::GetBuffer() const
{
    if (!m_wrapperBuffer) {
        RHIBufferDesc desc = {};
        desc.size = m_size;
        desc.usage = RHIBufferUsage::CopySrc;
        desc.memoryType = RHIMemoryType::Upload;
        m_wrapperBuffer = CreateDX12Buffer(m_device, desc);
    }
    return m_wrapperBuffer.get();
}
```

### Task 4: 修改DX12Device使用新实现

**Files:**
- Modify: `RHI_DX12/Private/DX12Device.cpp:1004-1024`

- [ ] **Step 1: 添加include**

```cpp
#include "DX12Upload.h"
```

- [ ] **Step 2: 修改CreateStagingBuffer实现**

```cpp
RHIStagingBufferRef DX12Device::CreateStagingBuffer(const RHIStagingBufferDesc& desc)
{
    return CreateDX12StagingBuffer(this, desc);
}
```

- [ ] **Step 3: 修改CreateRingBuffer实现**

```cpp
RHIRingBufferRef DX12Device::CreateRingBuffer(const RHIRingBufferDesc& desc)
{
    return CreateDX12RingBuffer(this, desc);
}
```

---

## Chunk 3: Vulkan StagingBuffer/RingBuffer实现 (P0)

### Task 5: 创建Vulkan上传缓冲区

**Files:**
- Create: `RHI_Vulkan/Private/VulkanUpload.h`
- Create: `RHI_Vulkan/Private/VulkanUpload.cpp`
- Modify: `RHI_Vulkan/Private/VulkanDevice.cpp:1173-1185`

- [ ] **Step 1: 创建VulkanStagingBuffer类 (VulkanUpload.h)**

```cpp
class VulkanStagingBuffer : public RHIStagingBuffer
{
public:
    VulkanStagingBuffer(VulkanDevice* device, const RHIStagingBufferDesc& desc);
    ~VulkanStagingBuffer() override;

    void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override;
    void Unmap() override;
    uint64 GetSize() const override { return m_size; }
    RHIBuffer* GetBuffer() const override;

    VkBuffer GetVKBuffer() const { return m_buffer; }

private:
    VulkanDevice* m_device = nullptr;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    uint64 m_size = 0;
    void* m_mappedData = nullptr;
    bool m_isMapped = false;
    mutable RHIBufferRef m_wrapperBuffer;
};
```

- [ ] **Step 2: 创建VulkanRingBuffer类 (VulkanUpload.h)**

```cpp
class VulkanRingBuffer : public RHIRingBuffer
{
public:
    VulkanRingBuffer(VulkanDevice* device, const RHIRingBufferDesc& desc);
    ~VulkanRingBuffer() override;

    RHIRingAllocation Allocate(uint64 size) override;
    void Reset(uint32 frameIndex) override;
    RHIBuffer* GetBuffer() const override;
    uint64 GetSize() const override { return m_totalSize; }
    uint32 GetAlignment() const override { return m_alignment; }

    VkBuffer GetVKBuffer() const { return m_buffer; }

private:
    VulkanDevice* m_device = nullptr;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;

    uint64 m_totalSize = 0;
    uint32 m_alignment = 256;

    std::atomic<uint64> m_head = 0;
    std::atomic<uint64> m_tail = 0;
    void* m_mappedData = nullptr;

    static constexpr uint32 MAX_FRAMES_IN_FLIGHT = 3;
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> m_frameSemaphores = {};
    std::mutex m_mutex;

    mutable RHIBufferRef m_wrapperBuffer;
};
```

- [ ] **Step 3: 实现VulkanStagingBuffer (VulkanUpload.cpp)**

使用HOST_VISIBLE_MEMORY创建缓冲区：
```cpp
VulkanStagingBuffer::VulkanStagingBuffer(VulkanDevice* device, const RHIStagingBufferDesc& desc)
    : m_device(device), m_size(desc.size)
{
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = m_size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    vkCreateBuffer(device->GetDevice(), &bufferInfo, nullptr, &m_buffer);

    // 获取内存需求并分配HOST_VISIBLE内存
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device->GetDevice(), m_buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = device->FindMemoryType(
        memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(device->GetDevice(), &allocInfo, nullptr, &m_memory);
    vkBindBufferMemory(device->GetDevice(), m_buffer, m_memory, 0);
}
```

- [ ] **Step 4: 实现VulkanRingBuffer**

类似DX12，但使用Vulkan同步原语（Semaphore）进行帧同步。

- [ ] **Step 5: 修改VulkanDevice.cpp**

```cpp
#include "VulkanUpload.h"

RHIStagingBufferRef VulkanDevice::CreateStagingBuffer(const RHIStagingBufferDesc& desc)
{
    return CreateVulkanStagingBuffer(this, desc);
}

RHIRingBufferRef VulkanDevice::CreateRingBuffer(const RHIRingBufferDesc& desc)
{
    return CreateVulkanRingBuffer(this, desc);
}
```

---

## Chunk 4: DX11/OpenGL StagingBuffer/RingBuffer实现 (P0)

### Task 6: 创建DX11上传缓冲区

**Files:**
- Create: `RHI_DX11/Private/DX11Upload.h`
- Create: `RHI_DX11/Private/DX11Upload.cpp`
- Modify: `RHI_DX11/Private/DX11Device.cpp:616-630`

- [ ] **Step 1: 创建DX11StagingBuffer类**

使用D3D11_USAGE_STAGING创建：
```cpp
class DX11StagingBuffer : public RHIStagingBuffer
{
public:
    DX11StagingBuffer(DX11Device* device, const RHIStagingBufferDesc& desc);
    ~DX11StagingBuffer() override;

    void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override;
    void Unmap() override;
    uint64 GetSize() const override { return m_size; }
    RHIBuffer* GetBuffer() const override;

    ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }

private:
    DX11Device* m_device = nullptr;
    ComPtr<ID3D11Buffer> m_buffer;
    uint64 m_size = 0;
    mutable RHIBufferRef m_wrapperBuffer;
};
```

- [ ] **Step 2: 创建DX11RingBuffer类**

DX11使用双缓冲或三缓冲策略：
```cpp
class DX11RingBuffer : public RHIRingBuffer
{
public:
    // 使用双缓冲策略
    static constexpr uint32 MAX_FRAMES_IN_FLIGHT = 2;
    // ...
};
```

### Task 7: 创建OpenGL上传缓冲区

**Files:**
- Create: `RHI_OpenGL/Private/OpenGLUpload.h`
- Create: `RHI_OpenGL/Private/OpenGLUpload.cpp`
- Modify: `RHI_OpenGL/Private/OpenGLDevice.cpp:588-600`

- [ ] **Step 1: 创建OpenGLStagingBuffer类**

使用PBO (Pixel Buffer Object)：
```cpp
class OpenGLStagingBuffer : public RHIStagingBuffer
{
public:
    OpenGLStagingBuffer(OpenGLDevice* device, const RHIStagingBufferDesc& desc);
    ~OpenGLStagingBuffer() override;

    void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override;
    void Unmap() override;
    uint64 GetSize() const override { return m_size; }
    RHIBuffer* GetBuffer() const override;

    GLuint GetPBO() const { return m_pbo; }

private:
    OpenGLDevice* m_device = nullptr;
    GLuint m_pbo = 0;
    uint64 m_size = 0;
    void* m_mappedData = nullptr;
    bool m_isMapped = false;
    mutable RHIBufferRef m_wrapperBuffer;
};
```

- [ ] **Step 2: 创建OpenGLRingBuffer类**

使用双缓冲PBO：
```cpp
class OpenGLRingBuffer : public RHIRingBuffer
{
    // 使用两个PBO交替使用
    std::array<GLuint, 2> m_pbos = {};
    // ...
};
```

---

## Chunk 5: 状态追踪增强 (P1)

### Task 8: 实现stages参数使用

**Files:**
- Modify: `Render/Private/Graph/RenderGraph.cpp:186-210`
- Modify: `Render/Private/Graph/RenderGraphExecutor.cpp`

- [ ] **Step 1: 分析stages参数用途**

RHIShaderStage 定义:
- Vertex - 顶点着色器
- Pixel - 像素着色器
- Compute - 计算着色器
- AllGraphics - 所有图形阶段

- [ ] **Step 2: 修改RenderGraphBuilder::Read实现**

```cpp
RGTextureHandle RenderGraphBuilder::Read(RGTextureHandle texture, RHIShaderStage stages)
{
    // 查找资源
    auto it = std::find_if(graph.textures.begin(), graph.textures.end(),
        [id = texture.GetId()](const RGTexture& t) { return t.id == id; });

    if (it == graph.textures.end()) return texture;

    // 创建读取使用记录，包含stages信息
    RGTextureUsage usage;
    usage.texture = &(*it);
    usage.state = RHIResourceState::ShaderRead;
    usage.stages = stages;  // 使用传入的stages参数

    // 添加到当前pass的使用列表
    if (currentPass) {
        currentPass->usages.push_back(usage);
    }

    // 记录到资源的读取列表
    it->reads.push_back(usage);

    return texture;
}
```

- [ ] **Step 3: 在RenderGraphExecutor中使用stages生成精细屏障**

```cpp
void ComputeShaderReadBarrier(const RGTextureUsage& usage)
{
    RHITextureBarrier barrier = {};
    barrier.texture = usage.texture->texture;
    barrier.beforeState = usage.texture->state;
    barrier.afterState = RHIResourceState::ShaderRead;

    // 根据stages设置正确的subresource
    if (usage.stages == RHIShaderStage::Vertex) {
        // 只对顶点着色器可见
        barrier.subresource.mipLevel = 0;  // 或具体mip
    } else if (usage.stages == RHIShaderStage::Pixel) {
        // 像素着色器
    }
    // ...
}
```

---

## Chunk 6: GBuffer Pass实现 (P1)

### Task 9: 创建GBuffer Pass头文件

**Files:**
- Create: `Render/Include/Render/Passes/GBufferPass.h`

- [ ] **Step 1: 定义GBuffer数据结构**

```cpp
#pragma once
#include "Render/Graph/RenderGraph.h"
#include "Render/Passes/IRenderPass.h"

namespace RVX
{
    // =============================================================================
    // GBuffer Pass Data
    // =============================================================================
    struct GBufferPassData
    {
        // 输入
        RGTextureHandle depthTexture;

        // 输出
        RGTextureHandle albedoRT;      // RGBA8: Albedo(RGB) + AO(A)
        RGTextureHandle normalRT;      // RGBA16F: Normal(XYZ) + Metallic(A)
        RGTextureHandle roughnessRT;   // RGBA16F: Roughness + padding
        RGTextureHandle velocityRT;    // RG16F: Velocity(XY) (optional, for TAA)
    };

    // =============================================================================
    // GBuffer Pass
    // =============================================================================
    class GBufferPass : public IRenderPass
    {
    public:
        static constexpr const char* kName = "GBuffer";

        GBufferPass();
        ~GBufferPass() override;

        // IRenderPass interface
        void AddPass(RenderGraphBuilder& builder, const GBufferPassData& data) override;
        void Execute(RHICommandContext& ctx, const GBufferPassData& data) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
```

### Task 10: 实现GBuffer Pass

**Files:**
- Create: `Render/Private/Passes/GBufferPass.cpp`

- [ ] **Step 1: 实现AddPass**

```cpp
void GBufferPass::AddPass(RenderGraphBuilder& builder, const GBufferPassData& data)
{
    // 创建RenderTarget描述
    RHIRenderPassDesc passDesc;

    // Albedo RT
    RGTextureDesc albedoDesc = {};
    albedoDesc.width = GetResolution(data.depthTexture).x;
    albedoDesc.height = GetResolution(data.depthTexture).y;
    albedoDesc.format = RHIFormat::RGBA8_UNORM;
    albedoDesc.usage = RHITextureUsage::RenderTarget;
    data.albedoRT = builder.CreateTexture("GBuffer_Albedo", albedoDesc);

    // Normal RT
    RGTextureDesc normalDesc = {};
    normalDesc.format = RHIFormat::RGBA16_FLOAT;
    data.normalRT = builder.CreateTexture("GBuffer_Normal", normalDesc);

    // Roughness RT
    RGTextureDesc roughnessDesc = {};
    roughnessDesc.format = RHIFormat::RGBA16_FLOAT;
    data.roughnessRT = builder.CreateTexture("GBuffer_Roughness", roughnessDesc);

    // 设置RenderTarget
    passDesc.renderTargets.push_back({data.albedoRT, RHIResourceState::RenderTarget});
    passDesc.renderTargets.push_back({data.normalRT, RHIResourceState::RenderTarget});
    passDesc.renderTargets.push_back({data.roughnessRT, RHIResourceState::RenderTarget});
    passDesc.depthStencil = {data.depthTexture, RHIResourceState::DepthWrite};

    builder.SetPassRenderTarget(0, passDesc);
}
```

- [ ] **Step 2: 实现Execute**

```cpp
void GBufferPass::Execute(RHICommandContext& ctx, const GBufferPassData& data)
{
    // 设置渲染管线
    ctx.SetPipeline(m_gBufferPipeline.get());

    // 设置RenderTarget
    RHIRenderPassDesc passDesc;
    passDesc.renderTargets.push_back({data.albedoRT.get(), RHIResourceState::RenderTarget});
    passDesc.renderTargets.push_back({data.normalRT.get(), RHIResourceState::RenderTarget});
    passDesc.renderTargets.push_back({data.roughnessRT.get(), RHIResourceState::RenderTarget});
    passDesc.depthStencil = {data.depthTexture.get(), RHIResourceState::DepthWrite};
    ctx.BeginRenderPass(passDesc);

    // 渲染所有不透明物体
    for (auto& mesh : m_opaqueMeshes) {
        RenderMesh(ctx, mesh);
    }

    ctx.EndRenderPass();
}
```

- [ ] **Step 3: 添加到RenderGraph注册**

```cpp
// 在RenderGraph.cpp或PassRegistry中注册
REGISTER_RENDER_PASS(GBufferPass, GBufferPassData);
```

---

## 验证步骤

### 编译验证
```bash
cmake --build build --config Release
```

### 功能验证
1. 运行Samples中的示例程序
2. 验证后处理效果正常显示

### 测试验证
```bash
# 运行RHI相关测试
./build/Tests/Release/DX12Validation.exe
./build/Tests/Release/VulkanValidation.exe
```

---

## 实施顺序总结

1. **Chunk 1**: 异步Compute (P0) - 最简单，先验证框架
2. **Chunk 2**: DX12 StagingBuffer/RingBuffer (P0)
3. **Chunk 3**: Vulkan StagingBuffer/RingBuffer (P0)
4. **Chunk 4**: DX11/OpenGL StagingBuffer/RingBuffer (P0)
5. **Chunk 5**: 状态追踪增强 (P1)
6. **Chunk 6**: GBuffer Pass (P1)
