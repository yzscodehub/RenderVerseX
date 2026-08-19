# RVX-NG 北极星引擎设计文档集（索引）

RenderVerseX 次世代引擎的北极星目标架构设计文档树，按层级管理。当前版本为 v1.1 目标口径调整，进入 Spike 验证、架构决策关闭与实施/迁移方案设计准备阶段。

本文档集先回答“一个面向长期演进、现代最佳实践校准、可验证的 RenderVerseX 目标引擎应该是什么样”。调整后的整体目标是：把 RenderVerseX 建设成一个以现代 RenderGraph、多后端 RHI 和清晰数据边界为核心的生产级 C++20 实时渲染/游戏引擎框架；首期先交付可编译、可测试、可诊断、可扩展、可工具化的框架底座，再把开放世界、高端 GI/RT、主机/移动/XR 和完整发行生态作为分级演进上限逐步兑现。当前代码库只作为经验教训来源，不作为目标架构的约束；迁移与实施方案在北极星设计完成后单独编写。

**当前状态**：RVX-NG 北极星框架 v1.0 已定稿；v1.1 只调整整体目标表述，不改变冻结的分层、边界和能力分级。后续进入 Spike 验证与实施/迁移方案设计。

- **Tier-0**：架构总纲（北极星）。
- **Tier-1**：每子系统 / 跨切面详细设计（接口面 + 数据布局 + 算法 + 线程 + 性能目标 + 测试 + 开放问题）。

> 北极星文档不讨论 keep/adapt/rewrite，不写当前实现替换路径，不把具体 legacy 名称写入目标设计。落地需另起实施与迁移评估。

## 子 spec 统一骨架（每份 Tier-1 都按此写）

`目标与范围 · 需求与约束(含数字) · 公共接口面(签名+不变量) · 数据结构与内存布局 · 核心算法 · 线程与内存模型 · 错误/失败语义 · 性能目标 · 测试计划 · 开放问题/Spike · 依赖与被依赖`

## 文档树与进度

图例：✅ 完成 · 🟡 进行中 · ⬜ 待写

### 00 总纲（Tier-0）
- ✅ [架构总纲](00-overview/00-architecture-overview.md) — 分层/决策 D1-D12/契约 C1-C10/里程碑 M0-M12/分级路线
- ✅ [北极星设计宪章](00-overview/north-star-charter.md) — 设计阶段边界/文档纯度/完成定义
- ✅ [最佳实践校准表](00-overview/best-practice-calibration.md) — 关键决策与行业实践对齐
- ✅ [设计完成度矩阵](00-overview/design-completion-matrix.md) — 模块状态/待审点/下一轮重点
- ✅ [Spike 注册表](00-overview/spike-register.md) — 高风险问题/实验输出/判定标准
- ✅ [全局依赖 DAG](00-overview/global-dependency-dag.md) — 模块边界/初始化顺序/公共头约束
- ✅ [能力分级与降级策略](00-overview/capability-tiering.md) — Proven/Advanced/Research/fallback 规则
- ✅ [北极星里程碑拆分](00-overview/milestone-breakdown.md) — M0-M12 可评审子阶段/验收门禁
- ✅ [北极星框架定稿 v1.0](00-overview/framework-freeze-v1.md) — 冻结项/非冻结项/Spike 关系/变更规则
- ✅ [ADR 模板](00-overview/adr-template.md) — 框架冻结项变更记录格式
- ✅ [能力测试矩阵](00-overview/capability-test-matrix.md) — capability/fallback/test gate/CI lane 映射
- ✅ [架构决策 Backlog](00-overview/architecture-decision-backlog.md) — 冻结决策/待 Spike 决策/实施期细化边界

### 01 Foundation（M0 地基）✅ 框架设计完成（部分待 Spike）
- ✅ [JobSystem](01-foundation/jobsystem.md) — task-graph 工作窃取 + 确定性子域 + fiber 可选
- ✅ [Memory](01-foundation/memory.md) — 帧竞技场/类型池/TLSF/分类预算/OOM
- ✅ [Reflection](01-foundation/reflection.md) — libclang codegen → C++26；序列化/编辑器/网络共用
- ✅ [Core 与基础工具](01-foundation/core-and-utilities.md) — Result/Handle/Name/Container/Math/RNG/Hash/Compress/Log/Config/Event/Time
- ✅ [Input](01-foundation/input.md) — action/axis 映射、设备抽象、上下文栈、录制
- ✅ [Platform & VFS](01-foundation/platform-vfs.md) — 窗口/线程/主机 SDK 接缝/虚拟文件系统

### 02 数据层（M1）✅ 框架设计完成（部分待 Spike）
- ✅ [ECS 存储与调度](02-data-layer/ecs.md) — archetype 列存、Query、Scheduler→task-graph、确定性子域
- ✅ [Authoring 门面与桥](02-data-layer/authoring-facade.md) — 一等 entity/actor 门面、门面↔数据层桥

### 03 RHI（M2）✅ 框架设计完成（部分待 Spike）
- ✅ [RHI 核心](03-rhi/rhi-core.md) — 设备/资源/队列/timeline/barrier/bindless/indirect/AS/query/VRS/residency + 主机接缝
- ✅ [ShaderCompiler](03-rhi/shadercompiler.md) — HLSL→SPIR-V→跨编译、permutation、异步 PSO 预编译

