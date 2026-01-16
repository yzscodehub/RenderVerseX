# RenderGraph 优化计划

## 概述

本文档记录 RenderGraph 模块的优化计划和实现状态。

---

## ✅ 阶段一：资源别名系统 (Memory Aliasing) - 已完成

### 实现状态

| 功能 | 状态 | 文件 |
|------|------|------|
| ResourceLifetime 结构 | ✅ 完成 | `RenderGraphInternal.h` |
| MemoryAlias 结构 | ✅ 完成 | `RenderGraphInternal.h` |
| TransientHeap 结构 | ✅ 完成 | `RenderGraphInternal.h` |
| CalculateResourceLifetimes() | ✅ 完成 | `RenderGraphCompiler.cpp` |
| ComputeMemoryAliases() | ✅ 完成 | `RenderGraphCompiler.cpp` |
| 统计信息扩展 | ✅ 完成 | `RenderGraph.h` |
| 生命周期基于执行顺序 | ✅ 完成 | `RenderGraphCompiler.cpp` |
| 统计数据编译前重置 | ✅ 完成 | `RenderGraphCompiler.cpp` |
| RHI Heap 接口 | ✅ 完成 | `RHI/RHIHeap.h`, `RHI/RHIDevice.h` |
| DX12 Heap 实现 | ✅ 完成 | `DX12Resources.cpp` |
| Vulkan Heap 实现 | ✅ 完成 | `VulkanResources.cpp` |
| Placed Resource 创建 | ✅ 完成 | `RenderGraphCompiler.cpp` |
| Aliasing Barrier 跟踪 | ✅ 完成 | `RenderGraphInternal.h`, `RenderGraphCompiler.cpp` |
| Graphviz DOT 导出 | ✅ 完成 | `RenderGraph.cpp` |

### ✅ 已完成：Placed Resource 支持

以下所有步骤已实现完成：

#### 1. RHI 接口扩展 ✅

```cpp
// RHI/RHIDevice.h - 添加以下接口

// 创建 Heap（用于 Placed Resource）
struct RHIHeapDesc
{
    uint64 size = 0;
    RHIHeapType type = RHIHeapType::Default;  // Default, Upload, Readback
    RHIHeapFlags flags = RHIHeapFlags::AllowAllResources;
    const char* debugName = nullptr;
};

class RHIHeap : public RefCounted
{
public:
    virtual uint64 GetSize() const = 0;
    virtual RHIHeapType GetType() const = 0;
};
using RHIHeapRef = Ref<RHIHeap>;

// IRHIDevice 新增方法
class IRHIDevice
{
public:
    // ... 现有方法 ...
    
    // Heap 管理
    virtual RHIHeapRef CreateHeap(const RHIHeapDesc& desc) = 0;
    
    // Placed Resource 创建
    virtual RHITextureRef CreatePlacedTexture(
        RHIHeap* heap, 
        uint64 offset, 
        const RHITextureDesc& desc) = 0;
    
    virtual RHIBufferRef CreatePlacedBuffer(
        RHIHeap* heap, 
        uint64 offset, 
        const RHIBufferDesc& desc) = 0;
    
    // Aliasing Barrier（当切换使用同一内存的不同资源时）
    virtual void AliasingBarrier(
        RHICommandContext* ctx,
        RHITexture* before,
        RHITexture* after) = 0;
};
```

#### 2. DX12 实现 ✅

```cpp
// RHI_DX12/Private/DX12Device.cpp

class DX12Heap : public RHIHeap
{
public:
    ComPtr<ID3D12Heap> m_heap;
    uint64 m_size;
    // ...
};

RHIHeapRef DX12Device::CreateHeap(const RHIHeapDesc& desc)
{
    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = desc.size;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;  // 64KB
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    
    ComPtr<ID3D12Heap> heap;
    DX12_CHECK(m_device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)));
    
    auto result = new DX12Heap();
    result->m_heap = heap;
    result->m_size = desc.size;
    return RHIHeapRef(result);
}

RHITextureRef DX12Device::CreatePlacedTexture(
    RHIHeap* heap, 
    uint64 offset, 
    const RHITextureDesc& desc)
{
    auto* dx12Heap = static_cast<DX12Heap*>(heap);
    
    D3D12_RESOURCE_DESC resourceDesc = {};
    // ... 填充 resourceDesc ...
    
    ComPtr<ID3D12Resource> resource;
    DX12_CHECK(m_device->CreatePlacedResource(
        dx12Heap->m_heap.Get(),
        offset,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource)));
    
    return CreateDX12TextureFromResource(this, resource, desc);
}
```

