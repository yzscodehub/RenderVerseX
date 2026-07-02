# RVX-NG · Render/FrameRenderer 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §9 FrameRenderer、决策 D12、契约 C6
**里程碑**：M3（骨架）→ 随各 pass 完善
**状态**：详细设计 · v1.0 可评审
**地位**：渲染编排者——把所有 pass 组装为**单一 RenderGraph**，统一多视图、可伸缩分级。

---

## 1. 目标与范围

**目标**：消费 Extract 快照，编排多视图（主/阴影/反射/探针）的 pass 链为一张 RenderGraph，按 ScalabilitySettings 分级，单图提交。

**范围内**：视图管理、pass 编排（剔除→几何→阴影→光照→RT→后处理→UI）、分级 tier 应用、多视图调度、帧时序对接（D12）。
**范围外**：各 pass 内部（已分别设计）、图执行（RenderGraph）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 单图 | 全部 pass 进一张 RenderGraph（无手写旁路） |
| 多视图 | 主视图 + N 阴影视图 + 反射/探针视图统一编排 |
| 分级 | tier（low→ultra）选择 pass 集与质量（C6）；研究级档可开关 |
| 流水线 | 对接 D12 可配流水线深度（吞吐/低延迟模式） |
| 诚实 | 不支持的 pass 显式跳过 + 状态可查（非静默） |

## 3. 公共接口面（设计契约）

```cpp
struct View { Camera cam; ViewType type; /*Main/Shadow/Reflection/Probe*/ Rect vp; };

class FrameRenderer {
    void Initialize(IDevice*, const ScalabilitySettings&);
    // 每帧：消费快照 → 编排 → 执行
    void Render(const RenderSnapshot&, ISwapchain*);
    void SetScalability(const ScalabilitySettings&);  // 运行时切档
    void EnableResearchTier(ResearchFeature, bool);   // VisBuffer/ReSTIR/...（M12 开关）
    const FrameStats& Stats() const;                  // 每 pass 状态/计时（诚实）
};

struct ScalabilitySettings {
    GiTier gi; ShadowQuality shadow; UpscaleMode upscale;
    bool rtReflections; u32 maxLights; /* ... */
};
```

**不变量**：所有 GPU 工作经单一 RenderGraph（无手写旁路）；不支持/关闭的 pass 在 `FrameStats` 显式标记跳过原因（诚实，C6）；多视图共享 GpuScene/AS，剔除按视图。

## 4. 数据结构与组织

- **视图列表**：本帧所有 View（主 + 阴影 clipmap/页视图 + 反射 + 探针更新）。
- **Pass 注册表**：按依赖序的 pass 工厂；tier 决定启用集。
- **帧计划**：组装好的 RenderGraph + 每视图的剔除/绘制子图。
- **FrameStats**：每 pass 启用/跳过/计时（GPU query），喂 Profiler/HUD。

## 5. 核心算法（一帧编排）

```
消费 Extract 快照 → GpuScene.Flush
for 各视图: GpuCulling(两阶段) → GeometryPass(间接 G-Buffer)
主视图:
  ShadowPass(VSM 脏页 + RT 阴影) → ShadowComposite
  ClusteredLighting(cluster+cull) → DeferredShading(+阴影+GI)
  RayTracing(AS build → RT 反射 + 辐照缓存 GI) → 降噪
  Atmosphere/Volumetrics → Decals → Transparent(Forward+)
  PostProcessStack(TAA/上采样/tonemap/HDR) → UI
→ RenderGraph.Compile → Execute(gfx, asyncCompute) → Present
```
- **分级应用**：tier 决定 GI 档/阴影质量/上采样器/RT 开关/research 档。
- **多视图**：阴影/反射视图复用剔除+几何子图，主视图全链。

## 6. 线程与内存模型

- 编排（建图）单线程；pass execute 多 worker 并行录制（RenderGraph）。
- 提交对接 D12：sim/render 阶段重叠，深度可配；Present 按平台亲和（JobSystem RunOnMainThread）。
- 跨视图共享 GpuScene/AS/瞬态堆。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| pass 不支持（caps） | 跳过 + `FrameStats` 标原因（诚实，非静默） |
| research 档关闭 | 走 proven 主线（VisBuffer→meshlet、ReSTIR→辐照缓存） |
| 视图过多（阴影/反射） | 按预算限视图数 + 分帧更新（探针 amortize） |
| 快照缺失/无效 | 跳过帧或上一快照（不崩） |

**禁止**：手写 pass 旁路；静默跳过 pass（必入 Stats）。

## 8. 性能目标

- 编排开销小（建图 < 0.2 ms，见 RenderGraph）。
- 多视图复用剔除/几何子图，避免重复工作。
- 切档/开关 research 档无重建卡顿（PSO 预编译覆盖）。

## 9. 测试计划

- **编排正确**：pass 顺序/依赖、多视图剔除复用。
- **分级**：每 tier 输出 golden；档间切换无崩/无 hitch。
- **诚实门禁**：跳过的 pass 必在 `FrameStats` 记录原因（防静默回归）。
- **research 开关**：VisBuffer/ReSTIR 开关下主线不受影响。
- **端到端 golden**：固定 fixture 场景全链 golden（硬门禁）。

## 10. 开放问题 / Spike

- 多视图/阴影/反射的图合并与并行度。
- 探针更新的 amortize 策略（每帧更新子集）。
- tier 自动调节（动态分辨率/质量随帧时）。
- 与 D12 流水线深度的耦合点。

## 11. 依赖与被依赖

- **依赖**：全部 Render 子系统、RenderGraph、GpuScene/Extract、RHI、Foundation。
- **被依赖**：Engine 主循环（每帧调用）、Editor（视口渲染）、Profiler（FrameStats）。



