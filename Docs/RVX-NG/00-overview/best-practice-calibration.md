# RVX-NG · 最佳实践校准表

**日期**：2026-06-26  
**状态**：北极星决策校准 · v1.0 可评审

本文记录 RVX-NG 关键架构选择与现代游戏引擎最佳实践之间的对应关系。它不是竞品复刻清单，而是用于说明“为什么选择这个主线，为什么把另一些能力降为高端或研究档”。

## 1. 总体校准

| 领域 | 最佳实践趋势 | RVX-NG 取向 |
|---|---|---|
| 世界表示 | Authoring 对象模型 + 数据导向热路径共存 | 一等 entity/actor 门面 + archetype/SoA 内核 |
| 并行 | Task graph / job system 统一并行入口 | 工作窃取 task graph，fiber 可选 |
| 渲染边界 | Game/Render 解耦，快照/proxy 模型 | 单向 Extract 三缓冲快照 |
| GPU 工作表达 | Frame graph / RenderGraph 管理资源与 barrier | 单一 RenderGraph，真 async 与真 aliasing |
| 几何 | GPU-driven、meshlet/cluster、indirect | meshlet GPU-driven 延迟主线，VisBuffer 研究档 |
| 光照 | Clustered/tiled lighting + 分级 GI | clustered 延迟 + probe/radiance cache/ReSTIR 分档 |
| 阴影 | Virtualized shadow maps / RT shadow 混合 | Virtual Shadow Maps 主线 + RT 阴影 |
| 资源 | Offline cook、DDC、稳定 GUID、runtime compact format | Import/Cook/DDC/Registry/Versioning |
| 开放世界 | World partition、streaming、spatial index | cell 流式 + 单一权威 spatial index |
| 工具 | Editor 与 runtime 同源，真实世界编辑 | Editor 跑同一运行时，PIE 隔离 |
| 测试 | Golden、perf、fuzz、determinism、fault injection | C10 可测性即架构 |

## 2. 数据模型

**实践观察**：纯 ECS 对高规模数据有效，但 gameplay authoring、调试、工具体验通常需要对象/Actor/Prefab 门面。  
**RVX-NG 决策**：D1 混合数据导向。热域走 archetype/SoA；bespoke gameplay 与编辑体验走一等 authoring 门面。

**保留风险**：

- 门面与数据层双写同步复杂。
- 结构性变更和 prefab/serialization 需要严格边界。

**校准结论**：正确。比纯 ECS 更适合通用游戏引擎。

## 3. 并行模型

**实践观察**：现代引擎倾向统一 task graph，而不是各模块私有线程。Fiber 可改善直线写法，但调试、TLS、平台移植成本高。  
**RVX-NG 决策**：D2 task graph 主线，fiber 可选后端。

**必须关闭的 Spike**：

- 无 fiber 后端的 `Wait` API 不能假装支持直线 continuation。
- deterministic mode 需要固定切片和固定规约。

**校准结论**：方向正确，但 API 需要进一步收紧。

## 4. 渲染架构

**实践观察**：高质量实时渲染通常采用 RenderGraph 管理 pass、资源生命周期、barrier、async compute 与 transient memory。  
**RVX-NG 决策**：RenderGraph 是唯一 GPU 工作表达；FrameRenderer 只做编排。

**主线能力**：

- pass DAG 与死 pass 剔除。
- 自动 barrier。
- 真 async compute。
- transient resource aliasing。
- Graphviz/FrameDebugger 可视化。

**校准结论**：正确，且必须作为架构约束而非可选工具类。

## 5. 几何与可见性

**实践观察**：GPU-driven meshlet/cluster culling 是务实主线；VisBuffer/软光栅很强但实现风险高。  
**RVX-NG 决策**：meshlet GPU-driven 延迟主线；VisBuffer + 软光栅为 M12 研究档。

**主线能力**：

- instance + meshlet 双层剔除。
- 两阶段 HiZ。
- ExecuteIndirect / DispatchMesh。
- HLOD/Impostor。

**校准结论**：分级合理，避免被高风险几何前端绑架。

## 6. 光照与 GI

**实践观察**：单一 GI 技术很难覆盖所有平台和内容。主线应具备低档兜底、中档可出货、高档可叠加。  
**RVX-NG 决策**：Probe/烘焙/SSGI 为 Proven 兜底，Radiance Cache 为 M6 目标能力/Advanced，ReSTIR 为 Research/高端档。

**必须关闭的 Spike**：

- Radiance cache 世界缓存结构：hash grid vs clipmap。
- Screen probe 密度、追踪步进、temporal reuse。
- RT 不可用时的 SSGI/probe fallback。

**校准结论**：方向正确；M6 前必须关闭 Radiance Cache 方案，关闭前不得升为 Proven 主线。

## 7. Asset 与 DDC

**实践观察**：生产化引擎必须把 source asset 与 runtime asset 分离，依赖 cook、DDC、版本迁移和稳定 GUID。  
**RVX-NG 决策**：Import/Cook/DDC/Registry/Runtime/Streaming 分层。

**主线能力**：

- Runtime compact binary。
- mmap/GPU-ready 段。
- stable GUID。
- DDC content-addressed key。
- 版本迁移。

**校准结论**：正确。Asset pipeline 是北极星能否生产化的关键，不应晚于渲染太多。

## 8. World 与 Streaming

**实践观察**：开放世界需要 World Partition、cell streaming、data layer、spatial index、asset streaming 与 GPU residency 协作。  
**RVX-NG 决策**：World cell 负责逻辑空间，Asset streaming 负责资源，RHI residency 负责显存，Memory Budget 统筹。

**校准结论**：方向正确，但 cell 粒度、预取策略和实体 ID 稳定性是高风险点。

## 9. Tools 与 Editor

**实践观察**：成熟引擎编辑器不应是“假壳”。它必须编辑真实运行时数据，并共享渲染、资产、反射、World。  
**RVX-NG 决策**：Editor 与 runtime 同源，PIE 隔离，Inspector 经 Reflection，所有编辑操作进入 transaction。

**校准结论**：正确。Editor 设计要尽早约束 Reflection、Serialization、World 和 Asset。

## 10. Testing 与 Shipping

**实践观察**：引擎架构越复杂，越需要真实行为测试、性能门禁、golden、fuzz、fault injection 与 shipping cook 验证。  
**RVX-NG 决策**：C10 可测性即架构；Shipping cook、PSO 预烘焙、Package/VFS、Crash/Telemetry 都进入北极星范围。

**校准结论**：正确。测试和发行不能作为最后阶段补丁。