#### 3. Vulkan 实现 ✅

```cpp
// RHI_Vulkan/Private/VulkanDevice.cpp

class VulkanHeap : public RHIHeap
{
public:
    VkDeviceMemory m_memory;
    uint64 m_size;
    uint32 m_memoryTypeIndex;
    // ...
};

RHIHeapRef VulkanDevice::CreateHeap(const RHIHeapDesc& desc)
{
    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = desc.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    VkDeviceMemory memory;
    VK_CHECK(vkAllocateMemory(m_device, &allocInfo, nullptr, &memory));
    
    auto result = new VulkanHeap();
    result->m_memory = memory;
    result->m_size = desc.size;
    return RHIHeapRef(result);
}

RHITextureRef VulkanDevice::CreatePlacedTexture(
    RHIHeap* heap, 
    uint64 offset, 
    const RHITextureDesc& desc)
{
    auto* vkHeap = static_cast<VulkanHeap*>(heap);
    
    // 1. 创建 VkImage（不分配内存）
    VkImageCreateInfo imageInfo = {};
    // ... 填充 imageInfo ...
    
    VkImage image;
    VK_CHECK(vkCreateImage(m_device, &imageInfo, nullptr, &image));
    
    // 2. 绑定到共享内存
    VK_CHECK(vkBindImageMemory(m_device, image, vkHeap->m_memory, offset));
    
    return CreateVulkanTextureFromImage(this, image, desc);
}
```

#### 4. RenderGraph 资源创建修改 ✅

```cpp
// RenderGraph/Private/RenderGraphCompiler.cpp

void CreateTransientResources(RenderGraphImpl& graph)
{
    if (!graph.device || !graph.enableMemoryAliasing)
    {
        // 回退到独立资源创建
        CreateIndependentResources(graph);
        return;
    }
    
    // 1. 创建 Transient Heaps
    std::vector<RHIHeapRef> heaps;
    heaps.reserve(graph.transientHeaps.size());
    
    for (auto& th : graph.transientHeaps)
    {
        RHIHeapDesc heapDesc;
        heapDesc.size = th.size;
        heapDesc.type = RHIHeapType::Default;
        
        auto heap = graph.device->CreateHeap(heapDesc);
        heaps.push_back(heap);
        th.platformHeap = heap.Get();
    }
    
    // 2. 创建 Placed Textures
    for (auto& texture : graph.textures)
    {
        if (texture.imported || texture.texture)
            continue;
        
        if (texture.alias.heapIndex != UINT32_MAX)
        {
            auto* heap = heaps[texture.alias.heapIndex].Get();
            texture.texture = graph.device->CreatePlacedTexture(
                heap,
                texture.alias.heapOffset,
                texture.desc);
        }
        else
        {
            texture.texture = graph.device->CreateTexture(texture.desc);
        }
    }
    
    // 3. 创建 Placed Buffers
    for (auto& buffer : graph.buffers)
    {
        if (buffer.imported || buffer.buffer)
            continue;
        
        if (buffer.alias.heapIndex != UINT32_MAX)
        {
            auto* heap = heaps[buffer.alias.heapIndex].Get();
            buffer.buffer = graph.device->CreatePlacedBuffer(
                heap,
                buffer.alias.heapOffset,
                buffer.desc);
        }
        else
        {
            buffer.buffer = graph.device->CreateBuffer(buffer.desc);
        }
    }
}
```

#### 5. Aliasing Barrier 插入 ✅

当同一 Heap 位置被不同资源使用时，需要插入 Aliasing Barrier：

