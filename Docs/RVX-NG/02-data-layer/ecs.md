# RVX-NG · 数据层/ECS 存储与调度 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §6、决策 D1/D11、契约 C2/C8
**里程碑**：M1
**状态**：详细设计 · 待 S1 Spike
**地位**：D1 混合数据导向的**数据导向内核**。Authoring 门面见 `authoring-facade.md`。

---

## 1. 目标与范围

**目标**：archetype 列式存储的世界状态 + 据读写集自动并行的系统调度，作为渲染/变换/粒子/人群/动画等海量域的唯一存储与迭代基底。

**范围内**：实体/组件存储（archetype SoA）、Query、System、Scheduler（→ JobSystem task-graph）、Resource（单例）、Relation（层级）、CommandBuffer（结构性变更）、变更检测、确定性子域调度。
**范围外**：bespoke gameplay 对象语义（Authoring 门面）、序列化字节（Reflect/Archive）、渲染消费（Render/Extract）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 迭代吞吐 | 组件列连续，线性迭代接近内存带宽（GB/s 级） |
| 实体规模 | 10⁶+ 实体不退化 |
| 结构性变更 | 增删组件/实体延迟到帧边界批量回放（迭代期安全） |
| 并行 | 系统据读写集自动并行（读读并行，写冲突串行） |
| 确定性 | 确定性子域内迭代/规约顺序固定（D11） |
| 句柄安全 | 实体代际化，悬垂解引用→null（C4） |

## 3. 公共接口面（设计契约）

```cpp
using Entity = Handle<struct EntityTag>;       // {index, gen}

class World {
    template<class...C> Entity Spawn(C&&...);   // 定位/创建 archetype，追加行
    void   Destroy(Entity);                      // 延迟到帧边界
    bool   Alive(Entity) const;
    template<class C> C*  Get(Entity);           // O(1) 经 (archetype,row)
    template<class C> void Add(Entity, C);       // 结构性变更→CommandBuffer
    template<class C> void Remove(Entity);

    template<class...C> Query<C...> Query();      // 迭代匹配 archetype 的 chunk
    template<class R>   R&  GetResource();        // 单例（非实体）

    void Relate(Entity child, Entity parent, RelationKind);
    CommandBuffer& Commands();                    // worker 内安全的延迟变更
};

// 系统：纯函数 + 声明读写集（编译期由参数推导或显式声明）
struct SystemDecl {
    Span<TypeId> reads, writes;   // 决定可并行性
    TickGroup    group;
    void (*run)(World&, JobContext&);
};

class Scheduler {
    void Register(SystemDecl);
    void Run(World&);             // 据读写冲突编译成 JobSystem task-graph
    void SetDeterministic(bool);  // 确定性子域：有序/固定规约（D11）
};
```

**不变量**：迭代期间**禁止**直接结构性变更（用 `Commands()` 延迟）；`Get` 返回指针在结构性变更回放后可能失效（不得跨帧持有）；同 archetype 行连续；`Entity` 解引用验证 gen。

## 4. 数据结构与内存布局

- **Archetype**：一组组件类型的唯一组合；持每组件一**列**（`PagedArray<C>`，SoA），+ 一列 `Entity`（行→实体反查）。同 archetype 实体在所有列同 row 对齐。
- **Chunk 分页**：每列按固定大小 chunk（如 16 KB）分页，利于并行切片与 cache 局部。
- **Entity 索引**：`Entity.index → {archetypeId, row, gen}`（`PagedArray`），O(1) 定位。
- **Archetype 图**：节点=archetype，边=加/减某组件 → 目标 archetype（结构性变更走边，避免重算）。
- **Query 缓存**：签名（组件 mask）→ 匹配 archetype 列表，archetype 新建时增量更新。
- **变更检测**：每列每 chunk 一个 `lastWriteVersion`；写系统 bump，Extract/消费者按 version 增量。

## 5. 核心算法

- **Spawn**：按组件集找/建 archetype（archetype 图）→ 各列 append → 写 entity 索引。
- **结构性变更（Add/Remove/Destroy）**：记入 `CommandBuffer`（每 worker 一个），**帧边界**单线程回放：沿 archetype 图边把行从源 archetype move 到目标（memcpy 列 + 更新索引），swap-remove 维持密集。
- **Query 迭代**：取签名匹配的 archetype 列表 → 遍历各 archetype 的 chunk → 对每 chunk 提供组件列指针窗口（`begin..end`）。
- **Scheduler**：拓扑系统按 TickGroup；同组内据 read/write 集构建冲突图 → 不冲突的系统并行（每系统一/多 job，`ParallelFor` 切 chunk）→ 提交 JobSystem。
- **确定性子域**：archetype 迭代按**稳定序**（archetypeId + chunk index 排序），`ParallelFor` 固定切片，规约固定结合序（调用 JobSystem 确定性模式）。

## 6. 线程与内存模型

- 系统经 Scheduler 编译为 JobSystem task-graph 并行跑（C2）；冲突系统串行，独立系统并行。
- 列/chunk 从类型池/竞技场分配（Memory）；结构性变更回放**单线程**于帧边界（无并发结构改动）。
- 渲染经 Extract 单向读快照（C8），绝不在渲染线程改 World。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 失效 Entity 解引用 | gen 校验 → null + 可选断言 |
| 迭代期结构性变更 | 编译期/运行期阻止直接改；强制走 `Commands()` |
| archetype 爆炸（碎片化组合） | 监控 archetype 数；建议组件设计 + 可选 chunk 合并 |
| 读写集声明错误（漏声明写） | Debug 期访问追踪校验声明 vs 实际，违例断言（防数据竞争） |

**禁止**：渲染/外部线程改 World（C8）；确定性子域用非确定迭代顺序。

## 8. 性能目标

- 单组件列迭代 ≥ 0.7× 内存带宽（连续 SoA）。
- `Get<C>(entity)` O(1) < 20 ns。
- 结构性变更回放 O(变更数)，批量 memcpy。
- 1M 实体 transform 系统 < 1 ms（多核 `ParallelFor`）。
- Query 匹配缓存命中 O(匹配 archetype 数)，非每帧全扫。

## 9. 测试计划

- **单元**：spawn/destroy/add/remove 后 archetype 正确、entity 索引一致、query 覆盖精确、relation 层级。
- **并行**：调度据读写集正确并行/串行（注入冲突验证串行化）；TSan 无竞争。
- **确定性回归**：确定性子域，`workerCount∈{1,8}` × 多次，系统输出逐位一致（与 JobSystem 确定性协同）。
- **变更检测**：脏 version 驱动的增量正确（Extract 只取变更）。
- **压力**：1M 实体迭代吞吐、结构性变更 churn、archetype 碎片。

## 10. 开放问题 / Spike

- **Spike-1**：archetype（迭代快、结构变更贵）vs sparse-set（结构变更快、迭代略慢）——按本引擎工作负载实测选型（M1）。
- **Spike-2**：Relation 存储（archetype 内关系组件 vs 侧表）与层级遍历性能。
- 流式：World Partition cell 加载/卸载时 archetype chunk 的批量 spawn/despawn 与实体 ID 跨流稳定（与 06-world 协调）。
- 组件设计规范（避免 archetype 爆炸）——纳入编码约定。

## 11. 依赖与被依赖

- **依赖**：JobSystem（系统并行）、Memory（列/chunk 池）、Reflect（组件注册/序列化）、Core（Entity/Handle/容器）。
- **被依赖**：Authoring 门面（桥接）、World、Render/Extract、全部 Feature 系统。**M1 核心，接口需早冻结**（被多层依赖）。

