# RVX-NG · 北极星里程碑拆分

**日期**：2026-06-26  
**状态**：目标架构阶段拆分 · v1.1 目标口径校准

本文把总纲中的 M0-M12 拆成更小的可评审、可验收阶段。这里仍然不是实施排期；它定义的是目标架构应如何分阶段证明自身成立。v1.1 目标口径下，里程碑先证明生产级框架底座，再补生产闭环，最后叠加研究能力：M0-M3 是框架骨架闭环，M4-M8 是渲染/资产/世界主线闭环，M9-M11 是生产工作流闭环，M12 只能作为不阻塞主线的研究叠加层。

## 1. 拆分原则

- 每个子阶段必须产出可运行或可验证的行为。
- 每个子阶段必须有明确测试门禁。
- 每个高风险能力必须先有 spike，再作为后续实施方案设计输入。
- Proven 主线先闭环，Advanced/Research 后叠加。
- 不把当前引擎迁移步骤写入本拆分。
- 首期完成不按终态功能数量验收，而按边界、契约、诊断、fallback、自动化门禁和工具可消费 artifact 是否闭环验收。

## 2. M0 · Foundation 地基

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M0a Core Contracts | Result/Handle/Name/Span/Log/Config/Time 基础契约 | 单元测试、编码规范、公共头无环 |
| M0b Memory | Frame arena、pool、TLSF、category tracking | 分配/释放/OOM 注入、热路径 no-malloc 测试 |
| M0c JobSystem | Task graph、work stealing、ParallelFor、main-thread queue | 百万微 job、深依赖链、无 worker blocking |
| M0d Deterministic Jobs | 固定切片、固定规约、workerCount 一致 | 多线程数 state hash 一致 |
| M0e Reflection Codegen | 类型/字段/属性元数据、生成 glue | 组件注册、序列化、editor inspector fixture |
| M0f Platform/VFS/Input | window/input/VFS/config stack 基础 | 输入录制、VFS mount、平台抽象测试 |

**关键 Spike**：S0 Job continuation/fiber、S2 Reflection codegen、S13 Build manifest。

## 3. M1 · Data Layer / World Core

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M1a ECS Storage | Entity、archetype/SoA、query、command buffer | spawn/add/remove/query 压力测试 |
| M1b Scheduler | System read/write set → task graph | 冲突系统串行、无冲突并行、TSan |
| M1c Authoring Facade | entity/actor 门面、component bridge | 门面变更同步到数据层 |
| M1d Relation/Prefab | 层级、Prefab、serialization 接缝 | 层级 dirty propagation、prefab instantiate |
| M1e World Partition Seed | cell id、data layer、streaming 生命周期骨架 | cell load/unload 和实体 ID 稳定性测试 |

**关键 Spike**：S1 ECS storage。

## 4. M2 · RHI / Shader 地基

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M2a Device/Caps | adapter、device、caps honesty matrix | caps bit 可执行验证 |
| M2b Resources/Barriers | buffer/texture/view、explicit barriers | barrier/state 单元与后端验证 |
| M2c Queues/Timeline | graphics/compute/copy queue、timeline sync | 跨队列 wait/signal 测试 |
| M2d Bindless/Descriptors | global descriptor heap、index lifetime | register/unregister/失效测试 |
| M2e Indirect/Mesh Shader | ExecuteIndirect、DispatchMesh fallback | indirect draw golden |
| M2f RT/AS | BLAS/TLAS、RT pipeline、fallback | DX12/Vulkan RT capability gate |
| M2g Residency | VRAM budget、priority、evict/retry | OOM/residency 注入 |
| M2h ShaderCompiler/PSO | HLSL pipeline、reflection、PSO cache | permutation、include invalidation、precompile |

**关键 Spike**：S3 RHI capability matrix。

