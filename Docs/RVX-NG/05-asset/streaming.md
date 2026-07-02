# RVX-NG · Asset/流式 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §10 Stream、决策 D10
**里程碑**：M8
**状态**：详细设计 · 待 S7 Spike

---

## 1. 目标与范围

**目标**：开放世界的虚拟纹理 + 虚拟几何流式，每帧驻留预算（time-sliced），与 RHI 显存 residency 联动，使内容量 ≫ 显存仍流畅。

**范围内**：虚拟纹理（页表/反馈/物理 atlas）、虚拟几何（cluster 流式）、驻留预算调度、优先级（屏幕需求驱动）、与 World Partition 协同。
**范围外**：cook/格式（asset-pipeline）、显存原语（RHI residency）、剔除（Geometry）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 虚拟化 | 纹理/几何按需分页，内容量 ≫ 显存 |
| 预算 | 每帧上传/驻留预算（time-sliced），不超带宽/显存 |
| 需求驱动 | 流入由 GPU 反馈（屏幕实际需要的页/cluster） |
| 联动 | 与 RHI residency + Memory 分类预算统一 |
| 渐进 | 未驻留用低 mip/低 LOD 兜底，不 pop 黑 |

## 3. 公共接口面（设计契约）

```cpp
class VirtualTexture {
    void RecordFeedback(RgBuilder&, RgTexture feedback); // GPU 写需求页
    void ProcessFeedback(BufferHandle requests);          // CPU 读需求 → 调度加载
    BindlessIndex PageTable();                             // shader 间接采样
};

class VirtualGeometry {                                    // cluster 流式
    void ProcessFeedback(BufferHandle clusterRequests);
    void StreamIn(ClusterId, Priority);
};

class StreamScheduler {
    void SetBudget(BytesPerFrame upload, Bytes resident);  // 预算
    void Tick();                                            // 按优先级 time-sliced 加载
};
```

**不变量**：流入由 GPU 反馈驱动（需求 = 屏幕实际所需）；每帧上传 ≤ 预算；未驻留走低 mip/LOD 兜底（不黑/不卡）；驻留与 RHI residency + Memory 预算一致。

## 4. 数据结构与内存布局

- **VT 页表**：虚拟页 → 物理 atlas 间接表（GPU 驻留）；物理 atlas（固定，LRU）。
- **反馈缓冲**：GPU 渲染时写"采样了哪些虚拟页"。
- **几何 cluster 流**：cluster 池（GPU），LRU；page request buffer。
- **加载队列**：按优先级（屏幕覆盖/距离）排序的 pending 请求。

## 5. 核心算法

- **VT 反馈循环**：渲染采样写反馈 → CPU 读 → 缺页入加载队列 → 异步加载（Asset）→ 填物理 atlas + 更页表。
- **优先级**：按屏幕覆盖/距离/可见性排序；预算内 time-sliced 处理。
- **驱逐**：物理 atlas/cluster 池满 → LRU 逐出未用页（与 residency 协同）。
- **兜底**：缺页采样回退到已驻留的低 mip/父 cluster（渐进细化，不 pop）。
- **World Partition 协同**：cell 加载预取其资产；卸载释放驻留。

## 6. 线程与内存模型

- 反馈处理 + 加载调度在 JobSystem；上传经 RHI staging（预算限速）。
- 物理 atlas/cluster 池 GPU 驻留，受 residency 管理。
- CPU↔GPU 反馈有 1-2 帧延迟（容忍，兜底覆盖）。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 预算耗尽 | 本帧只处理高优先，余下帧续（渐进） |
| 物理池满 | LRU 逐出；极端 → 降全局质量档 |
| 加载失败 | 保留兜底 mip/LOD，WARN |
| 反馈延迟 pop | 低 mip 兜底 + 渐进细化掩盖 |

**禁止**：无预算无限上传（卡带宽）；缺页黑/硬 pop。

## 8. 性能目标

- 上传带宽 ≤ 预算（可配，按平台）。
- 反馈→驻留延迟 1-2 帧，兜底无视觉黑。
- 内容量 ≫ 显存仍流畅（开放世界核心能力）。

## 9. 测试计划

- **单元**：页表/反馈正确、LRU 驱逐、优先级排序、预算限速。
- **渐进**：飞行相机快速移动，无黑/无硬 pop（渐进细化）。
- **预算门禁**：上传不超预算（监控）。
- **联动**：与 residency/Memory 预算一致（OOM 不崩）。

## 10. 开放问题 / Spike

- VT 页大小/物理 atlas 尺寸 vs 反馈延迟。
- 虚拟几何 cluster 流式与 meshlet/Nanite 式 LOD 的关系（与 Geometry/VisBuffer 协调）。
- World Partition 预取策略（cell 提前量）。
- 反馈 readback vs GPU 端调度（减 CPU 往返）。

## 11. 依赖与被依赖

- **依赖**：asset-pipeline（加载/格式）、RHI（residency/staging/bindless）、Foundation（Jobs/Memory）、06-world（cell 流式）。
- **被依赖**：Render（VT 采样/几何 cluster）、World Partition。

