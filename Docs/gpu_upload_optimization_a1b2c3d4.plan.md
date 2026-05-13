---
name: GPU Upload System Optimization
overview: 基于对 GPU Upload 全链路（GPUUploadService → GPUResourceManager → RenderSubsystem → RHI DX12）的深度分析，制定全面的性能优化计划，消除渲染线程阻塞、减少 GPU 同步开销、降低 CPU 分配压力。
todos:
  - id: p0-render-thread-sync
    content: "P0: 消除渲染线程阻塞同步上传，改为异步预加载 + 渲染时跳过未就绪资源"
    status: pending
  - id: p0-command-context-per-upload
    content: "P0: 消除每上传一个 buffer 创建一个 CommandContext 的问题，改为持久化 Copy Queue Context + 批量提交"
    status: pending
  - id: p0-command-allocator-pool
    content: "P0: 让 DX12CommandContext 使用已有的 DX12CommandAllocatorPool（当前完全未使用）"
    status: pending
  - id: p0-texture-immediate-empty
    content: "P0: 修复 UploadTextureImmediate() 空壳实现（创建 texture 但不写入数据）"
    status: pending
  - id: p1-staging-buffer-pool
    content: "P1: 实现 StagingBuffer 池化，避免每次上传分配新的 Upload Heap"
    status: pending
  - id: p1-fence-reuse
    content: "P1: 实现 Fence 复用池，避免每个上传创建一个 Fence 对象"
    status: pending
  - id: p1-time-budget-accuracy
    content: "P1: 修复 ProcessPendingUploads 的时间预算计算不精确问题（UploadMesh 内部耗时未计入）"
    status: pending
  - id: p1-priority-queue-comment
    content: "P1: 修正 PendingUpload operator< 的误导性注释"
    status: pending
  - id: p1-completed-scan-optimization
    content: "P1: 优化 UpdateCompletedResourceUploads()，避免遍历所有非 pending 资源"
    status: pending
  - id: p2-async-texture-upload
    content: "P2: 扩展纹理异步上传路径，支持 mipmaps、cubemap、3D texture 的 staged copy"
    status: pending
  - id: p2-upload-ring-buffer
    content: "P2: 评估并引入 RingBuffer 替代独立 StagingBuffer 用于高频小数据上传"
    status: pending
  - id: p2-upload-metrics
    content: "P2: 增强 Upload Stats，添加每帧上传耗时、队列深度、带宽利用率等指标"
    status: pending
---

# GPU Upload 系统优化计划

## 执行摘要

当前 GPU Upload 模块在功能上能够完成 CPU→GPU 资源上传，但在高负载场景下存在严重的性能瓶颈：

| 场景 | 当前 CPU 耗时 | 目标 CPU 耗时 | 主要瓶颈 |
|------|-------------|-------------|---------|
| 加载 10 个 mesh（每个 5 个 buffer） | **15-30ms** 渲染线程阻塞 | **<1ms** 非阻塞 | 每 buffer 创建 CommandContext + 同步上传 |
| 大量小纹理上传 | 线性增长 | 亚毫秒级 | StagingBuffer/Fence 反复创建 |
| 帧时间稳定性 | 偶发尖峰 | 平滑 | 渲染线程阻塞等待上传完成 |

**核心结论**：当前 async upload queue 被 `EnsureVisibleResourcesResident()` 的同步上传完全架空。必须先移除渲染线程上的阻塞同步路径，再优化底层 RHI 实现。

---

## Phase 0: 问题全景分析

### 0.1 调用链路（渲染一帧时）

```
Engine::Tick()
  ├─ Step 4: ProcessGPUUploads(2.0f)        ← 处理 async queue（但 queue 几乎为空）
  │     └─ GPUResourceManager::ProcessPendingUploads()
  │           └─ UploadMesh() / UploadTexture()  ← 每个 buffer 创建独立 CommandContext
  │
  └─ Step 5: RenderFrame()
        └─ RenderSubsystem::Render(world, camera)
              ├─ SceneRenderer::SetupView()        ← 收集可见对象
              ├─ EnsureVisibleResourcesResident()  ← 渲染线程阻塞！同步 UploadImmediate()
              │     └─ 对每个非 resident 可见 mesh:
              │           gpuMgr->UploadImmediate(meshHandle.Get())  ← 完全同步
              └─ SceneRenderer::Render()            ← 实际渲染（被上面的同步阻塞）
```

### 0.2 关键问题清单

| ID | 严重度 | 问题描述 | 影响文件 | 量化影响 |
|----|--------|---------|---------|---------|
| A1 | **P0** | 渲染线程阻塞同步上传 | `RenderSubsystem.cpp:156-185` | 每 mesh 1-3ms 渲染线程阻塞 |
| A2 | **P0** | 每个 buffer 上传创建一个 CommandContext | `GPUUploadService.cpp:165-241` | 10 mesh × 5 buffer = 50 次上下文创建 |
| A3 | **P0** | DX12CommandAllocatorPool 完全未使用 | `DX12CommandContext.cpp:27-28` | 每帧泄漏/重建 allocator |
| A4 | **P0** | `UploadTextureImmediate()` 空壳实现 | `GPUUploadService.cpp:377-397` | 创建 texture 但不写入任何数据 |
| B1 | P1 | 每次上传创建新的 StagingBuffer | `GPUUploadService.cpp:179-185` | Upload Heap 反复分配 |
| B2 | P1 | 每次上传创建新的 Fence | `GPUUploadService.cpp:216-237` | Kernel 对象创建开销 |
| B3 | P1 | 时间预算未包含 UploadMesh 内部耗时 | `GPUResourceManager.cpp:181-227` | 实际耗时可能远超 budget |
| B4 | P1 | `UpdateCompletedResourceUploads()` 全量扫描 | `GPUResourceManager.cpp:635-689` | O(N) 每帧，N = 总资源数 |
| B5 | P1 | `PendingUpload::operator<` 注释误导 | `GPUResourceManager.h:261-265` | 维护风险 |
| C1 | P2 | 纹理 staged upload 仅支持 2D/单 mip | `GPUUploadService.cpp:279-298` | Cubemap、3D、Mipmap 走 fallback |
| C2 | P2 | RingBuffer 已存在但 upload service 未使用 | `DX12Upload.cpp:160-360` | 高频小数据可以走 ring buffer |

