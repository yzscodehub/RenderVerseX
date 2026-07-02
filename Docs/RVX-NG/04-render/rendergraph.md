# RVX-NG · Render/RenderGraph 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §9 RenderGraph、契约 C6
**里程碑**：M3
**状态**：详细设计 · 待 S4 Spike
**地位**：唯一表达 GPU 工作的机制。**真** async compute + **真**瞬态别名。

---

## 1. 目标与范围

**目标**：声明式帧图——pass 声明资源读写，图自动推导依赖、插入 barrier、剔除死 pass、瞬态资源内存别名、跨队列 async compute 调度。

**范围内**：pass 声明、瞬态/导入资源、依赖 DAG、barrier 推导与批处理、内存别名、async compute、编译/执行、可视化导出。
**范围外**：具体 pass 内容（各渲染子系统）、RHI 原语（barrier/timeline 由 RHI 提供）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 依赖推导 | 据 Read/Write 自动建 DAG，无手写 barrier |
| barrier | 自动插入 + 批处理 + 冗余消除；每子资源状态跟踪 |
| async | compute pass 真提交 compute 队列 + timeline 同步（非回退） |
| 别名 | 瞬态资源生命周期不重叠则共享堆内存，**发射别名 barrier** |
| 诚实 | async/别名能力位与实现一致；不支持即显式标注降级（C6） |
| 编译开销 | 每帧编译 < 帧时预算小头（pass 数百级） |

## 3. 公共接口面（设计契约）

```cpp
struct RgTexture { u32 id; };  struct RgBuffer { u32 id; };

class RgBuilder {
    RgTexture CreateTexture(const TextureDesc&);       // 瞬态
    RgTexture Import(TextureHandle, ResourceState initial);
    RgTexture Read(RgTexture, ShaderStage);
    RgTexture Write(RgTexture, ResourceState);         // RT/UAV/DS
    RgBuffer  ReadWrite(RgBuffer);
    void      SetSideEffect();                          // 防剔除（present 等）
};

class RenderGraph {
    template<class Data>
    void AddPass(Name, PassType /*Graphics/Compute/RayTracing/Copy*/,
                 Fn<void(RgBuilder&, Data&)> setup,
                 Fn<void(const Data&, ICommandContext&)> execute);
    CompileResult Compile();          // 建 DAG/剔除/barrier/别名/async 计划
    void Execute(IQueue* gfx, IQueue* asyncCompute);   // 多 context 并行录制
    const CompileStats& Stats() const; // 含诚实位：asyncUsed/aliasingUsed/...
    String ExportGraphviz() const;
};
```

**不变量**：`Execute` 前必 `Compile`；剔除无副作用且无下游消费的 pass；瞬态资源别名前必发射 aliasing barrier（否则不别名）；`CompileStats` 的 `asyncComputeUsed`/`aliasingEnabled` 反映**真实**行为（不谎报）。

## 4. 数据结构与内存布局

- **Pass 节点**：类型、setup/execute 闭包、读写资源列表（带 state/stage）、side-effect 标记。
- **资源节点**：瞬态(desc) 或导入(handle+initial)；每子资源（mip/slice）状态跟踪；首/末使用 pass（生命周期区间）。
- **DAG**：写→读边（生产者-消费者）；拓扑序 = 执行序。
- **瞬态堆**：按生命周期区间着色（interval graph coloring）分配别名组；placed resource 共享堆 offset（RHI 显式堆）。
- **Barrier 计划**：每 pass 前的 barrier 批（state 转换 + aliasing barrier）。

## 5. 核心算法

- **建图**：扫 pass 读写 → 建资源生产者/消费者边 → DAG。
- **剔除**：从 side-effect/导出 pass 反向可达性标记；不可达 pass 剔除。
- **拓扑排序**：Kahn；失败（环）→ 编译错误（非静默回退）。
- **barrier 推导**：沿执行序跟踪每子资源 state；状态变化处插 barrier；合并相邻/冗余（split barrier 可用时跨 pass）。
- **别名**：瞬态资源生命周期区间 → interval coloring → 不重叠者共享堆；别名切换处发**aliasing barrier**（真发射，非注释）。
- **async compute**：compute pass 标记 async → 分配 compute 队列；跨队列依赖用 timeline semaphore（gfx signal value N，compute wait N）。能力不足 → 标 `asyncComputeUsed=false` 走 gfx（诚实降级）。

## 6. 线程与内存模型

- **编译**：单线程（DAG/排序/barrier/别名），开销小。
- **执行**：pass 的 execute 闭包在多 worker 并行录制不同 `ICommandContext`（C9）；提交按队列 + timeline。
- **瞬态内存**：每帧从瞬态堆池分配，帧末回收；别名组共享物理内存（省显存）。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 依赖环 | 拓扑失败 → 编译错误（非静默回退） |
| 读未初始化瞬态 | 校验 → 错误 |
| async/别名不支持 | 显式标 `used=false` + 降级路径（诚实） |
| 资源 state 冲突 | 校验层报不兼容用法 |

**禁止**：手写 barrier 旁路；别名却不发 barrier；async stub 谎报成功。

## 8. 性能目标

- 编译（数百 pass）< 0.2 ms。
- barrier 合并率 ≥ 50%。
- 别名省显存：瞬态总量 vs 别名后，开放世界场景省 ≥ 30%。
- async compute 真重叠：阴影/AO/光剔除等与 gfx 重叠，帧时下降可测。

## 9. 测试计划

- **单元**：DAG 正确、剔除正确、拓扑环检测、barrier 推导（每子资源 state）、别名区间不重叠。
- **诚实门禁**：`CompileStats` 位与实际提交一致（async 真走 compute 队列、别名真发 barrier）——防回退谎报回归。
- **golden**：含 async/别名的图，跨后端逐像素一致 + 无竞争（GPU 校验层）。
- **可视化**：Graphviz 导出正确（FrameDebugger 用）。

## 10. 开放问题 / Spike

- async compute 队列调度策略（哪些 pass 划 async 收益最大）——实测。
- 别名 interval coloring 的启发式（贪心 vs 最优）。
- GPU Work Graphs 对图模型的影响（research，M12）。
- 多视图（阴影/反射）下的图复用/合并。

## 11. 依赖与被依赖

- **依赖**：RHI（barrier/timeline/placed heap/队列）、Foundation（Jobs 并行录制/Memory）。
- **被依赖**：所有渲染 pass、FrameRenderer（编排）。**M3 渲染地基**。