## 5. M3 · RenderGraph / GpuScene / FrameRenderer

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M3a RenderGraph DAG | pass/resource DAG、dead pass cull | DAG/topology/error tests |
| M3b Barrier Compiler | subresource state tracking、merge | barrier golden/cross-backend validation |
| M3c Transient Aliasing | lifetime interval coloring、alias barrier | memory saving stats、alias correctness |
| M3d Async Compute | queue split、timeline sync、stats | async 真提交和 fallback 测试 |
| M3e Extract Snapshot | ECS → RenderSnapshot 三缓冲 | no World write, snapshot consistency |
| M3f GpuScene | persistent tables、dirty upload | 增量 vs 全量 golden |
| M3g FrameRenderer Skeleton | 单图编排、多视图、FrameStats | pass skip reason、tier switch |

**关键 Spike**：S4 RenderGraph async/aliasing。

## 6. M4 · Geometry 主线

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M4a Meshlet Cook | meshlet/cluster runtime format | cook correctness、bounds/cone tests |
| M4b GPU Culling | instance + meshlet cull、compaction | CPU reference compare |
| M4c HiZ Two Phase | prev/current HiZ 两阶段剔除 | 遮挡变化无漏剔/闪烁 |
| M4d Indirect GBuffer | draw args/count GPU 生成 | GBuffer golden |
| M4e LOD/HLOD/Impostor | GPU LOD selection | 过渡和预算测试 |

**关键 Spike**：S5 Meshlet cook and culling。

## 7. M5-M7 · Lighting / RT / Frame Completion

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M5a Clustered Lighting | cluster build + light cull | 多光压力、PBR golden |
| M5b VSM Shadows | virtual pages、cache、dirty budget | 动态阴影 golden、页预算 |
| M5c Atmosphere/Volumetrics/Decals | 天空、体积、贴花接入 RenderGraph | pass golden、质量档 |
| M5d Material Runtime | material type/instance、Graph IR cook、GpuScene material table | material fallback、layout reflection、PSO enumeration |
| M6a RT Scene | AS update、RT shadow/reflection | RT on/off fallback |
| M6b Probe GI | irradiance/reflection probe fallback | 低档 GI golden |
| M6c Radiance Cache GI | screen/world cache 目标能力 | 稳定性、性能、RT off fallback；通过 S6 后才可升格 |
| M7a Postprocess | TAA/upscale/tonemap/HDR | HDR/color tests |
| M7b Transparency/UI | Forward+ transparent、UI composite | 透明阴影/UI golden |
| M7c Camera/View Runtime | camera stack、ViewSet、split-screen、cinematic runtime | blend/modifier/ViewSet contract tests |

**关键 Spike**：S6 Radiance cache。

## 8. M8 · Asset / Streaming / World Scale

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M8a Runtime Format | compact binary、version header | load/migrate tests |
| M8b Registry/DDC | stable GUID、content-addressed cook | DDC hit/miss、GUID stability |
| M8c Async Asset Runtime | handle、refcount、fallback、hot reload | no blocking load、failure fallback |
| M8d Virtual Texture | feedback、page table、atlas | no black page、budget test |
| M8e Virtual Geometry | cluster streaming、residency | camera flythrough no hard pop |
| M8f World Streaming | cell load/unload + asset prefetch | open-world fixture |

**关键 Spike**：S7 VT/VG feedback、S8 Runtime format/DDC。