---

## Phase 1: P0 关键修复（阻塞问题）

### 1.1 消除渲染线程阻塞同步上传

**问题**: `RenderSubsystem::EnsureVisibleResourcesResident()` 在渲染线程上对每个可见但非 resident 的 mesh 调用 `UploadImmediate()`，这是完全同步的操作。

**当前代码** (`RenderSubsystem.cpp:156-185`):
```cpp
void RenderSubsystem::EnsureVisibleResourcesResident()
{
    // ...
    for (uint32_t idx : visibleIndices)
    {
        const auto& obj = renderScene.GetObject(idx);
        if (!gpuMgr->IsResident(obj.meshId))
        {
            Resource::ResourceHandle<Resource::MeshResource> meshHandle = 
                Resource::ResourceManager::Get().Load<Resource::MeshResource>(obj.meshId);
            if (meshHandle.IsValid())
            {
                gpuMgr->UploadImmediate(meshHandle.Get());  // ← 阻塞！
            }
        }
    }
}
```

**修改方案**:

1. **改为异步预加载 + 渲染时跳过未就绪资源**:

```cpp
void RenderSubsystem::EnsureVisibleResourcesResident()
{
    auto* gpuMgr = m_sceneRenderer ? m_sceneRenderer->GetGPUResourceManager() : nullptr;
    if (!gpuMgr)
        return;

    const auto& renderScene = m_sceneRenderer->GetRenderScene();
    const auto& visibleIndices = m_sceneRenderer->GetVisibleObjectIndices();

    for (uint32_t idx : visibleIndices)
    {
        const auto& obj = renderScene.GetObject(idx);
        
        if (gpuMgr->IsResident(obj.meshId))
        {
            gpuMgr->MarkUsed(obj.meshId);  // 更新 LRU
            continue;
        }

        // 已经是 queued/uploading 状态的，不再重复请求
        if (gpuMgr->GetResourceState(obj.meshId) == GPUResourceState::UploadQueued ||
            gpuMgr->GetResourceState(obj.meshId) == GPUResourceState::Uploading)
        {
            continue;
        }

        // 仅将资源加入异步上传队列，不阻塞
        Resource::ResourceHandle<Resource::MeshResource> meshHandle = 
            Resource::ResourceManager::Get().Load<Resource::MeshResource>(obj.meshId);
        if (meshHandle.IsValid())
        {
            // 可见对象赋予高优先级
            gpuMgr->RequestUpload(meshHandle.Get(), UploadPriority::High);
        }
    }
}
```

2. **SceneRenderer 渲染时跳过未就绪资源**:

在 `SceneRenderer::BuildRenderGraph()` 或 `OpaquePass::Execute()` 中，只渲染 `IsGPUReady()` 的对象：

```cpp
// 在 OpaquePass 或 SceneRenderer 收集绘制命令时
for (uint32_t idx : visibleIndices)
{
    const auto& obj = renderScene.GetObject(idx);
    
    auto buffers = gpuMgr->GetMeshBuffers(obj.meshId);
    if (!buffers.IsValid())
    {
        // 资源尚未就绪，跳过（下一帧会重试）
        continue;
    }
    
    // 正常设置 vertex/index buffer 并绘制
    ctx->SetVertexBuffer(0, buffers.positionBuffer);
    // ...
    ctx->DrawIndexed(...);
}
```

3. **保留 UploadImmediate 用于启动/加载屏幕**: `UploadImmediate` 作为 API 保留，但仅用于：
   - 引擎启动时的强制预加载（加载屏幕期间）
   - 用户显式调用的阻塞加载
   - 单元测试

**涉及文件**:
- `Render/Private/RenderSubsystem.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Private/Passes/OpaquePass.cpp`（或其他实际绘制的地方）

---

### 1.2 消除每上传一个 Buffer 创建一个 CommandContext

**问题**: `TryUploadBufferStaged()` 每个 buffer 都调用 `m_device->CreateCommandContext(RHICommandQueueType::Copy)`，创建一个全新的 Copy CommandContext，包含 CommandAllocator + CommandList + 相关状态。

**当前代码** (`GPUUploadService.cpp:165-241`):
```cpp
GPUUploadBufferResult GPUUploadService::TryUploadBufferStaged(...)
{
    auto commandContext = m_device->CreateCommandContext(RHICommandQueueType::Copy);
    // ... create staging buffer, create buffer, record copy, submit with fence
}
```

**修改方案**: 引入 **Persistent Copy CommandContext**，按帧批量提交。

