# RVX-NG · 能力分级与降级策略

**日期**：2026-06-26  
**状态**：北极星能力分级 · v1.0 可评审

本文定义 RVX-NG 的能力分级规则：首期主线必须能稳定出货，高端能力可选，研究级能力不得阻塞主线。每个能力的测试门禁映射见 `capability-test-matrix.md`。

## 1. 分级定义

| 档位 | 含义 | 进入条件 |
|---|---|---|
| Proven | 主线能力，默认开启，必须有完整测试和 fallback | 技术成熟、可跨目标平台、性能可预算 |
| Advanced | 高端能力，可按平台/质量档开启 | 已有可行原型，但平台覆盖或成本不均 |
| Research | 研究级叠加，不阻塞任何主线里程碑 | 风险高、实现成本高、可能被替代 |

## 2. 全局规则

- 每个 Research 能力必须有 Proven fallback。
- 每个 Advanced/Research 开关必须体现在 config、FrameStats、Profiler 和测试中。
- 不支持的能力必须显式降级并记录原因，不允许静默跳过。
- Shipping profile 必须知道每个目标平台的能力矩阵。
- 质量档切换不得要求重启，除非该能力明确标记为 startup-only。

## 3. 渲染能力

| 领域 | Proven | Advanced | Research |
|---|---|---|---|
| 几何 | meshlet GPU-driven 延迟、ExecuteIndirect | Mesh Shader 路径 | VisBuffer + 软件光栅、GPU Work Graphs |
| 剔除 | Frustum + 两阶段 HiZ | GPU-driven LOD/HLOD | Work Graph 生成 draw |
| 光照 | Clustered 延迟 | 面光源 LTC、Forward+ 透明复用 | 更复杂的多散射/实时路径采样 |
| GI | Probe/烘焙/SSGI 兜底 | Radiance Cache 混合路径（S6 后可升格） | ReSTIR GI |
| 阴影 | Virtual Shadow Maps | RT shadow 混合 | Path traced shadow experiments |
| 反射 | SSR/probe fallback | RT reflection + denoise | ReSTIR/path sampled reflection |
| 材质 | 固定 PBR shading models | Clearcoat/SSS/anisotropy | Substrate 式分层 BSDF |
| 后处理 | TAA/tonemap/HDR | FSR/XeSS/DLSS 抽象 | Frame generation / neural post |

## 4. Runtime 能力

| 领域 | Proven | Advanced | Research |
|---|---|---|---|
| 并行 | Task graph + work stealing | Deterministic fixed slicing | Fiber backend |
| 数据层 | Archetype/SoA hot path + authoring facade | 多世界并行 | ECS hot reload schema mutation |
| 资产 | Offline cook + DDC + GUID | Distributed cook/DDC | Fully GPU-driven streaming decisions |
| World | Cell streaming + spatial BVH | Runtime data layers | Procedural world generation |
| Physics | Fixed-step rigid body/character | Ragdoll/vehicle/cloth | Deterministic cross-platform physics for all bodies |
| Animation | Blend tree/state machine/GPU skinning | IK/retarget/motion matching | ML-driven animation synthesis |
| AI | NavMesh/path/behavior/perception | Crowd LOD | ML agent runtime |
| Networking | Server authoritative snapshot | Prediction/rollback/lag compensation | Large-scale replication graph research |
| Online | Null/offline provider + platform auth/session abstraction | Crossplay/session/presence/cloud save providers | Advanced matchmaking/rich social graph |
| Scripting | Sandboxed VM + reflection binding | Hot reload state migration | Visual scripting parity |
| Camera | Camera stack/ViewSet/blend/modifier | Photo mode/cinematic lens/focus | XR late-latched camera research |
| Gameplay Framework | GameInstance/GameRules/Player/Controller 接缝 | Lightweight gameplay tags/messages | Full built-in ability framework |
| Media | Cooked media playback + subtitles | Hardware decode/platform profiles | Protected/DRM media pipeline |

## 5. Tools/Shipping 能力

| 领域 | Proven | Advanced | Research |
|---|---|---|---|
| Editor | Inspector, viewport, transactions, PIE | Material/VFX graph, Sequencer | Multi-user collaboration |
| Profiler | CPU/GPU timeline, memory, CVars | Frame capture, RenderGraphViz | Automated bottleneck diagnosis |
| Packaging | Pak/VFS, manifest, platform chunks | Binary patch/DLC | Cloud streaming content |
| Crash/Telemetry | Minidump + trace ring | Symbol backend integration | Automated recovery/safe mode tuning |
| Project/Package | Project manifest + resolved lock + local packages | Remote registry + feature sets/templates | Marketplace-like distribution |
| Team Workflow | Checkout/lock/pre-submit validation | Structured asset merge/provider integrations | Realtime multi-user editing |
| Platform Profile | Desktop/console profile + explicit caps | Mobile/XR/Web profile implementations | Cloud/device-adaptive profile generation |
| Scalability | Static quality profiles + manual settings | Dynamic resolution/budget pressure auto-degrade | ML/device-adaptive quality tuning |
| Compliance | Third-party manifest + SBOM + license gate | Advisory integration/auto-upgrade policy | Automated risk remediation |

## 6. 降级语义

| 情形 | 降级规则 |
|---|---|
| RHI caps 不支持 | 使用下一级技术，并在 stats 中记录 |
| 预算超限 | 优先降 Research，再降 Advanced，最后调整 Proven 质量参数 |
| 资产未驻留 | 使用低 mip/低 LOD/proxy/fallback asset |
| shader/PSO 缺失 | Development 可后台编译；Shipping 走 fallback 并记录严重错误 |
| 测试环境缺能力 | SKIP 不是 PASS；能力测试必须在支持环境运行 |

## 7. 验收规则

- Proven 能力必须有单元/集成/性能测试。
- Render Proven 能力必须有至少一个 golden fixture。
- Advanced 能力必须有开关测试和降级测试。
- Research 能力必须证明关闭后主线仍可出帧/可运行。
- 每个质量档必须导出 FrameStats 或同等可观测状态。




