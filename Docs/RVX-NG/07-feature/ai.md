# RVX-NG · Feature/AI 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §12 AI  
**里程碑**：M9  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供开放世界可扩展 AI 框架：导航网格、动态障碍、寻路、局部避让、行为树/状态树、感知、智能体 LOD、人群批处理与调试可视化。

**范围内**：NavMesh 生成与流式、path query、steering/avoidance、行为树/状态树、黑板、感知、AI LOD、ECS System 接入。  
**范围外**：具体玩法决策内容、机器学习训练、编辑器 UI。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 开放世界 | NavMesh 分 tile/cell 流式，与 World Partition 协作 |
| 批量查询 | 大量 agent 寻路与感知按预算 time-slice |
| 可调试 | 每个决策、感知、路径可追踪 |
| 可降级 | 远距离 agent 降低 tick 频率、感知精度、路径更新 |
| 确定性边界 | 回放/回滚所需 AI 子集可固定种子与固定调度 |

## 3. 公共接口面（设计契约）

```cpp
struct AgentComponent {
    NavAgentId navId;
    float radius;
    float maxSpeed;
    AiLod lod;
};

class NavigationSystem {
    PathRequestId RequestPath(Entity agent, Vec3 start, Vec3 goal, PathOptions options);
    bool TryGetPath(PathRequestId id, Path& outPath) const;
    void UpdateTiles(WorldCellSet visibleCells);
};

class BehaviorSystem {
    void RegisterTree(Entity agent, BehaviorTreeHandle tree);
    void TickAgents(float dt, AiBudget budget);
};
```

**不变量**：AI 不直接改渲染资源；所有异步 path/behavior 结果在帧边界提交；动态障碍更新与 NavMesh tile 版本一致；预算耗尽时延迟而非阻塞主线程。

## 4. 数据结构与内存布局

- **NavMesh Tile**：cell 级可加载数据，含 polygon、portal、off-mesh link。
- **Agent State**：位置、速度、路径 corridor、行为状态、LOD、感知缓存。
- **Blackboard**：类型化 key/value，反射驱动序列化与调试。
- **Perception Cache**：视野/听觉/事件感知按空间索引批量查询。
- **Path Queue**：优先级队列，按 agent 重要性、距离、等待时间排序。

## 5. 核心算法

- **寻路**：tile graph A* + polygon corridor + funnel string-pull。
- **动态障碍**：局部 obstacle map / tile 局部重建，避免全局重烘焙。
- **局部避让**：ORCA/RVO 风格或 steering force，可按 agent LOD 降级。
- **行为执行**：行为树/状态树节点声明读写黑板键；高成本节点预算化。
- **感知**：空间索引候选集 + LOS/听觉衰减检测，结果缓存并按帧过期。
- **AI LOD**：近处全 tick，远处低频 tick/目标代理，群体可用 crowd simulation。

## 6. 线程与内存模型

寻路、感知、行为 tick 都通过 JobSystem 批处理。每个 agent 的状态写入单线程分片，跨系统通过 command buffer 合并。NavMesh tile 数据为只读共享，动态更新生成新版本后原子切换。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 无可达路径 | 返回 partial/path failed，行为树可降级 |
| tile 未加载 | 请求挂起或返回低精度远景路径 |
| 预算耗尽 | 延迟低优先级请求，记录等待时间 |
| 黑板类型错误 | Debug 断言，Release 返回默认/失败 |

**禁止**：寻路阻塞主线程；行为节点直接持有易失实体指针；动态障碍导致全世界 NavMesh 重建。

## 8. 性能目标

- 数千 agent 行为 tick 在预算内按 LOD 分摊。
- path query p95 延迟可控，高优先级 agent 优先完成。
- tile 流式不会造成帧尖峰。

## 9. 测试计划

- NavMesh tile 加载/卸载、off-mesh link、partial path、funnel 正确性。
- 行为树 determinism 子集回归。
- 感知 LOS/听觉/事件过滤测试。
- 大量 agent 压力与预算门禁。

## 10. 开放问题 / Spike

- Recast/Detour 接入 vs 自研 tile navmesh。
- Crowd simulation 与动画 motion matching 的职责边界。
- 大世界远距离 AI 的代理模型。
- 网络同步 AI 决策还是同步输入/状态。

## 11. 依赖与被依赖

- **依赖**：World Partition、Spatial、ECS、JobSystem、Physics 查询。
- **被依赖**：Gameplay、Networking、Editor debug、Animation locomotion。


