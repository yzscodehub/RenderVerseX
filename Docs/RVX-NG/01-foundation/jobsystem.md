# RVX-NG · Foundation/JobSystem 详细设计（Tier-1）

**日期**：2026-06-20  
**层级**：Tier-1 · 派生自总纲 §5 Jobs、契约 C2/C9、决策 D2/D11  
**里程碑**：M0  
**状态**：详细设计 · 待 S0 Spike

---

## 1. 目标与范围

**目标**：提供全引擎唯一的并行执行基底：工作窃取 task graph、依赖计数、`ParallelFor`、主线程亲和队列、确定性子域调度，以及可选 fiber 后端。

**范围内**：job 表达、依赖图、调度/窃取、批量 graph 提交、worker 生命周期、worker 内等待语义、主线程任务、确定性并行、profiler 标记。  
**范围外**：ECS system scheduling 策略、GPU queue/timeline 实现、语言级 coroutine、平台线程池实现细节。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 唯一并行基底 | Runtime 模块不得裸建长期线程；专用实时线程需通过 Platform/App 明确声明 |
| 低开销 | 微 job 调度和完成摊销开销受控，热路径不做通用 malloc |
| 非阻塞等待 | worker 内等待不得阻塞 OS worker 线程；默认使用 continuation 语义 |
| 主线程亲和 | OS 消息泵、窗口、部分 present/API 走主线程队列 |
| 确定性子域 | 固定切片、固定规约、稳定合并，结果不依赖 worker 数 |
| 可观测 | job scope、等待、steal、park、主线程队列积压都进入 trace |

## 3. 公共接口面（设计契约）

```cpp
struct JobHandle { uint32 index; uint32 gen; };

struct JobDesc {
    Name debugName;
    JobFn fn;
    void* data = nullptr;
    JobPriority priority = JobPriority::Normal;
    AllocatorTag allocatorTag;
};

class JobCounter {
public:
    uint32 Pending() const;
};

class IJobSystem {
public:
    JobHandle Schedule(const JobDesc& desc, Span<const JobHandle> deps = {});
    JobHandle ParallelFor(uint32 count, uint32 grain, ParallelForFn body, void* data,
                          Span<const JobHandle> deps = {});
    JobHandle Then(JobHandle dep, const JobDesc& continuation);
    void Wait(JobHandle handle);
    void Wait(JobCounter& counter);
    void RunOnMainThread(const JobDesc& desc);
    GraphBuilder BeginGraph(Name debugName);
    uint32 WorkerCount() const;
    bool IsWorkerThread() const;
};
```

**不变量**：`Schedule` 返回后句柄可作为依赖；`data` 生命周期必须覆盖 job 执行；默认后端的 worker 内 `Wait` 只允许通过 continuation/fiber 让出执行权，不能阻塞 worker；主线程队列只在主线程 drain。

## 4. 数据结构与内存布局

- **Worker State**：worker id、TLS frame allocator、local job pool、work-stealing deque、profiler context。
- **Job Record**：fn/data、pending dependency count、successor list、priority、debug name、generation。
- **Work Deque**：Chase-Lev 风格 owner LIFO、thief FIFO，cache-line 对齐避免 false sharing。
- **Counter**：原子 pending count，支持 fan-in/fan-out 与外部等待。
- **Graph Arena**：批量 graph builder 使用专用 arena，Kick 后冻结拓扑。
- **Main Thread Queue**：MPSC queue，按 frame safe point drain，记录积压。

热字段（fn/data/pending/successor）与冷字段（debug/profiler/name）分离；job record 从 per-worker slab 池分配，完成后批量回收。

## 5. 核心算法

- **Schedule**：创建 job record，初始化 pending count；deps 为空直接推入本地 deque，否则挂到前置 job successor list。
- **Worker Loop**：本地 pop → 随机 victim steal → 短自旋 → park；新 job 到达唤醒。
- **Completion**：执行 fn 后递减 successors pending；到 0 的 successor 推入当前 worker deque；signal counter；回收 record。
- **ParallelFor**：按 grain 切片生成 job；grain=0 使用调用点历史耗时和 worker 数自适应。
- **Continuation Wait**：默认后端把等待后的工作表达为 `Then` continuation；worker 回到 loop 执行其他 ready job。
- **Fiber Backend**：可选后端在 `Wait` 时切换 fiber，恢复后语义等价；fiber 不改变公共契约。
- **Deterministic Mode**：固定切片、固定规约树、稳定 command buffer 合并；必要时单线程参考模式。

## 6. 线程与内存模型

worker 数按硬件线程和保留线程配置。主线程负责 OS pump 和主线程亲和任务，空闲时可以参与 job drain。所有 worker 使用 TLS scratch/arena；跨线程共享只通过原子 counter、deque steal、MPSC queue 和冻结后的 graph 数据。

确定性 scope 不依赖执行先后，所有可见写入通过稳定顺序 command buffer 合并。Fiber 后端使用固定栈池，禁用跨 fiber 依赖未声明 TLS 状态。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 依赖环 | graph Kick 前 fail-fast，输出环路径 |
| worker 内阻塞式 Wait | Debug 断言；必须用 continuation 或 fiber backend |
| job 池耗尽 | 触发 pressure，必要时就地执行低优先 job 并计数 |
| job 异常逃逸 | 终止当前任务，进入 crash/assert 管线 |
| 主线程队列积压 | 按预算 drain，超额延后并记录 trace |

**禁止**：隐藏 OS blocking wait、确定性域使用 steal 完成顺序影响结果、模块绕过 JobSystem 创建长期 worker。

## 8. 性能目标

- 微 job schedule/complete 摊销开销低于帧预算噪声阈值。
- 百万级微 job、深依赖链、宽 fan-in/fan-out 不退化到 O(N²)。
- 高争用下 worker steal、park/wakeup、main thread drain 可在 profiler 中归因。
- 确定性模式允许较慢，但成本受限于 deterministic domain。

## 9. 测试计划

- 单元：依赖顺序、counter、代际句柄、`ParallelFor` 切片覆盖、`Then` continuation。
- 压力：百万微 job、深链、fan-in/fan-out、job pool 耗尽、steal 高争用。
- 确定性：workerCount 1/2/8/16 多次运行 state hash 一致。
- 死锁：依赖环、worker 内 wait、主线程队列阻塞注入。
- Sanitizer：TSan/ASan 覆盖队列、counter、job data 生命周期。

## 10. 开放问题 / Spike

- **S0 JobSystem continuation vs fiber**：验证无 fiber 默认后端的 API 工效、调试体验和 profiler 表达；fiber 作为可选后端是否值得进入 Advanced tier。
- GPU fence 转 JobCounter 的桥接 API 与 Frame Timing/RHI timeline 对齐。
- NUMA-aware stealing 在高核数平台上的收益。
- 优先级 lane 是否需要独立队列，还是本地出队排序足够。

## 11. 依赖与被依赖

- **依赖**：Core/Handle/Result、Memory per-worker pool、Platform thread primitives、Profiler trace。
- **被依赖**：ECS Scheduler、RenderGraph、Asset streaming/cook、Animation、Physics、AI、Audio decode、Tools/Editor、Testing。M0 中接口必须先稳定。