## 9. M9 · Feature Systems

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M9a Physics | fixed-step bodies/queries/interpolation | falling/collision/determinism |
| M9b Animation | clip/blend/state/GPU skinning | sampling/skinning golden |
| M9c AI | nav/path/behavior/perception LOD | path and behavior fixtures |
| M9d Audio | event/voice/mixer/streaming/spatial | no audio-thread block |
| M9e Networking | transport/replication/prediction | packet fuzz/reconciliation |
| M9f Scripting | sandbox/binding/hot reload | permission and reload tests |
| M9g VFX | GPU particle simulation/render | alive-list and render golden |
| M9h Terrain/Water/Foliage | terrain LOD/foliage/water | streaming and render tests |
| M9i UI | layout/text/input/accessibility | layout/text/input golden |
| M9j Camera/Cinematic | gameplay camera、camera modifiers、replay/photo camera | camera blend、invalid target fallback、replay camera |
| M9k Online Services | auth/session/presence/cloud save/platform SDK isolation | mock provider、session flow、privacy/token tests |
| M9l Gameplay Framework Seams | GameInstance/GameRules/Player/Controller/HUD bridge | join/spawn/possess/travel/headless server fixtures |
| M9m Media Playback | media asset、decode backend、A/V sync、video texture | state machine、sync drift、codec fallback、subtitle routing |

**关键 Spike**：S9 Physics determinism、S10 UI shaping。

## 10. M10 · Tools / Observability

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M10a Editor Shell | windows/layout/project/context | mode startup tests |
| M10b Inspector/Transactions | reflection edit + undo/redo | property roundtrip |
| M10c Viewport/PIE | runtime viewport、world fork | PIE isolation |
| M10d Asset Browser | registry thumbnails/reimport | async thumbnails |
| M10e Graph Editors | material/VFX/shader graph IR | cook/compile fixture |
| M10f Profiler | CPU/GPU/memory timeline | trace/capture tests |
| M10g FrameDebugger | RenderGraph/resource/barrier view | capture artifact |
| M10h Team Workflow | checkout/lock、asset merge、pre-submit validation | mock SCM、GUID conflict、merge policy tests |

**关键 Spike**：S11 PIE world fork、S12 Golden/GPU farm。

## 11. M11 · Shipping / Release

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M11a Build Profiles | Debug/Development/Test/Shipping裁剪 | dependency audit |
| M11b Package/VFS | pak/chunk/manifest/mount priority | hash and mount tests |
| M11c Shipping Cook | all assets + shader/PSO pre-bake | no runtime shader compile |
| M11d Patch/DLC | manifest diff/rollback | patch apply/fail tests |
| M11e Crash/Telemetry | minidump/trace/privacy | crash injection |
| M11f Security/Anti-cheat Hooks | signing/sandbox/network validation | fuzz and deny tests |
| M11g Localization/Accessibility | locale/font/input/subtitle | accessibility fixtures |
| M11h Save/Profile/UserData | slot、checkpoint、schema migration、cloud conflict | atomic write、corruption、migration、etag conflict |
| M11i Platform Profiles | Desktop/Console/Mobile/XR/Web caps、budget、package profile | profile/cook closure、suspend/resume、XR ViewSet |
| M11j Third-party Compliance | third-party manifest、SBOM、license、attribution | license gate、SBOM reproducibility、ABI leak audit |
| M11k Scalability Profiles | quality tier、scalability groups、budget pressure | profile validation、pressure injection、subscriber coverage |

## 12. M11x · Project Ecosystem

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M11x-a Project Manifest | project settings、feature set、dependency list | schema validation、lock reproducibility |
| M11x-b Package/Plugin | package manifest、plugin descriptor、ABI/version policy | dependency conflict、profile裁剪、safe mode |
| M11x-c Templates/Samples | starter project、sample content、CI preset | template create、GUID stability、sample cook |

## 13. M12 · Research Overlay

| 子阶段 | 目标 | 验收 |
|---|---|---|
| M12a VisBuffer | optional visibility buffer frontend | disabled path unchanged |
| M12b ReSTIR GI | optional high-end GI | fallback to radiance cache |
| M12c Substrate Material | layered BSDF research | fixed PBR remains valid |
| M12d GPU Work Graphs | optional work generation path | ExecuteIndirect fallback |
| M12e Fiber Backend | optional straight-line job wait | default backend unchanged |

**规则**：任何 M12 能力关闭后，M0-M11 主线必须仍可运行、可测试、可出帧。