```cpp
// 在 ExecuteRenderGraph 中，当切换到使用别名资源时
void InsertAliasingBarriers(RenderGraphImpl& graph, Pass& pass, RHICommandContext& ctx)
{
    for (const auto& usage : pass.usages)
    {
        if (usage.type == ResourceType::Texture)
        {
            auto& resource = graph.textures[usage.index];
            if (!resource.alias.isAliased)
                continue;
            
            // 检查是否需要 Aliasing Barrier
            // （当前资源首次使用，且同一内存位置之前被其他资源使用）
            if (IsFirstUseInCurrentFrame(resource, pass))
            {
                auto* previousResource = FindPreviousResourceAtSameLocation(
                    graph, resource.alias);
                if (previousResource)
                {
                    ctx.AliasingBarrier(previousResource, resource.texture.Get());
                }
            }
        }
    }
}
```
#### 6. 生命周期计算修正（按执行顺序） ✅

当前 `CalculateResourceLifetimes()` 使用 Pass 插入顺序，若执行顺序被拓扑排序重排，会导致生命周期区间错误。应改为基于 `executionOrder` 计算：

```cpp
// RenderGraph/Private/RenderGraphCompiler.cpp
// 用 executionOrder 序号作为“时间轴”
void CalculateResourceLifetimes(RenderGraphImpl& graph)
{
    // Reset lifetimes
    for (auto& texture : graph.textures)
    {
        texture.lifetime = ResourceLifetime{};
        texture.alias = MemoryAlias{};
    }
    for (auto& buffer : graph.buffers)
    {
        buffer.lifetime = ResourceLifetime{};
        buffer.alias = MemoryAlias{};
    }

    // executionOrder 为空则回退到插入顺序
    if (graph.executionOrder.empty())
    {
        for (uint32 passIndex = 0; passIndex < graph.passes.size(); ++passIndex)
        {
            // 旧逻辑
        }
        return;
    }

    for (uint32 order = 0; order < graph.executionOrder.size(); ++order)
    {
        uint32 passIndex = graph.executionOrder[order];
        const auto& pass = graph.passes[passIndex];
        if (pass.culled) continue;

        for (const auto& usage : pass.usages)
        {
            // 将 order 视为 first/lastUse
        }
    }
}
```

#### 7. 统计数据编译前重置 ✅

避免多次 `Compile()` 叠加统计数据：

```cpp
// RenderGraph/Private/RenderGraphCompiler.cpp
void CompileRenderGraph(RenderGraphImpl& graph)
{
    graph.stats = {};
    graph.totalMemoryWithoutAliasing = 0;
    graph.totalMemoryWithAliasing = 0;
    graph.aliasedTextureCount = 0;
    graph.aliasedBufferCount = 0;
    // ...
}
```

---

## ⏳ 阶段二：Split Barriers 支持

### 目标

将 Barrier 拆分为 Begin/End，让 GPU 在等待期间可以执行其他工作。

### 实现计划

#### 1. RHI 接口扩展

```cpp
// RHI/RHICommandContext.h

enum class RHIBarrierType
{
    Immediate,      // 传统同步 Barrier
    BeginOnly,      // 异步 Barrier 开始
    EndOnly,        // 异步 Barrier 结束
};

struct RHITextureBarrier
{
    RHITexture* texture;
    RHIResourceState stateBefore;
    RHIResourceState stateAfter;
    RHISubresourceRange subresourceRange;
    RHIBarrierType type = RHIBarrierType::Immediate;  // 新增
};

// DX12: 使用 Enhanced Barriers (D3D12_BARRIER_SYNC_SPLIT)
// Vulkan: 分离的 srcStageMask/dstStageMask
```

#### 2. RenderGraph 扩展

```cpp
// RenderGraphInternal.h

struct Pass
{
    // ... 现有字段 ...
    
    // Split Barriers
    std::vector<RHITextureBarrier> beginBarriers;   // Pass 开始前发起
    std::vector<RHITextureBarrier> endBarriers;     // 之前 Pass 的结束 Barrier
};
```

#### 3. 编译时分析