```cpp
class GPUUploadService
{
public:
    void Initialize(IRHIDevice* device);
    void Shutdown();
    
    // 新增：每帧调用，提交当前 batch 并获取新的 context
    void FlushBatchUploads();
    
private:
    IRHIDevice* m_device = nullptr;
    
    // 持久化的 Copy Queue CommandContext
    RHICommandContextRef m_copyContext;
    bool m_copyContextDirty = false;
    
    // 当前 batch 中待提交的 uploads（用于 fence 追踪）
    struct BatchUpload
    {
        uint64 uploadId;
        RHIStagingBufferRef stagingBuffer;
        uint64 stagingBytes;
    };
    std::vector<BatchUpload> m_currentBatch;
    
    // Fence 复用（见 1.4）
    // ...
};
```

```cpp
void GPUUploadService::Initialize(IRHIDevice* device)
{
    m_device = device;
    m_copyContext = m_device->CreateCommandContext(RHICommandQueueType::Copy);
    m_copyContext->Begin();
}

void GPUUploadService::FlushBatchUploads()
{
    if (!m_copyContextDirty || m_currentBatch.empty())
        return;
    
    m_copyContext->End();
    
    // 复用一个 fence 来追踪整个 batch
    auto fence = AcquireFence();  // 从 pool 获取
    const uint64 fenceValue = fence->GetNextValue();
    m_device->SubmitCommandContext(m_copyContext.Get(), fence.Get());
    
    // 追踪整个 batch 的完成
    for (auto& batch : m_currentBatch)
    {
        TrackPendingUpload(batch.uploadId, fence, fenceValue, batch.stagingBytes);
    }
    m_currentBatch.clear();
    
    // 获取新的 context 用于下一帧
    m_copyContext = m_device->CreateCommandContext(RHICommandQueueType::Copy);
    m_copyContext->Begin();
    m_copyContextDirty = false;
}

GPUUploadBufferResult GPUUploadService::TryUploadBufferStaged(const GPUUploadBufferDesc& desc, const void* data, uint64 dataSize)
{
    // 不再创建新的 command context
    // 使用 m_copyContext
    
    auto stagingBuffer = m_device->CreateStagingBuffer(stagingDesc);
    auto buffer = m_device->CreateBuffer(bufferDesc);
    
    // Map staging, memcpy, unmap
    void* mapped = stagingBuffer->Map(0, dataSize);
    std::memcpy(mapped, data, static_cast<size_t>(dataSize));
    stagingBuffer->Unmap();
    
    // 记录到持久的 copy context
    m_copyContext->CopyBuffer(stagingBuffer->GetBuffer(), buffer.Get(), 0, 0, dataSize);
    m_copyContext->BufferBarrier(buffer.Get(), RHIResourceState::CopyDest, RHIResourceState::Common);
    m_copyContextDirty = true;
    
    // 加入当前 batch（不在此处 submit）
    uint64 uploadId = m_nextUploadId++;
    m_currentBatch.push_back({uploadId, stagingBuffer, dataSize});
    
    GPUUploadBufferResult result;
    result.resource = buffer;
    result.succeeded = true;
    result.mode = GPUUploadMode::StagedCopy;
    result.bytesUploaded = dataSize;
    result.uploadId = uploadId;
    result.isPending = true;  // 等待 batch flush
    return result;
}
```

**调用时序**:
```
Engine::Tick()
  ├─ ProcessGPUUploads(2.0f)
  │     └─ GPUResourceManager::ProcessPendingUploads()
  │           └─ 调用 UploadMesh() → 多个 buffer 记录到同一个 copy context
  │
  ├─ RenderFrame() 之前:
  │     └─ GPUUploadService::FlushBatchUploads()  ← 提交整批
  │
  └─ RenderFrame()
        └─ 正常渲染（此时 copy queue 在 GPU 上并行执行）
```

**涉及文件**:
- `Render/Include/Render/GPUUploadService.h`
- `Render/Private/GPUUploadService.cpp`

---

### 1.3 让 DX12CommandContext 使用 DX12CommandAllocatorPool

**问题**: `DX12CommandContext` 构造函数直接从 D3D12 Device 创建 CommandAllocator，而项目中已有一个完整实现的 `DX12CommandAllocatorPool` 完全未被使用。

**当前代码** (`DX12CommandContext.cpp:12-33`):
```cpp
DX12CommandContext::DX12CommandContext(DX12Device* device, RHICommandQueueType type)
    : m_device(device), m_queueType(type)
{
    auto d3dDevice = device->GetD3DDevice();
    // ...
    DX12_CHECK(d3dDevice->CreateCommandAllocator(listType, IID_PPV_ARGS(&m_commandAllocator)));
    DX12_CHECK(d3dDevice->CreateCommandList(0, listType, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    // ...
}
```

**修改方案**:

1. 在 `DX12Device` 中持有并初始化 Pool:

```cpp
// DX12Device.h
class DX12Device : public IRHIDevice
{
public:
    // ...
    DX12CommandAllocatorPool& GetCommandAllocatorPool() { return m_allocatorPool; }
    
private:
    DX12CommandAllocatorPool m_allocatorPool;
};
```

2. 修改 `DX12CommandContext` 构造函数:

```cpp
DX12CommandContext::DX12CommandContext(DX12Device* device, RHICommandQueueType type)
    : m_device(device), m_queueType(type)
{
    auto d3dDevice = device->GetD3DDevice();
    
    D3D12_COMMAND_LIST_TYPE listType;
    switch (type)
    {
        case RHICommandQueueType::Graphics: listType = D3D12_COMMAND_LIST_TYPE_DIRECT; break;
        case RHICommandQueueType::Compute:  listType = D3D12_COMMAND_LIST_TYPE_COMPUTE; break;
        case RHICommandQueueType::Copy:     listType = D3D12_COMMAND_LIST_TYPE_COPY; break;
        default: listType = D3D12_COMMAND_LIST_TYPE_DIRECT; break;
    }
    
    // 使用 Pool 获取 allocator，而不是直接创建
    m_commandAllocator = device->GetCommandAllocatorPool().Acquire(listType);
    DX12_CHECK(d3dDevice->CreateCommandList(0, listType, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    
    m_commandList->Close();
    m_isRecording = false;
}
```

3. 在 `DX12CommandContext` 析构或 `Begin()` 时归还 allocator:

```cpp
DX12CommandContext::~DX12CommandContext()
{
    // CommandList 释放时不需要特殊处理
    // Allocator 在 Release 回 pool 前需要知道 fence value
    // 因此改为在 Submit 时由调用方（或设备）负责归还
}
```

更合理的设计：在 `DX12Device::SubmitCommandContext()` 中，提交完成后将 allocator 归还 pool:

```cpp
void DX12Device::SubmitCommandContext(IRHICommandContext* ctx, IRHIFence* fence)
{
    auto* dx12Ctx = static_cast<DX12CommandContext*>(ctx);
    auto* dx12Fence = static_cast<DX12Fence*>(fence);
    
    // ... 实际提交到 queue ...
    
    // 提交后，将 allocator 标记为待归还（在 fence 完成后）
    uint64 signaledValue = dx12Fence ? dx12Fence->GetSignaledValue() : 0;
    m_allocatorPool.Release(dx12Ctx->GetCommandAllocator(), 
                            dx12Ctx->GetD3DListType(), 
                            signaledValue);
}
```

或者更简单：让 `DX12CommandContext` 在 `End()` 或析构时自行安排归还（如果已知 fence value）。需要权衡。

**涉及文件**:
- `RHI_DX12/Private/DX12CommandContext.h`
- `RHI_DX12/Private/DX12CommandContext.cpp`
- `RHI_DX12/Private/DX12Device.h`
- `RHI_DX12/Private/DX12Device.cpp`

---

### 1.4 修复 UploadTextureImmediate() 空壳实现

**问题**: `UploadTextureImmediate()` 创建 texture 资源后完全不写入数据，返回一个空的 texture。

**当前代码** (`GPUUploadService.cpp:377-397`):
```cpp
GPUUploadTextureResult GPUUploadService::UploadTextureImmediate(const GPUUploadTextureDesc& desc)
{
    auto texture = m_device->CreateTexture(desc.textureDesc);
    if (!texture)
    {
        return MakeTextureFailure(GPUUploadFailureReason::CreateResourceFailed);
    }

    // Compatibility fallback for backends or texture layouts that do not yet
    // have a staged-copy path in this service.
    m_stats.textureUploadCount++;
    m_stats.immediateUploadCount++;
    m_stats.uploadedBytes += desc.dataSize;

    GPUUploadTextureResult result;
    result.resource = texture;
    result.succeeded = true;
    result.mode = GPUUploadMode::ImmediateMapped;
    result.bytesUploaded = desc.dataSize;
    return result;
}
```

**修改方案**: 实现实际的 immediate texture upload。对于不支持 staged copy 的格式（compressed、mipmap 等），使用 Upload heap texture 或 Read/Write 方式。

DX12 方案（immediate fallback）:
```cpp
GPUUploadTextureResult GPUUploadService::UploadTextureImmediate(const GPUUploadTextureDesc& desc, const void* data)
{
    auto texture = m_device->CreateTexture(desc.textureDesc);
    if (!texture)
    {
        return MakeTextureFailure(GPUUploadFailureReason::CreateResourceFailed);
    }

    // 尝试通过 Map 直接写入（仅支持特定 layout 的 texture）
    // 或者：创建 upload buffer，用 graphics/compute context 做 CopyBufferToTexture
    
    // 方案 A：如果 RHI 支持 texture Map（如某些 OpenGL/Vulkan 路径）
    void* mapped = texture->Map(0, 0);  // subresource 0
    if (mapped)
    {
        // 计算 row pitch 和拷贝
        uint32_t rowPitch = desc.textureDesc.width * GetFormatBytesPerPixel(desc.textureDesc.format);
        uint32_t rows = desc.textureDesc.height;
        
        for (uint32_t row = 0; row < rows; ++row)
        {
            std::memcpy(static_cast<uint8_t*>(mapped) + row * rowPitch,
                       static_cast<const uint8_t*>(data) + row * rowPitch,
                       rowPitch);
        }
        texture->Unmap(0, 0);
    }
    else
    {
        // 方案 B：创建 upload buffer，用已有的 copy context 上传
        // 这需要 graphics queue 上的 context（copy queue 可能不支持某些 barrier/格式）
        // 或者记录到当前 persistent copy context 中并立即 flush
        
        RHIStagingBufferDesc stagingDesc;
        stagingDesc.size = desc.dataSize;
        stagingDesc.debugName = "TextureImmediateUpload";
        
        auto stagingBuffer = m_device->CreateStagingBuffer(stagingDesc);
        void* mappedBuf = stagingBuffer->Map(0, desc.dataSize);
        std::memcpy(mappedBuf, data, static_cast<size_t>(desc.dataSize));
        stagingBuffer->Unmap();
        
        // 使用 persistent copy context 或创建一个临时的
        auto ctx = m_device->CreateCommandContext(RHICommandQueueType::Copy);
        ctx->Begin();
        
        RHIBufferTextureCopyDesc copyDesc;
        copyDesc.bufferOffset = 0;
        copyDesc.bufferRowPitch = /* compute */;
        copyDesc.bufferImageHeight = desc.textureDesc.height;
        copyDesc.textureSubresource = 0;
        copyDesc.textureRegion = {0, 0, desc.textureDesc.width, desc.textureDesc.height};
        
        ctx->CopyBufferToTexture(stagingBuffer->GetBuffer(), texture.Get(), copyDesc);
        ctx->TextureBarrier(texture.Get(), RHIResourceState::CopyDest, RHIResourceState::Common);
        ctx->End();
        
        m_device->SubmitCommandContext(ctx.Get(), nullptr);
        m_device->WaitIdle();  // immediate = 同步等待
    }

    m_stats.textureUploadCount++;
    m_stats.immediateUploadCount++;
    m_stats.uploadedBytes += desc.dataSize;

    GPUUploadTextureResult result;
    result.resource = texture;
    result.succeeded = true;
    result.mode = GPUUploadMode::ImmediateMapped;
    result.bytesUploaded = desc.dataSize;
    return result;
}
```

