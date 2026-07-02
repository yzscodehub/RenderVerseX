# RVX-NG · RHI 核心 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §7、决策 D4、契约 C6
**里程碑**：M2
**状态**：详细设计 · 待 S3 Spike

---

## 1. 目标与范围

**目标**：薄、显式的图形硬件抽象，把现代特性（bindless / timeline / indirect / AS / mesh shader / query / VRS / residency）作为**一等接口**统一暴露，后端 D3D12 / Vulkan / Metal3 **+ 主机原生（GNM/AGC、NVN）**接缝，能力差异**诚实分级**（C6）。

**范围内**：设备/适配器/能力、资源（buffer/texture/view）、队列与 timeline、命令录制、bindless 堆、管线（graphics/compute/RT/mesh）、加速结构、query、VRS、显存 residency、swapchain、barrier。
**范围外**：帧图编排（RenderGraph §04）、着色器编译（ShaderCompiler §03b）、高层渲染语义。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 抽象厚度 | 薄——贴近显式 API 形态，不过度虚化；每 draw/dispatch 录制开销极低 |
| Bindless | 全局描述符堆，注册即得 shader 索引，**无 per-draw 描述符绑定** |
| 同步 | timeline semaphore 跨队列；无隐式 barrier |
| 能力诚实 | caps 位与实现一致（CI 校验）；不支持即 false，不谎报 |
| 多后端 | 共享命令翻译基类，避免 N 份枚举映射重复 |
| 主机 | console 原生后端可接入而不漏原生类型 |

## 3. 公共接口面（设计契约）

```cpp
struct RhiCaps {                                  // 能力位，运行时可查，CI 校验一致性
    bool bindless, rayTracing, meshShader, vrs, asyncCompute, timeline;
    u32  maxBindlessDescriptors, maxRayRecursion;
    TierLevel resourceBindingTier; /* ... */
};

class IDevice {
    RhiCaps Caps() const;
    BufferHandle  CreateBuffer(const BufferDesc&);
    TextureHandle CreateTexture(const TextureDesc&);
    PipelineHandle CreateGraphicsPipeline(const GfxPipelineDesc&);
    PipelineHandle CreateComputePipeline(const ComputeDesc&);
    PipelineHandle CreateRtPipeline(const RtPipelineDesc&);     // caps.rayTracing 守门
    ASHandle       CreateAccelStruct(const ASDesc&);

    BindlessIndex  RegisterSRV(TextureView);  BindlessIndex RegisterUAV(...);  // → shader 索引
    void           Unregister(BindlessIndex);

    IQueue*        Queue(QueueType);          // Graphics/Compute/Copy
    ISwapchain*    CreateSwapchain(const SwapchainDesc&); // 含 HDR 输出格式
    void           SetResidencyPriority(ResourceHandle, Priority); // residency
};

class ICommandContext {                       // 录制式；可在任意 worker 录制
    void SetPipeline(PipelineHandle);
    void BindBindlessHeap();                   // 一次绑定全局堆
    void PushConstants(Span<const u8>);        // 索引经此/小 cbuffer 传入
    void DrawIndexedIndirect(BufferHandle args, u32 count, ...);
    void DispatchMesh(u32x3);  void Dispatch(u32x3);
    void DispatchRays(const DispatchRaysDesc&); // caps.rayTracing 守门
    void BuildAccelStruct(ASHandle dst, const ASBuildDesc&, BufferHandle scratch);
    void Barriers(Span<const BufferBarrier>, Span<const TextureBarrier>); // 显式批量
    void BeginQuery/EndQuery(QueryHandle);     // timestamp/occlusion/stats
};

class IQueue {
    u64  Submit(Span<ICommandContext*>, Span<const Wait>, Span<const Signal>); // timeline
    void WaitTimeline(u64 value);              // GPU 侧等待，不阻塞 CPU
};
```

**不变量**：能力守门优先于一切（不支持的特性调用 = 早返回 + WARN，绝不谎报）；bindless 索引在 `Unregister` 后失效；barrier 显式由调用方（RenderGraph）批量发起；命令录制线程安全（不同 context 不同线程），单 context 单线程。

## 4. 数据结构与内存布局

