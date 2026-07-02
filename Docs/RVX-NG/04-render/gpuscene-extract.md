# RVX-NG · Render/GpuScene & Extract 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §9 Extract/GpuScene、决策 D3、契约 C8
**里程碑**：M3
**状态**：详细设计 · v1.0 可评审
**地位**：游戏线↔渲染线**唯一跨界**（Extract，三缓冲）+ 持久化 bindless 场景表（GpuScene），GPU-driven 的数据基底。

---

## 1. 目标与范围

**目标**：把 ECS 世界的可渲染状态**增量**抽取为渲染世界快照（三缓冲），并维护持久化 GPU 大表（mesh/instance/material/light/transform，全 bindless），供 GPU-driven 剔除/绘制直接索引。

**范围内**：Extract（ECS→快照）、RenderProxy、GpuScene 表布局与增量上传、slot 分配、脏跟踪、三缓冲。
**范围外**：剔除/绘制（Geometry&Culling）、ECS 存储（数据层）、上传原语（RHI staging）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 单向 | 渲染只读快照，绝不反向改 World（C8） |
| 增量 | 仅上传变更（脏跟踪），非每帧重建 |
| bindless | 几何/材质/纹理经全局索引访问，无 per-draw 绑定 |
| 解耦 | 三缓冲：sim 写 N+1，render 读 N，GPU 用 N-1 |
| 规模 | 10⁶ 实例的表增量更新 < 帧时小头 |

## 3. 公共接口面（设计契约）

```cpp
// 渲染侧实例（数据导向，从 ECS 抽取）
struct RenderProxy {                  // ECS 组件：实体的渲染表示引用
    u32 meshId, materialId;
    u32 gpuSceneSlot;                 // 在 GpuScene instance 表的行
    u32 flags;                        // 可见/投影/RT 等位
};

class Extract {
    // sim 阶段末并行运行：脏 RenderProxy/Transform → 快照增量
    SnapshotHandle Run(const World&);          // 三缓冲之一
    const RenderSnapshot& Read(SnapshotHandle); // render 线程只读
};

class GpuScene {
    u32  AllocInstance();  void FreeInstance(u32 slot);        // 持久 slot
    void UpdateInstance(u32 slot, const InstanceData&);        // 标脏
    BindlessIndex RegisterMesh(const MeshGpu&);                // 几何表
    BindlessIndex RegisterMaterial(const MaterialGpu&);       // 材质表
    void Flush(ICommandContext&);     // 仅上传脏区段（增量）
    BufferHandle InstanceBuffer(), MeshBuffer(), MaterialBuffer(), LightBuffer();
};
```

**不变量**：Extract 只读 World（C8）；`gpuSceneSlot` 持久（实体存活期间稳定，利于时域）；`Flush` 只传脏；快照三缓冲互不阻塞。

## 4. 数据结构与内存布局

- **GpuScene 表**（GPU buffer，bindless 可索引）：
  - `InstanceData[]`：transform(3x4)、prevTransform（motion vector/TAA）、meshId、materialId、bounds、flags。对齐 GPU。
  - `MeshGpu[]`：meshlet 偏移、顶点/索引 buffer bindless 索引、LOD 信息。
  - `MaterialGpu[]`：bindless 纹理索引集 + 参数（PBR/分层预留）。
  - `LightGpu[]`：类型/位置/颜色/范围/阴影索引。
- **slot 分配**：freelist + 代际（FreeInstance 后槽位复用）。
- **脏跟踪**：脏 slot 集（或脏区段 range），`Flush` 合并为最少上传段。
- **快照**：三份 `RenderSnapshot`（可见实例索引 + 视图 + 光源），轮换。

## 5. 核心算法

- **Extract**：`ParallelFor` 遍历脏 `RenderProxy`/`Transform`（ECS 变更检测驱动）→ 写快照 + 标 GpuScene slot 脏。仅处理变更实体（增量）。
- **增量上传**：`Flush` 收集脏 slot → 合并连续区段 → RHI staging ring 上传 → barrier。
- **prevTransform**：每帧把 transform 滚动到 prev（motion vector/TAA 用）。
- **三缓冲轮换**：sim 完成 Extract 写 buffer i；render 读 buffer (i-1)；无锁交换索引。

## 6. 线程与内存模型

- Extract 在 sim 阶段末并行（JobSystem），写当前快照 buffer。
- GpuScene `Flush` 在 render 阶段录制上传命令。
- 快照内存三缓冲池化；GPU 表持久驻留（residency 高优先）。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 渲染期改 World | 编译/契约禁止（C8）；Debug 校验 |
| slot 耗尽 | 表扩容或拒绝（上层分级降实例数） |
| mesh/material 未上传 | 实例标 pending，剔除阶段跳过（不画半成品） |
| 快照读写竞争 | 三缓冲 + 索引原子交换保证无竞争 |

**禁止**：每帧全量重建表；渲染反向写世界。

## 8. 性能目标

- Extract 增量：仅 O(脏实体)，1M 实例典型脏率下 < 0.5 ms（多核）。
- `Flush` 上传：合并段，带宽友好；满帧重传 = 异常（监控）。
- GpuScene 索引访问 O(1)（shader 直查 bindless）。

## 9. 测试计划

- **单元**：slot 分配/释放/代际、脏跟踪正确、增量上传只传脏、prevTransform 滚动。
- **解耦**：sim 改 transform → 下一快照体现，render 读旧快照不受影响（三缓冲正确）。
- **C8 门禁**：渲染路径无 World 写（静态/运行期校验）。
- **golden**：增量 vs 全量重建产出一致（验证增量正确）。

## 10. 开放问题 / Spike

- 脏跟踪粒度（per-slot vs region）与上传合并启发式。
- 快照内存量（1M 实例 × 大小）vs 三缓冲成本——按规模实测。
- World Partition 流式时 slot 的批量分配/释放（与 06-world 协调）。
- skinned/动画实例的 transform 上传策略（每帧大量）。

## 11. 依赖与被依赖

- **依赖**：数据层（ECS 查询/变更检测）、RHI（buffer/bindless/staging）、Foundation（Jobs/Memory）。
- **被依赖**：Geometry&Culling、Lighting、RayTracing、所有消费场景表的 pass。**M3 渲染地基**。