**注意**: `UploadTextureImmediate` 的签名需要加上 `const void* data` 参数（当前缺少这个参数！）。

**涉及文件**:
- `Render/Include/Render/GPUUploadService.h`（修改接口签名）
- `Render/Private/GPUUploadService.cpp`

---

## Phase 2: P1 性能优化

### 2.1 StagingBuffer 池化

**问题**: 每个 staged upload 都调用 `m_device->CreateStagingBuffer()`，在 DX12 后端中这意味着 `CreateCommittedResource()` 一个 Upload Heap，上传完成后资源被丢弃。

**修改方案**: 在 `GPUUploadService` 中维护一个 StagingBuffer 池。

```cpp
class GPUUploadService
{
private:
    struct StagingBufferEntry
    {
        RHIStagingBufferRef buffer;
        uint64 size;
        uint64 lastUsedFrame;
    };
    
    std::vector<StagingBufferEntry> m_stagingBufferPool;
    static constexpr uint64 STAGING_BUFFER_MAX_AGE_FRAMES = 10;
    static constexpr uint64 STAGING_BUFFER_MAX_POOL_SIZE = 32;
    
    RHIStagingBufferRef AcquireStagingBuffer(uint64 size, const char* debugName);
    void ReleaseStagingBuffer(RHIStagingBufferRef buffer, uint64 size);
    void TrimStagingBufferPool();
};
```

```cpp
RHIStagingBufferRef GPUUploadService::AcquireStagingBuffer(uint64 size, const char* debugName)
{
    // 查找足够大且未在使用的 staging buffer
    for (auto& entry : m_stagingBufferPool)
    {
        if (entry.buffer && entry.size >= size && /* not in use */)
        {
            auto result = entry.buffer;
            entry.buffer = nullptr;  // mark as in-use
            return result;
        }
    }
    
    // 没有可用的，创建新的
    RHIStagingBufferDesc desc;
    desc.size = size;
    desc.debugName = debugName;
    return m_device->CreateStagingBuffer(desc);
}

void GPUUploadService::ReleaseStagingBuffer(RHIStagingBufferRef buffer, uint64 size)
{
    if (!buffer)
        return;
    
    // 尝试回收
    if (m_stagingBufferPool.size() < STAGING_BUFFER_MAX_POOL_SIZE)
    {
        StagingBufferEntry entry;
        entry.buffer = buffer;
        entry.size = size;
        entry.lastUsedFrame = m_currentFrame;
        m_stagingBufferPool.push_back(std::move(entry));
    }
    // 否则 buffer 随 Ref 释放
}
```

**简化方案**: 更实际的做法是使用一个大的环形 upload buffer（RingBuffer）。项目中已有 `DX12RingBuffer` 实现（`DX12Upload.cpp:160-360`），但未在 upload service 中使用。

评估：对于 2-16MB/帧 的上传量，RingBuffer 是更好的方案；对于偶尔的大纹理（>64MB），仍然需要独立的 StagingBuffer。推荐混合策略：
- 小数据（< 阈值）→ RingBuffer
- 大数据（> 阈值）→ 独立 StagingBuffer（可池化）

**涉及文件**:
- `Render/Private/GPUUploadService.cpp`
- `RHI/RHIUpload.h`（可能需要扩展接口）

---

### 2.2 Fence 复用池

**问题**: 每个 staged upload 创建一个 `IRHIFence`。Fence 是内核级同步对象，创建开销较高。

**修改方案**: 在 `GPUUploadService` 中实现 Fence 池。

```cpp
class GPUUploadService
{
private:
    std::vector<RHIFenceRef> m_availableFences;
    std::vector<std::pair<RHIFenceRef, uint64>> m_pendingFences;  // fence + expected value
    
    RHIFenceRef AcquireFence();
    void ReleaseFence(RHIFenceRef fence, uint64 completedValue);
};
```