```cpp
// RenderGraphCompiler.cpp

void AnalyzeSplitBarriers(RenderGraphImpl& graph)
{
    // 对于每个 Barrier：
    // 1. 找到资源的最后使用 Pass (fromPass)
    // 2. 找到资源的下次使用 Pass (toPass)
    // 3. 如果 toPass - fromPass > 1，可以拆分
    
    for (uint32 toPassIdx : graph.executionOrder)
    {
        auto& toPass = graph.passes[toPassIdx];
        
        for (auto& barrier : toPass.textureBarriers)
        {
            uint32 fromPassIdx = FindLastWritePass(graph, barrier.texture);
            
            if (fromPassIdx != UINT32_MAX && toPassIdx - fromPassIdx > 1)
            {
                // 可以拆分：在 fromPass 结束时 Begin，在 toPass 开始时 End
                auto& fromPass = graph.passes[fromPassIdx];
                
                RHITextureBarrier beginBarrier = barrier;
                beginBarrier.type = RHIBarrierType::BeginOnly;
                fromPass.beginBarriers.push_back(beginBarrier);
                
                barrier.type = RHIBarrierType::EndOnly;
            }
        }
    }
}
```

---

## ⏳ 阶段三：异步计算队列支持

### 目标

让独立的 Compute Pass 在异步队列上并行执行。

### 实现计划

#### 1. Pass 队列偏好

```cpp
// RenderGraph.h

template<typename Data>
void AddPass(
    const char* name,
    RenderGraphPassType type,
    RHICommandQueueType preferredQueue,  // 新增
    std::function<void(RenderGraphBuilder&, Data&)> setup,
    std::function<void(const Data&, RHICommandContext&)> execute);
```

#### 2. Pass 分组和调度

```cpp
// RenderGraphInternal.h

struct PassGroup
{
    RHICommandQueueType queue;
    std::vector<uint32> passIndices;
    
    // 队列同步点
    uint32 waitForGraphicsPass = UINT32_MAX;  // 等待 Graphics 队列
    uint32 signalToGraphicsPass = UINT32_MAX; // 通知 Graphics 队列
};

// RenderGraphCompiler.cpp

std::vector<PassGroup> ScheduleAsyncPasses(RenderGraphImpl& graph)
{
    std::vector<PassGroup> groups;
    
    // 1. 识别可以异步执行的 Compute Pass
    // 2. 分析依赖关系，确定同步点
    // 3. 生成 Pass 组
    
    return groups;
}
```

#### 3. 执行时同步

```cpp
// RenderGraphExecutor.cpp

void ExecuteWithAsyncCompute(
    RenderGraphImpl& graph,
    RHICommandContext& graphicsCtx,
    RHICommandContext& computeCtx,
    RHIFence* fence)
{
    uint64 fenceValue = 0;
    
    for (const auto& group : graph.passGroups)
    {
        // 等待依赖
        if (group.waitForGraphicsPass != UINT32_MAX)
        {
            computeCtx.WaitForFence(fence, fenceValue);
        }
        
        // 执行 Pass 组
        auto& ctx = (group.queue == RHICommandQueueType::Compute) 
            ? computeCtx : graphicsCtx;
        
        for (uint32 passIdx : group.passIndices)
        {
            ExecutePass(graph, passIdx, ctx);
        }
        
        // 发出信号
        if (group.signalToGraphicsPass != UINT32_MAX)
        {
            graphicsCtx.Signal(fence, ++fenceValue);
        }
    }
}
```

---

## ⏳ 阶段四：Lambda 内存优化

### 目标

减少 AddPass 时的堆分配。

### 实现计划

```cpp
// RenderGraphInternal.h

class RenderGraphArena
{
public:
    template<typename T, typename... Args>
    T* Allocate(Args&&... args)
    {
        size_t alignment = alignof(T);
        size_t size = sizeof(T);
        
        // 对齐偏移
        m_offset = (m_offset + alignment - 1) & ~(alignment - 1);
        
        if (m_offset + size > m_memory.size())
        {
            // 扩容
            m_memory.resize(m_memory.size() * 2 + size);
        }
        
        void* ptr = m_memory.data() + m_offset;
        m_offset += size;
        
        return new (ptr) T(std::forward<Args>(args)...);
    }
    
    void Reset()
    {
        // 调用所有析构函数
        for (auto& dtor : m_destructors)
        {
            dtor();
        }
        m_destructors.clear();
        m_offset = 0;
    }

private:
    std::vector<uint8> m_memory;
    size_t m_offset = 0;
    std::vector<std::function<void()>> m_destructors;
};

// RenderGraph.cpp

template<typename Data>
void RenderGraph::AddPass(...)
{
    Data* data = m_impl->arena.Allocate<Data>();
    // ...
}
```

