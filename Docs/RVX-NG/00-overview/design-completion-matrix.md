# RVX-NG · 北极星设计完成度矩阵

**日期**：2026-06-26  
**状态**：v1.0 框架定稿矩阵

本文用于追踪 RVX-NG 北极星目标架构的设计完成度。这里的“完成”表示达到可评审的框架设计深度，不表示已经实现，也不表示已经进入当前引擎迁移或实施方案阶段。

## 状态定义

| 状态 | 含义 |
|---|---|
| 未设计 | 只有索引或一句话范围，没有可评审内容 |
| 草案 | 已有目标、范围、主要接口和约束，但仍需补细节 |
| 可评审 | 覆盖目标、接口、不变量、数据布局、算法、线程、失败语义、性能、测试、开放问题、依赖 |
| 待 Spike | 框架设计方向明确，但关键技术取舍需要实验关闭或用 proven fallback 固化 |
| 可进入实施方案设计 | Spike 已关闭或 fallback 已定，接口与验收标准稳定，可作为后续实施方案输入 |

## 总体原则

- 北极星设计不受当前代码实现约束。
- 当前实现相关的 keep/adapt/rewrite 判断不进入本矩阵，后续迁移评估单独建立。
- 每个 Tier-1 文档都保留明确的开放问题 / Spike 列表，不用“待实现”掩盖设计缺口。
- 所有高风险能力必须有 proven fallback，不允许研究级能力阻塞主线。
- v1.0 的目标是“北极星框架定稿”：冻结分层、边界、依赖、能力分级和里程碑骨架；Spike 仍作为后续验证项，不代表进入实施排期。

## 模块矩阵