```cpp
RHIFenceRef GPUUploadService::AcquireFence()
{
    // 回收已完成的 fence
    for (auto it = m_pendingFences.begin(); it != m_pendingFences.end();)
    {
        if (it->first->GetCompletedValue() >= it->second)
        {
            m_availableFences.push_back(it->first);
            it = m_pendingFences.erase(it);
        }
        else
        {
            ++it;
        }
    }
    
    if (!m_availableFences.empty())
    {
        auto fence = m_availableFences.back();
        m_availableFences.pop_back();
        return fence;
    }
    
    return m_device->CreateFence(0);
}

void GPUUploadService::ReleaseFence(RHIFenceRef fence, uint64 completedValue)
{
    m_pendingFences.push_back({fence, completedValue});
}
```

配合 1.2 的 batch flush：一个 batch 只需要一个 fence。单个 batch 内的所有 upload 共享同一个 fence value。

**涉及文件**:
- `Render/Private/GPUUploadService.cpp`

---

### 2.3 修复时间预算计算不精确

**问题**: `ProcessPendingUploads()` 的时间预算只检查 while 循环每次迭代开始时的耗时，但 `UploadMesh()` 内部执行了多个 buffer 创建和上传操作，这些耗时没有被实时检查。

**当前代码** (`GPUResourceManager.cpp:200-206`):
```cpp
while (!m_pendingQueue.empty())
{
    auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
    float elapsedMs = std::chrono::duration<float, std::milli>(elapsed).count();
    if (elapsedMs > timeBudgetMs)
        break;
    
    PendingUpload upload = m_pendingQueue.top();
    m_pendingQueue.pop();
    
    if (auto* mesh = dynamic_cast<Resource::MeshResource*>(upload.resource))
    {
        UploadMesh(mesh);  // ← 这个函数可能耗时远超剩余 budget
    }
    // ...
}
```

**修改方案**:

1. 在 `UploadMesh` 内部增加细粒度 budget 检查点，或在调用前估算耗时；
2. 更实际的方案：按 buffer 粒度检查 budget（因为每个 buffer 上传是独立操作）。

```cpp
void GPUResourceManager::ProcessPendingUploads(float timeBudgetMs)
{
    // ...
    auto startTime = std::chrono::high_resolution_clock::now();
    
    while (!m_pendingQueue.empty())
    {
        auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
        float elapsedMs = std::chrono::duration<float, std::milli>(elapsed).count();
        if (elapsedMs > timeBudgetMs)
            break;
        
        PendingUpload upload = m_pendingQueue.top();
        m_pendingQueue.pop();
        
        // 检查单个资源是否能在剩余时间内完成（简单启发式）
        float remainingMs = timeBudgetMs - elapsedMs;
        if (!CanUploadWithinBudget(upload.resource, remainingMs))
        {
            // 放回去，下帧处理
            m_pendingQueue.push(upload);
            break;
        }
        
        // 执行上传...
    }
}
```

**涉及文件**:
- `Render/Private/GPUResourceManager.cpp`

---

### 2.4 优化 UpdateCompletedResourceUploads() 全量扫描

**问题**: 每帧遍历所有 mesh 和 texture 的 GPUData，检查是否有 pending upload 已完成。时间复杂度 O(N)，其中 N 是总 resident 资源数。

**当前代码** (`GPUResourceManager.cpp:635-689`):
```cpp
void GPUResourceManager::UpdateCompletedResourceUploads()
{
    for (auto& [id, data] : m_meshGPUData)
    {
        if (data.isResident || data.pendingUploadIds.empty())
            continue;  // 大部分资源走这个分支，但仍然访问了每个 entry
        // ...
    }
}
```

**修改方案**: 维护一个独立的 "waiting for upload completion" 集合。

```cpp
class GPUResourceManager
{
private:
    // 只包含真正有 pending upload 的资源
    std::unordered_set<Resource::ResourceId> m_pendingResources;
};
```

在 `UploadMesh()` / `UploadTexture()` 中，如果有 pending upload id，加入集合：
```cpp
if (!gpuData.pendingUploadIds.empty())
{
    m_pendingResources.insert(meshRes->GetId());
}
```

在 `UpdateCompletedResourceUploads()` 中只扫描这个集合：
```cpp
void GPUResourceManager::UpdateCompletedResourceUploads()
{
    std::vector<Resource::ResourceId> completed;
    
    for (Resource::ResourceId id : m_pendingResources)
    {
        // 查找 mesh 或 texture...
        bool allComplete = CheckAllUploadsComplete(id);
        if (allComplete)
        {
            // 标记 resident
            completed.push_back(id);
        }
    }
    
    for (Resource::ResourceId id : completed)
    {
        m_pendingResources.erase(id);
    }
}
```

这样从 O(总资源数) 降到 O(正在上传的资源数)。

**涉及文件**:
- `Render/Private/GPUResourceManager.cpp`
- `Render/Include/Render/GPUResourceManager.h`

---

### 2.5 修正 PendingUpload operator< 的误导性注释

**问题**: 注释说 "Lower priority value = higher priority in queue"，但 `std::priority_queue` 默认是最大堆，`operator<` 返回 true 表示 this 的优先级更低。所以实际上是 **Higher priority value = higher priority in queue**。

```cpp
// 当前（注释错误）
bool operator<(const PendingUpload& other) const
{
    // Lower priority value = higher priority in queue  ← 错误！
    return static_cast<int>(priority) < static_cast<int>(other.priority);
}
```

正确的注释应该是：
```cpp
bool operator<(const PendingUpload& other) const
{
    // std::priority_queue is a max-heap by default.
    // operator< returning true means "this has lower priority than other".
    // Therefore higher numeric priority value = earlier execution.
    return static_cast<int>(priority) < static_cast<int>(other.priority);
}
```

**涉及文件**:
- `Render/Include/Render/GPUResourceManager.h`

