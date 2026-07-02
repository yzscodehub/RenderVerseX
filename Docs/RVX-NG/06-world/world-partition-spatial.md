# RVX-NG · World/World Partition & Spatial 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §11、契约 C8
**里程碑**：M1+（流式于 M8 与 Asset 联动）
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：开放世界的空间分区流式（cell 加载/卸载 + data layer）+ 单一权威空间索引（增量 BVH）+ 层级/Prefab，提供统一空间查询来源。

**范围内**：World Partition（网格/八叉树 cell、data layer、流式触发）、单一 BVH 空间索引（增量 refit + 脏列表）、空间查询、实体 ID 跨流稳定、Prefab/序列化对接。
**范围外**：ECS 存储（数据层）、资产流式（Asset streaming）、渲染剔除（Geometry，自有 GPU HiZ）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 分区 | 大世界按 cell 流式，按需加载/卸载 |
| 单一索引 | **一个**权威空间索引，增量维护 |
| 查询 | 射线/范围/最近查询供 picking/物理/AI |
| ID 稳定 | 实体跨 cell 流式 ID 稳定（持久 GUID 或稳定映射） |
| 单向 | 渲染不反向改世界（C8） |

## 3. 公共接口面（设计契约）

```cpp
class WorldPartition {
    void SetStreamingSource(Vec3 pos, float radius);   // 通常相机
    void Tick();                                        // 触发 cell 加载/卸载
    void EnableDataLayer(LayerId, bool);                // 逻辑分层（昼夜/任务）
};

class SpatialIndex {                                    // 单一权威 BVH
    void Insert(Entity, Aabb);  void Update(Entity, Aabb);  void Remove(Entity);
    Span<Entity> QueryAabb(const Aabb&) const;
    bool         Raycast(const Ray&, HitResult&) const;
    Span<Entity> QueryNearest(Vec3, float radius) const;
    void Refit();                                        // 帧边界增量 refit + 脏列表
};
```

**不变量**：唯一 BVH（picking/物理/AI 共用，无第二索引）；transform 变更标脏 → 帧边界批量 refit；cell 流式时实体 ID 稳定；渲染只读（C8）。

## 4. 数据结构与内存布局

- **Cell 网格/八叉树**：世界按 cell 划分；每 cell 持其实体集 + 资产引用 + 流式状态。
- **Data layer**：逻辑分层（可独立启停的实体集合）。
- **BVH**：单一权威层次包围盒；脏实体列表驱动增量 refit（非每帧重建）。
- **ID 映射**：实体持久 ID（GUID 或稳定索引）跨 cell load/unload。

## 5. 核心算法

- **流式**：streaming source（相机）→ 计算应驻留 cell 集 → 加载新进入 cell（批量 spawn 实体 + 预取资产）/ 卸载离开 cell（despawn + 释放驻留）。
- **增量 BVH**：脏实体（transform 变更）入脏列表 → 帧边界 refit 受影响子树（非全建）；周期性 rebalance。
- **查询**：BVH 遍历（射线/AABB/最近）。
- **ID 稳定**：cell 卸载保留实体持久 ID；重载恢复同 ID（存档/网络/引用稳定）。

## 6. 线程与内存模型

- cell 加载/卸载在 JobSystem（批量 spawn 经 ECS CommandBuffer，帧边界回放）。
- BVH refit 帧边界（与 ECS 结构性变更同步点）；查询可并发只读。
- cell 实体经数据层 ECS 存储；资产经 Asset streaming。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| cell 加载未完成被查询 | 用已加载子集；pending 标记 |
| 流式卸载进行中的引用 | 持久 ID + 代际句柄，悬垂安全 |
| BVH 退化（频繁大移动） | 周期 rebalance；脏预算限速 |
| 渲染改世界 | C8 禁止 |

**禁止**：多个权威空间索引并存；每帧全量重建 BVH。

## 8. 性能目标

- BVH 增量 refit O(脏数)，非 O(N)。
- cell 流式无卡顿（批量 spawn time-sliced + 资产预取）。
- 查询（射线/范围）log(N)。

## 9. 测试计划

- **单元**：BVH 增删改查正确、增量 refit、cell load/unload 实体集正确。
- **ID 稳定**：cell 卸载重载后实体 ID/引用稳定（存档往返）。
- **单一索引门禁**：无第二空间索引（防回归）。
- **流式**：移动 streaming source 无卡顿、无悬垂。

## 10. 开放问题 / Spike

- cell 大小/八叉树深度 vs 流式粒度（与 Asset/Render 协调）。
- 大移动实体（载具/飞行）的 BVH 策略。
- data layer 与存档/网络的交互。
- 实体 ID 跨流的持久化方案（GUID vs 稳定索引）。

## 11. 依赖与被依赖

- **依赖**：数据层（ECS spawn/despawn）、Asset streaming（cell 资产）、Foundation（Jobs/Memory）。
- **被依赖**：Picking、Physics、AI（空间查询）、Render（cell→Extract）、存档/网络（ID）。