| 区域 | 文档 | 当前状态 | 主要待审点 |
|---|---|---:|---|
| 00 Overview | `00-architecture-overview.md` | 可评审 | 北极星分层、决策 D1-D12、契约 C1-C10 |
| 00 Overview | `north-star-charter.md` | 可评审 | 框架设计阶段边界与文档纯度 |
| 00 Overview | `best-practice-calibration.md` | 可评审 | 行业最佳实践对齐与 ambition 分级 |
| 00 Overview | `global-dependency-dag.md` | 可评审 | 模块依赖、初始化顺序、公共头边界 |
| 00 Overview | `capability-tiering.md` | 可评审 | Proven/Advanced/Research 与 fallback 规则 |
| 00 Overview | `capability-test-matrix.md` | 可评审 | capability 到 fallback、测试门禁、CI lane 的映射 |
| 00 Overview | `architecture-decision-backlog.md` | 可评审 | 冻结决策、待 Spike 决策、实施期细化边界 |
| 00 Overview | `milestone-breakdown.md` | 可评审 | M0-M12 目标架构验收拆分 |
| 01 Foundation | `jobsystem.md` | 待 Spike | S0 continuation vs fiber API 语义 |
| 01 Foundation | `memory.md` | 可评审 | CPU allocator 与全局 memory budget 对齐 |
| 01 Foundation | `reflection.md` | 待 Spike | S2 libclang codegen 增量、诊断、跨平台稳定性 |
| 01 Foundation | `core-and-utilities.md` | 可评审 | Result/Handle/Name/Math/RNG/Log/Config 契约冻结 |
| 01 Foundation | `input.md` | 可评审 | 输入录制、低延迟采样、action map 边界 |
| 01 Foundation | `platform-vfs.md` | 可评审 | 主机 SDK 接缝、VFS mount policy、平台抽象 |
| 02 Data | `ecs.md` | 待 Spike | S1 archetype vs sparse-set 工作负载验证 |
| 02 Data | `authoring-facade.md` | 可评审 | authoring 门面与数据层桥生命周期 |
| 03 RHI | `rhi-core.md` | 待 Spike | S3 capability honesty matrix 与 backend fallback |
| 03 RHI | `shadercompiler.md` | 可评审 | permutation、reflection、PSO 预烘焙与 DDC |
| 04 Render | `rendergraph.md` | 待 Spike | S4 async compute/aliasing policy |
| 04 Render | `gpuscene-extract.md` | 可评审 | Extract 快照、GpuScene 表、脏上传粒度 |
| 04 Render | `material-system.md` | 可评审 | Material Type/Graph IR/permutation/GPU 表/fallback |
| 04 Render | `geometry-culling.md` | 待 Spike | S5 meshlet cook、GPU culling、HLOD/impostor |
| 04 Render | `lighting-gi.md` | 待 Spike | S6 radiance cache GI 与 probe/SSGI fallback |
| 04 Render | `shadows.md` | 可评审 | VSM 页缓存、RT 阴影 fallback、动态预算 |
| 04 Render | `raytracing.md` | 可评审 | AS 更新预算、denoiser、RT off fallback |
| 04 Render | `postprocess.md` | 可评审 | TAA/upscale/HDR/color 管线抽象 |
| 04 Render | `frame-renderer.md` | 可评审 | 多视图编排、tier switch、pass skip reason |
| 05 Asset | `asset-pipeline.md` | 待 Spike | S8 runtime format/DDC；与 S5 meshlet cook 对齐 |
| 05 Asset | `streaming.md` | 待 Spike | S7 VT/VG feedback、驻留延迟、预算曲线 |
| 06 World | `world-partition-spatial.md` | 可评审 | cell 粒度、流式预取、空间索引与 prefab |
| 07 Feature | `physics.md` | 待 Spike | S9 physics deterministic subset 与中间件边界 |
| 07 Feature | `animation.md` | 可评审 | 压缩、motion matching、GPU skinning、LOD 门禁 |
| 07 Feature | `ai.md` | 可评审 | NavMesh tile、AI LOD、行为/感知预算 |
| 07 Feature | `audio.md` | 可评审 | 实时 mixer、voice budget、中间件窄接口 |
| 07 Feature | `networking.md` | 待 Spike | S9 rollback/prediction 与 server-authoritative 边界 |
| 07 Feature | `scripting.md` | 可评审 | VM/沙箱/热重载/绑定安全策略 |
| 07 Feature | `vfx-particle.md` | 可评审 | VFX graph、GPU simulation、sort、budget/fallback |
| 07 Feature | `terrain-water-foliage.md` | 可评审 | terrain VT、foliage GPU-driven、水体质量档 |
| 07 Feature | `ui.md` | 待 Spike | S10 text shaping、font atlas、UI 描述格式 |
| 07 Feature | `camera-cinematic.md` | 可评审 | Camera stack、ViewSet、cinematic、split-screen、Replay camera |
| 07 Feature | `online-platform-services.md` | 可评审 | account/session/presence/cloud save/store/platform SDK 隔离 |
| 07 Feature | `gameplay-framework-seams.md` | 可评审 | GameInstance/GameRules/Player/Controller/HUD bridge 接缝 |
| 07 Feature | `media-video-playback.md` | 可评审 | media asset、decode backend、A/V sync、video texture、subtitle cue |
| 08 Tools | `editor.md` | 待 Spike | S11 PIE world fork 与 Editor UI 技术栈 |
| 08 Tools | `profiler-debug.md` | 可评审 | Trace/capture 格式、FrameDebugger、性能基线 |
| 08 Tools | `team-workflow-source-control.md` | 可评审 | checkout/lock/merge/pre-submit/团队资产协作 |
| 09 Shipping | `packaging-shipping.md` | 可评审 | Pak/patch、PSO shipping cook、crash/telemetry 接缝 |
| 10 Cross-cutting | `frame-timing-sync.md` | 可评审 | fixed tick、pipeline depth、low latency、fence bridge |
| 10 Cross-cutting | `memory-budget.md` | 可评审 | budget profile、pressure、eviction、OOM fallback |
| 10 Cross-cutting | `determinism.md` | 待 Spike | S9 fixed-point/physics/network deterministic boundary |
| 10 Cross-cutting | `testing.md` | 待 Spike | S12 golden tolerance、GPU farm 分层、artifact policy |
| 10 Cross-cutting | `build-system.md` | 待 Spike | S13 manifest、include audit、profile 裁剪、generated nodes |
| 10 Cross-cutting | `replay-recording.md` | 可评审 | replay 文件、state hash、seek、fuzz 安全 reader |
| 10 Cross-cutting | `save-profile-userdata.md` | 可评审 | SaveGame/Profile/Settings/cloud conflict/schema migration |
| 10 Cross-cutting | `crash-telemetry.md` | 可评审 | minidump、trace ring、privacy、offline cache |
| 10 Cross-cutting | `localization-accessibility.md` | 待 Spike | S10 CJK/RTL/font fallback/IME 与平台无障碍矩阵 |
| 10 Cross-cutting | `security-anti-cheat.md` | 可评审 | capability policy、package signing、anti-cheat bridge |
| 10 Cross-cutting | `platform-profiles-xr-mobile-web.md` | 可评审 | Desktop/Console/Mobile/XR/Web profile、budget、lifecycle、caps |
| 10 Cross-cutting | `scalability-quality-profiles.md` | 可评审 | quality tier、scalability group、budget pressure、动态降级 |
| 10 Cross-cutting | `third-party-license-compliance.md` | 可评审 | third-party manifest、SBOM、license、vulnerability、attribution |
| 11 App | `app-engine.md` | 可评审 | AppMode、ModuleGraph、Subsystem lifecycle、safe mode |
| 12 Ecosystem | `project-package-plugin.md` | 可评审 | project/package/plugin manifest、lock、feature set、template |

## v1.0 后续重点

1. 对所有“可评审”模块做实施前一致性审查：接口边界、数据所有权、线程模型、测试门禁是否互相矛盾。
2. 对所有“待 Spike”模块执行 S0-S13：关闭技术取舍，或把 fallback 固化为 Proven 主线。
3. 做一次全局依赖 DAG、能力分级、里程碑拆分和 Tier-1 文档的交叉审查。
4. 评审通过后，另起“实施与迁移方案”文档；该方案不得反向修改北极星目标以迁就当前实现。