---

## Phase 3: P2 功能扩展

### 3.1 扩展纹理异步上传路径

**问题**: `TryUploadTextureStaged()` 目前仅支持 2D texture、单 mip、单 array slice。Cubemap、3D texture、Mipmap chain 等 fallback 到 `UploadTextureImmediate()`，而后者目前是空壳。

**当前限制** (`GPUUploadService.cpp:279-298`):
```cpp
if (desc.textureDesc.dimension != RHITextureDimension::Texture2D ||
    desc.textureDesc.mipLevels != 1 || desc.textureDesc.arraySize != 1 ||
    bytesPerPixel == 0)
{
    return unsupported;  // fallback to immediate
}
```

**修改方案**: 扩展 staged copy 支持：

1. **Mipmap chain**: 计算每个 mip 的 offset 和 row pitch，在 staging buffer 中按顺序排列所有 mip 数据，用 `CopyBufferToTexture` 的多个 subresource 拷贝。
2. **Cubemap/Array**: 遍历每个 array slice 和 face，分别拷贝。
3. **Compressed format (BC1/BC3/BC5/BC7)**: 压缩格式的 staged copy 需要按 block 大小计算行 pitch（4x4 block），不能使用 `GetFormatBytesPerPixel()`。

DX12 具体实现需要使用 `CopyTextureRegion` with `D3D12_SUBRESOURCE_FOOTPRINT` 或 `D3D12_TEXTURE_COPY_LOCATION` 来正确处理 mip/array。

由于涉及 RHI 抽象层的设计（`CopyBufferToTexture` 接口是否支持 subresource 数组），建议分两步：
- **Step 1**: 在 DX12 backend 中直接实现，绕过 RHI 限制
- **Step 2**: 将通用接口扩展到 RHI，其他 backend 跟进

**涉及文件**:
- `Render/Private/GPUUploadService.cpp`
- `RHI/RHICommandContext.h`（可能需要扩展 CopyBufferToTexture 接口）
- `RHI_DX12/Private/DX12CommandContext.cpp`

---

### 3.2 引入 RingBuffer 用于高频小数据上传

**现状**: `DX12RingBuffer` 已完整实现（`DX12Upload.cpp:160-360`），支持 lock-free allocate、per-frame fence-based reset。但没有被任何上传路径使用。

**适用场景**:
- 每帧的动态 uniform buffer 更新
- 小 texture（< 1MB）的异步上传
- 粒子数据、instance data 的 GPU 推送

**集成方案**:

在 `GPUUploadService` 中维护一个 per-frame ring buffer：

```cpp
class GPUUploadService
{
private:
    RHIRingBufferRef m_ringBuffer;
    static constexpr uint64 RING_BUFFER_SIZE = 64 * 1024 * 1024;  // 64MB
};
```

对于小 buffer 上传（< 256KB），使用 ring buffer 代替独立 staging buffer：

```cpp
GPUUploadBufferResult GPUUploadService::UploadBufferDataWithResult(...)
{
    // 小数据走 ring buffer 直接路径
    if (dataSize < 256 * 1024 && m_ringBuffer)
    {
        auto alloc = m_ringBuffer->Allocate(dataSize);
        if (alloc.cpuAddress)
        {
            std::memcpy(alloc.cpuAddress, data, dataSize);
            // 使用 ring buffer 的 GPU 地址进行 copy
            // ...
        }
    }
    
    // 大数据走独立 staging buffer
    // ...
}
```

**涉及文件**:
- `Render/Private/GPUUploadService.cpp`
- `RHI/RHIUpload.h`

---

### 3.3 增强 Upload Metrics

**现状**: `Stats` 只有基本的 count 和 bytes，缺乏时间维度的指标。

**扩展指标**:
```cpp
struct Stats
{
    // 现有指标...
    uint32 bufferUploadCount = 0;
    uint32 textureUploadCount = 0;
    // ...
    
    // 新增时间/性能指标
    float lastFrameUploadTimeMs = 0.0f;       // 上一帧 ProcessPendingUploads 耗时
    float averageUploadTimeMs = 0.0f;         // 滑动平均
    uint32 peakPendingCount = 0;              // 历史峰值 pending 数量
    uint64 peakStagingBytesInFlight = 0;      // 历史峰值 staging 内存
    float uploadBandwidthMBps = 0.0f;         // 有效上传带宽
    uint32 batchCount = 0;                    // 每帧 batch 数量（配合 1.2）
    uint32 resourcesSkippedRendering = 0;     // 因未就绪被跳过的资源数（配合 1.1）
};
```

**涉及文件**:
- `Render/Include/Render/GPUUploadService.h`
- `Render/Include/Render/GPUResourceManager.h`

---

## 实施路线图

### Sprint 1: 阻塞问题消除（预计 2-3 天）

| 天数 | 任务 | 验证方式 |
|------|------|---------|
| Day 1 | 实现 1.1: 移除 `EnsureVisibleResourcesResident` 同步上传，改为 async queue | 加载场景时帧时间不应有 >5ms 的尖峰 |
| Day 1 | 修改渲染 pass 跳过未就绪资源 | 验证未就绪 mesh 正确跳过，不 crash |
| Day 2 | 实现 1.2: Persistent Copy CommandContext + Batch Flush | 验证 10 个 mesh 上传只创建 1 个 context |
| Day 2 | 实现 1.4: 修复 `UploadTextureImmediate` 空壳 | 单元测试：上传 texture 后 GPU 数据正确 |
| Day 3 | 实现 1.3: CommandAllocatorPool 集成 | 验证 pool 统计数字 > 0，context 复用 |