- **Bindless 堆**：单一大描述符堆（SRV/UAV/Sampler 分区或统一），`BindlessIndex` = 堆内 slot；分配器（freelist + 代际）管理 slot 复用。
- **资源句柄**：代际句柄（C4）→ 后端原生对象（ID3D12Resource / VkImage / MTLTexture）侧表。
- **命令翻译基类**：`CommandContextBase` 持公共状态/校验，后端覆写 `EmitDraw/EmitBarrier/...` —— 统一公共校验并减少后端重复映射。
- **timeline**：每队列一个单调递增 fence value；`Submit` 返回 signaled value。
- **indirect arg 布局**：标准化 `DrawIndexedIndirectArgs` struct（与 GpuScene/Culling 产出的 GPU buffer 对齐）。

## 5. 核心算法

- **Barrier 批处理**：调用方一次提交多个 barrier；后端合并/去冗余（split barrier 可用时用）。
- **Bindless 分配**：`RegisterSRV` → freelist 取 slot → 写描述符 → 返回 index；`Unregister` → 代际 bump + 归还。
- **AS 构建**：`BuildAccelStruct` 录制（prebuild size 查询 → scratch → build → UAV barrier），DXR/VK_KHR_ray_tracing 对齐。
- **能力分级**：`Caps()` 来自真实查询（D3D12_OPTIONS / VK feature struct / Metal family）；不支持的路径在更高层（RenderGraph/FrameRenderer）按 tier 降级。
- **residency**：VRAM 预算 + MakeResident/Evict（与 Asset streaming §05 联动；overcommit 时按 priority 逐出）。

## 6. 线程与内存模型

- **录制**：多 worker 并行录制多个 `ICommandContext`（C9 渲染阶段）。
- **提交**：`Submit` 在 render 阶段；`Present` 按平台要求（主线程或 submit 线程，经 JobSystem `RunOnMainThread`）。
- **跨队列同步**：timeline semaphore（GPU 侧 wait，CPU 不阻塞）。
- **资源内存**：placed resource + 显式堆（支持 RenderGraph 瞬态别名）；上传走 staging ring（每帧轮换）。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 调用不支持特性 | caps 守门 → 早返回 + WARN（诚实，绝不假装成功） |
| device removed（TDR） | 检测 → 上报崩溃管线 + 尝试设备重建（或安全退出） |
| VRAM OOM | residency 逐出（按 priority）→ 重试；失败 → 降质量档/拒绝 |
| bindless 堆满 | 达上限 → 错误（上层应分级降资源数） |
| 无效句柄/代际失效 | Debug 断言；Release 早返回 |
| 命令录制契约违反 | Debug 校验层（如 barrier 缺失、状态不符）报错 |

**禁止**：谎报能力位（C6）；隐式 barrier；跨 context 共享录制状态。

## 8. 性能目标

- `DrawIndexedIndirect` 录制 < 50 ns（无描述符绑定，bindless）。
- 一次 `Submit` 多 context，跨队列 timeline 无 CPU 阻塞。
- barrier 批处理：合并后 barrier 数 ≤ 朴素的 50%。
- bindless register/unregister < 100 ns。

## 9. 测试计划

- **跨后端 golden**：同一渲染输入，D3D12/Vulkan/Metal3 逐像素一致（硬门禁，designated GPU 实跑）。
- **能力一致性门禁**：每后端 `Caps()` 位 == 实际可执行的特性（注入调用验证，防谎报回归）。
- **单元**：bindless 分配/失效、barrier 批处理正确、timeline 语义、AS 构建。
- **headless/参考设备**：CI 无 GPU 环境下的 API 契约测试（WARP/lavapipe）。
- device-removed 注入、VRAM OOM 注入路径。

## 10. 开放问题 / Spike

- **主机后端**：GNM/AGC、NVN 的 bindless/timeline/RT 模型差异收敛（NDA 下另议；接缝先留）。
- **GPU Work Graphs**：作为 indirect 之外的工作生成路径（research 档，M12）——接口预留 `DispatchGraph`。
- Metal3 的 bindless（argument buffer）/ RT 模型映射成本。
- 上传/staging 策略（ring vs per-frame arena）实测。

## 11. 依赖与被依赖

- **依赖**：Foundation（Handle/Memory/Platform 原生窗口/动态库）、JobSystem（录制并行）。
- **被依赖**：ShaderCompiler（PSO 创建对接）、RenderGraph、GpuScene、所有渲染 pass。**M2 地基，接口需早冻结**。


