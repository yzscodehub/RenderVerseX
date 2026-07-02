# RVX-NG · Render/Geometry & Culling 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §9 Geometry/Culling、决策 D5
**里程碑**：M4
**状态**：详细设计 · 待 S5 Spike
**地位**：D5 **首期主线 = meshlet GPU-driven 延迟**；VisBuffer 软光栅为 Phase-2/M12 可叠加前端（本文留接口，不实现）。

---

## 1. 目标与范围

**目标**：GPU 上完成两阶段 HiZ 遮挡剔除 + meshlet 级剔除，产出 indirect 参数，间接绘制写 G-Buffer——CPU 零逐物体提交。

**范围内**：meshlet/cluster 几何表示、两阶段 HiZ 剔除（frustum + occlusion）、LOD/HLOD/Impostor 选择、indirect G-Buffer 绘制、mesh shader 路径。
**范围外**：着色（Lighting）、场景表（GpuScene）、VisBuffer 软光栅实现（M12）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| GPU-driven | 剔除/draw 参数 GPU 生成，CPU 不逐物体 |
| 遮挡 | 两阶段 HiZ：避免 1 帧延迟的漏剔/过剔 |
| 粒度 | 实例级 + meshlet/cluster 级双层剔除 |
| LOD | GPU 选 LOD/HLOD/Impostor（距离/屏幕覆盖） |
| 提交 | ExecuteIndirect / DispatchMesh，draw 数由剔除结果定 |

## 3. 公共接口面（设计契约）

```cpp
class GpuCulling {
    // 两阶段剔除，产 indirect draw 参数（消费 GpuScene 表）
    struct Inputs { BufferHandle instances, meshes; TextureHandle hizPrev; View view; };
    struct Outputs { BufferHandle drawArgs, visibleMeshlets, drawCount; };
    void RecordPhase1(RgBuilder&, const Inputs&, Outputs&);   // frustum + 上帧 HiZ
    void RecordBuildHiZ(RgBuilder&, TextureHandle depth, TextureHandle& hiz);
    void RecordPhase2(RgBuilder&, const Inputs&, Outputs&);   // 重剔被遮后又可见者
};

class GeometryPass {
    // 间接绘制 meshlet → G-Buffer（mesh shader 或 vertex pull）
    void Record(RgBuilder&, const Outputs& cull, RgTexture gbuffer[], RgTexture depth);
};
```

**不变量**：剔除全在 GPU compute；`drawArgs`/`drawCount` 由 GPU 写、ExecuteIndirect 消费（CPU 不读回）；两阶段保证无 1 帧遮挡延迟的漏剔。VisBuffer 接口（`RecordVisBuffer`）预留但 M4 不实现。

## 4. 数据结构与内存布局

- **Meshlet**：~64-128 顶点/三角的簇；每 meshlet 存 bounds + 法线锥（cone）+ 顶点/索引偏移。
- **HiZ 金字塔**：depth 的 mip 链（max-reduction），用于保守遮挡测试。
- **Indirect 参数**：`DrawIndexedIndirectArgs[]` + atomic `drawCount`（GPU 写）。
- **可见 meshlet 列表**：phase 输出的紧凑 meshlet 索引（stream compaction）。
- **LOD 数据**：每 mesh 的 LOD 链 + HLOD 代理 + impostor 引用。

## 5. 核心算法（两阶段 HiZ，Nanite/UE 式但走传统栅格）

1. **Phase-1**：对所有实例做 frustum 剔除 + 用**上帧 HiZ**做遮挡测试 → 通过者展开 meshlet（cone + HiZ 剔除）→ stream compaction → indirect draw → 写 G-Buffer + depth。
2. **Build HiZ**：从本帧 depth 构建 HiZ 金字塔（max reduction，compute）。
3. **Phase-2**：对 phase-1 中**被上帧 HiZ 判遮挡**的实例，用**本帧 HiZ** 重测 → 之前误剔（实际可见）者补绘。
   - 两阶段消除"上帧遮挡数据"导致的漏剔/闪烁。
- **LOD 选择**：GPU 按屏幕覆盖/距离选 LOD/HLOD/Impostor。
- **mesh shader 路径**：可用时 `DispatchMesh` 直接 meshlet→图元，省 IA。

## 6. 线程与内存模型

- 全程 GPU compute + indirect draw；CPU 仅录制（无回读）。
- 与 RenderGraph async compute 协同：剔除 compute 可与上一阶段 gfx 重叠。
- HiZ/indirect/compaction buffer 为瞬态（RenderGraph 别名）。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| mesh shader 不支持 | 降级 vertex pull + ExecuteIndirect（caps 守门） |
| indirect count 溢出 | drawArgs buffer 上限 + 监控；超额裁剪 |
| 实例几何未驻留 | 剔除阶段跳过（GpuScene pending 标记） |
| HiZ 精度伪影 | 保守测试（max-reduction）确保不漏剔（宁过绘不漏） |

**禁止**：CPU 逐物体提交；回读 GPU 剔除结果到 CPU（破坏 GPU-driven）。

## 8. 性能目标

- 1M 实例 / 数千万 meshlet：剔除 compute < 1-2 ms（GPU）。
- 过绘率：两阶段后接近精确可见集。
- draw call：合并为少量 ExecuteIndirect（非每物体一 draw）。
- GPU culling 是几何主路径，而非可选旁路。

## 9. 测试计划

- **正确性**：剔除结果 vs CPU 参考可见集（无漏剔；过剔在保守界内）。
- **两阶段**：构造遮挡变化场景，验证无 1 帧漏剔/闪烁。
- **golden**：间接绘制 G-Buffer vs 朴素绘制逐像素一致。
- **性能门禁**：大场景剔除+绘制帧时基线。
- **降级**：无 mesh shader 后端走 indirect vertex pull，结果一致。

## 10. 开放问题 / Spike

- **VisBuffer + 软光栅（M12）**：微多边形的可见性缓冲 + compute 材质 resolve——接口预留。
- LOD/HLOD 过渡（dither vs 几何 morph）。
- GPU Work Graphs 替代 phase 链（research，M12）。
- meshlet 构建（离线 cook，与 Asset 协调）。

## 11. 依赖与被依赖

- **依赖**：GpuScene（实例/mesh 表）、RHI（indirect/mesh shader/compute）、RenderGraph、ShaderCompiler。
- **被依赖**：Lighting（消费 G-Buffer）、Shadows（剔除复用）、FrameRenderer。**M4 主线**。