### 04 Render（M3-M7）✅ 框架设计完成（部分待 Spike）
- ✅ [RenderGraph](04-render/rendergraph.md) — pass/瞬态资源/自动 barrier/真 async/真别名
- ✅ [GpuScene & Extract](04-render/gpuscene-extract.md) — 持久化 bindless 场景表 + 三缓冲快照
- ✅ [Material System](04-render/material-system.md) — Material Type/Instance/Graph IR/permutation/runtime GPU 表
- ✅ [Geometry & Culling](04-render/geometry-culling.md) — meshlet GPU-driven 延迟、两阶段 HiZ、indirect
- ✅ [Lighting & GI](04-render/lighting-gi.md) — clustered 延迟、GI 分级（Probe/SSGI → Radiance Cache 目标能力 → ReSTIR）
- ✅ [Shadows](04-render/shadows.md) — 虚拟阴影图(VSM) + RT 阴影
- ✅ [RayTracing](04-render/raytracing.md) — AS 场景、RT 阴影/反射、降噪
- ✅ [PostProcess & Atmosphere](04-render/postprocess.md) — TAA/上采样/tonemap/HDR 输出、天空/体积/贴花
- ✅ [FrameRenderer](04-render/frame-renderer.md) — 多视图、分级、单图编排

### 05 Asset（M8）✅ 框架设计完成（部分待 Spike）
- ✅ [资产管线](05-asset/asset-pipeline.md) — Import/Cook 格式/DDC/Registry/版本迁移
- ✅ [流式](05-asset/streaming.md) — 虚拟纹理/几何、驻留预算、与 RHI residency 联动

### 06 World（M1+）✅ 框架设计完成
- ✅ [World Partition & Spatial](06-world/world-partition-spatial.md) — cell 流式、增量 BVH、层级、Prefab

### 07 Feature（M9）✅ 框架设计完成（部分待 Spike）
- ✅ [Physics](07-feature/physics.md) · ✅ [Animation](07-feature/animation.md) · ✅ [AI](07-feature/ai.md) · ✅ [Audio](07-feature/audio.md)
- ✅ [Networking](07-feature/networking.md) · ✅ [Scripting](07-feature/scripting.md) · ✅ [VFX/Particle](07-feature/vfx-particle.md)
- ✅ [Terrain/Water/Foliage](07-feature/terrain-water-foliage.md) · ✅ [UI](07-feature/ui.md)
- ✅ [Camera & Cinematic](07-feature/camera-cinematic.md) · ✅ [Online & Platform Services](07-feature/online-platform-services.md)
- ✅ [Gameplay Framework Seams](07-feature/gameplay-framework-seams.md) · ✅ [Media & Video Playback](07-feature/media-video-playback.md)

### 08 Tools（M10）✅ 框架设计完成（部分待 Spike）
- ✅ [Editor](08-tools/editor.md) — 引擎客户端、PIE、反射 inspector、材质图、Sequencer
- ✅ [Profiler & Debug](08-tools/profiler-debug.md) — CPU/GPU 时间轴、FrameDebugger、RenderGraphViz
- ✅ [Team Workflow & Source Control](08-tools/team-workflow-source-control.md) — checkout/lock/merge/pre-submit/团队资产协作

### 09 Shipping（M11）✅ 框架设计完成
- ✅ [打包与发行](09-shipping/packaging-shipping.md) — Pak/VFS、Shipping Cook、Patching/DLC、主机、崩溃/无障碍/反作弊

### 10 跨切面（贯穿）✅ 框架设计完成（部分待 Spike）
- ✅ [帧时序与同步](10-cross-cutting/frame-timing-sync.md) — 流水线深度(D12)、fence、缓冲轮换、低延迟模式
- ✅ [内存与预算](10-cross-cutting/memory-budget.md) — 按平台/类别预算、OOM、显存↔流式
- ✅ [确定性](10-cross-cutting/determinism.md) — 确定性子域(D11)、定点/有序、回放/回滚
- ✅ [测试与可测性](10-cross-cutting/testing.md) — 单元/集成/golden/性能/确定性回归/fuzz
- ✅ [构建系统](10-cross-cutting/build-system.md) — 模块定义、依赖 DAG、构建配置、CI 门禁
- ✅ [Replay 录制回放](10-cross-cutting/replay-recording.md) — 输入日志/状态快照/回放/分歧诊断
- ✅ [存档、Profile 与用户数据](10-cross-cutting/save-profile-userdata.md) — SaveGame/Profile/Settings/cloud conflict/schema migration
- ✅ [崩溃与遥测](10-cross-cutting/crash-telemetry.md) — minidump/trace ring/telemetry/privacy
- ✅ [本地化与无障碍](10-cross-cutting/localization-accessibility.md) — 文本/字体/字幕/重绑定/读屏接缝
- ✅ [安全与反作弊接缝](10-cross-cutting/security-anti-cheat.md) — 沙箱/验签/网络验证/安全事件
- ✅ [平台 Profile / XR / Mobile / Web](10-cross-cutting/platform-profiles-xr-mobile-web.md) — profile/caps/budget/lifecycle/stereo 接缝
- ✅ [Scalability & Quality Profiles](10-cross-cutting/scalability-quality-profiles.md) — quality tier/scalability group/budget pressure/dynamic degrade
- ✅ [第三方与许可证合规](10-cross-cutting/third-party-license-compliance.md) — third-party manifest/SBOM/license/vulnerability/attribution

### 11 App / Engine（装配层）✅ 框架设计完成
- ✅ [App / Engine 装配层](11-app/app-engine.md) — 启动/模块/子系统生命周期/应用模式/主循环

### 12 Ecosystem（项目生态）✅ 框架设计完成
- ✅ [Project / Package / Plugin](12-ecosystem/project-package-plugin.md) — project manifest/package/plugin/template/lock/feature set

## 产出顺序

按依赖序：Foundation → 数据层 → RHI → Render → Asset/World → Feature → Tools/Shipping → 跨切面收口。v1.1 目标口径调整后的下一阶段是 S0-S13 Spike 关闭、架构决策 backlog 收口与实施/迁移方案设计；当前引擎实施/迁移方案仍然后置并单独成文。