---

## ⏳ 阶段五：Graphviz DOT 导出

### 目标

生成可视化的渲染图。

### 实现计划

```cpp
// RenderGraph.h

class RenderGraph
{
public:
    // ... 现有方法 ...
    
    // Debug 导出
    std::string ExportGraphviz() const;
    void SaveGraphviz(const char* filename) const;
};

// RenderGraph.cpp

std::string RenderGraph::ExportGraphviz() const
{
    std::ostringstream ss;
    ss << "digraph RenderGraph {\n";
    ss << "  rankdir=LR;\n";
    ss << "  node [shape=box, style=filled];\n";
    ss << "  edge [color=gray];\n\n";
    
    // 资源节点（椭圆形）
    ss << "  // Resources\n";
    for (size_t i = 0; i < m_impl->textures.size(); ++i)
    {
        const auto& tex = m_impl->textures[i];
        ss << "  tex" << i << " [shape=ellipse, label=\""
           << (tex.desc.debugName ? tex.desc.debugName : "Texture")
           << "\\n" << tex.desc.width << "x" << tex.desc.height
           << "\", fillcolor=" << (tex.imported ? "lightblue" : "lightgreen")
           << "];\n";
    }
    
    // Pass 节点（方形）
    ss << "\n  // Passes\n";
    for (size_t i = 0; i < m_impl->passes.size(); ++i)
    {
        const auto& pass = m_impl->passes[i];
        std::string color = pass.culled ? "gray" : 
            (pass.type == RenderGraphPassType::Compute ? "lightyellow" : "lightcoral");
        ss << "  pass" << i << " [label=\"" << pass.name 
           << "\", fillcolor=" << color << "];\n";
    }
    
    // 依赖边
    ss << "\n  // Dependencies\n";
    for (size_t i = 0; i < m_impl->passes.size(); ++i)
    {
        const auto& pass = m_impl->passes[i];
        for (uint32 texIdx : pass.readTextures)
        {
            ss << "  tex" << texIdx << " -> pass" << i << ";\n";
        }
        for (uint32 texIdx : pass.writeTextures)
        {
            ss << "  pass" << i << " -> tex" << texIdx << ";\n";
        }
    }
    
    ss << "}\n";
    return ss.str();
}
```

### 使用示例

```bash
# 生成 DOT 文件后，使用 Graphviz 渲染
dot -Tpng render_graph.dot -o render_graph.png
```

---

## 实施优先级

| 阶段 | 优先级 | 预期收益 | 复杂度 | 状态 |
|------|:------:|----------|:------:|:----:|
| 1. Memory Aliasing (算法) | 🔴 高 | 显存节省 30-50% | 中 | ✅ 完成 |
| 1. Memory Aliasing (RHI) | 🔴 高 | 实际生效 | 高 | ✅ 完成 |
| 1.5. Graphviz 导出 | 🟡 低 | 调试便利 | 低 | ✅ 完成 |
| 2. Split Barriers | 🟠 中 | GPU 利用率 +5-15% | 中 | ⏳ 待实现 |
| 3. Async Compute | 🟠 中 | GPU 利用率 +10-30% | 高 | ⏳ 待实现 |
| 4. Lambda 优化 | 🟡 低 | CPU 开销减少 | 低 | ⏳ 待实现 |

---

## 验收标准

### 阶段一（完整） ✅
- [x] RHI 支持 CreateHeap / CreatePlacedTexture / CreatePlacedBuffer
- [x] 生命周期计算基于执行顺序（拓扑排序后）
- [x] Aliasing Barrier 跟踪机制
- [x] Graphviz DOT 导出功能
- [ ] 相同场景显存占用降低 30%+（待性能验证）
- [ ] 无内存泄漏或访问冲突（待测试验证）

### 阶段二
- [ ] Split Barrier 正确插入
- [ ] GPU Timeline 显示 Barrier 与 Pass 执行重叠

### 阶段三
- [ ] Compute Pass 在异步队列执行
- [ ] 正确的队列间同步

### 阶段四
- [ ] 添加 100 个 Pass 时无堆分配