### Sprint 2: 资源池化优化（预计 2 天）

| 天数 | 任务 | 验证方式 |
|------|------|---------|
| Day 4 | 实现 2.1: StagingBuffer 池 | 验证多次上传后 CreateStagingBuffer 调用次数减少 |
| Day 4 | 实现 2.2: Fence 复用池 | 验证 pool 统计数字 |
| Day 5 | 实现 2.3 + 2.4: Time budget 精确化 + 扫描优化 | Profile 对比 |
| Day 5 | 实现 2.5: 修正注释 | Code review |

### Sprint 3: 纹理与高级功能（预计 3-4 天）

| 天数 | 任务 | 验证方式 |
|------|------|---------|
| Day 6-7 | 实现 3.1: 纹理 staged upload 扩展（mip、array、cubemap） | 加载含 mipmap 的纹理 |
| Day 8 | 评估 3.2: RingBuffer 集成（如需） | Benchmark 对比 |
| Day 8-9 | 实现 3.3: 增强 metrics + 集成到 Debug overlay | ImGui 面板显示 upload stats |

---

## 风险与回滚策略

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 异步上传导致首次渲染时大量资源缺失 | 画面闪烁/物体消失 | 在加载屏幕阶段使用 `UploadImmediate` 预加载核心资源；同时提高 async 优先级和时间预算 |
| Batch flush 延迟导致 staging buffer 内存峰值 | OOM | 设置 batch 大小上限（如 max 32MB/ batch），超限时提前 flush |
| CommandAllocatorPool 集成引入 allocator 状态 bug | GPU validation error / crash | 渐进式：先在 Copy queue 启用，稳定后扩展到 Direct/Compute；使用 D3D12 Debug Layer 验证 |
| RingBuffer 空间不足 | 上传失败 | fallback 到独立 staging buffer |

---

## 验证策略

### 单元测试

```cpp
// Test 1: 异步上传不阻塞
TEST(GPUUploadService, AsyncUploadDoesNotBlock)
{
    auto start = Clock::Now();
    service.RequestUpload(mesh, UploadPriority::Normal);
    service.ProcessPendingUploads(16.0f);  // 16ms budget
    auto elapsed = Clock::Now() - start;
    EXPECT_LT(elapsed, 1ms);  // 不应阻塞
}

// Test 2: Batch 只创建一个 context
TEST(GPUUploadService, BatchUsesSingleContext)
{
    uint32 initialContextCount = device->GetStats().commandContextCount;
    
    // 上传 5 个 buffer
    for (int i = 0; i < 5; ++i)
    {
        service.UploadBufferData(desc, data, size);
    }
    service.FlushBatchUploads();
    
    uint32 newContextCount = device->GetStats().commandContextCount;
    EXPECT_EQ(newContextCount - initialContextCount, 1);  // 只创建 1 个
}

// Test 3: Texture immediate 实际写入数据
TEST(GPUUploadService, TextureImmediateWritesData)
{
    auto result = service.UploadTextureDataWithResult(desc, pixelData);
    EXPECT_TRUE(result);
    
    // 读取回 CPU 验证（如果 backend 支持）
    auto readback = ReadbackTexture(result.resource);
    EXPECT_EQ(memcmp(readback.data(), pixelData, dataSize), 0);
}
```

### Profile 检查点

- **Before/After 帧时间**: 使用 GPU Profiler 测量 `RenderSubsystem::Render()` 的 CPU 耗时
- **Staging 内存占用**: 监控 `GPUUploadService::GetStats().stagingBytesInFlight`
- **Upload queue 深度**: 监控 `pendingUploadCount` 确保不无限增长
- **资源就绪延迟**: 从 `RequestUpload` 到 `IsGPUReady` 的平均帧数

---

## 相关文件索引

| 文件 | 角色 | 涉及优化项 |
|------|------|-----------|
| `Render/Private/RenderSubsystem.cpp` | 渲染子系统，调度上传 | 1.1 |
| `Render/Private/GPUResourceManager.cpp` | 资源生命周期管理 | 1.1, 2.3, 2.4, 2.5 |
| `Render/Include/Render/GPUResourceManager.h` | ResourceManager 接口 | 2.5 |
| `Render/Private/GPUUploadService.cpp` | 底层上传服务 | 1.2, 1.4, 2.1, 2.2, 3.1, 3.2 |
| `Render/Include/Render/GPUUploadService.h` | UploadService 接口 | 1.2, 1.4, 3.3 |
| `RHI_DX12/Private/DX12CommandContext.cpp` | DX12 CommandContext | 1.3 |
| `RHI_DX12/Private/DX12CommandContext.h` | CommandContext 定义 | 1.3 |
| `RHI_DX12/Private/DX12CommandAllocatorPool.cpp` | Allocator 池（未使用） | 1.3 |
| `RHI_DX12/Private/DX12CommandAllocatorPool.h` | Allocator 池定义 | 1.3 |
| `RHI_DX12/Private/DX12Device.cpp` | DX12 设备 | 1.3 |
| `RHI_DX12/Private/DX12Upload.cpp` | DX12 Upload 实现 | 3.2 |
| `Engine/Private/Engine.cpp` | 引擎主循环 | 1.1, 1.2（调用时序） |
| `Render/Private/Renderer/SceneRenderer.cpp` | 场景渲染器 | 1.1（跳过未就绪资源） |
