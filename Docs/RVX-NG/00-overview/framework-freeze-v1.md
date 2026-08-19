# RVX-NG · 北极星框架定稿 v1.0

**日期**：2026-06-27  
**状态**：框架定稿 · 北极星目标架构冻结 · v1.1 目标口径补丁

本文是 RVX-NG 北极星框架的定稿说明。它冻结的是“目标架构框架”：分层、模块边界、依赖方向、能力分级、跨切面约束、里程碑骨架和文档口径。它不冻结具体实现细节，也不代表当前引擎迁移方案已经开始。

> v1.1 只调整整体目标表述：目标从“一次性追逐完整终态引擎”收敛为“先建立可编译、可测试、可诊断、可扩展、可工具化的生产级 C++20 实时渲染/游戏引擎框架底座，再按 Proven / Advanced / Research 分级演进”。v1.1 不改变 v1.0 已冻结的分层、边界、依赖方向和能力分级。

## 1. 定稿结论

RVX-NG 北极星框架 v1.0 达到以下完成标准：

- 目标架构分层完整：Foundation、Data Layer、RHI、ShaderCompiler、Render、Asset、World、Feature、Tools、Shipping、Cross-cutting、App/Engine、Ecosystem。
- 每个主要模块都有 Tier-1 设计文档，覆盖目标、范围、接口、不变量、数据布局、算法、线程、失败语义、性能目标、测试计划、开放问题、依赖关系。
- 总控文档完整：设计宪章、最佳实践校准、依赖 DAG、能力分级、能力测试矩阵、架构决策 backlog、里程碑拆分、完成度矩阵、Spike 注册表。
- 北极星口径已冻结：当前实现不作为目标架构约束；实施与迁移方案另起；v1.1 将目标执行口径校准为“框架底座先闭环、生产能力分级兑现”。
- 高风险能力均分级处理：Proven 主线可出货，Advanced 可选，Research 可叠加且不阻塞主线。
- 关键跨切面已纳入框架：帧时序、内存预算、确定性、测试、构建、Replay、Save/Profile、Crash/Telemetry、本地化/无障碍、安全/反作弊、Platform Profile、Scalability、第三方合规。

## 2. 冻结项

以下内容在 v1.0 中冻结，后续修改必须走架构变更记录：

| 冻结项 | 定稿内容 |
|---|---|
| 文档定位 | RVX-NG 是北极星目标架构，不是迁移方案 |
| 分层结构 | Foundation → Data/RHI/Asset → World/Render/Feature → App/Tools/Shipping |
| 依赖方向 | 下层不知上层；Feature 不直接依赖 RHI；Render 只读 Extract 快照；工具/构建/网络/插件 ABI 边界按 Global DAG 冻结 |
| 数据模型 | 混合数据导向：热域 archetype/SoA，一等 authoring 门面 |
| 并行模型 | 全引擎统一 JobSystem/task graph；fiber 为可选后端 |
| 渲染边界 | ECS/World → Extract → RenderSnapshot → GpuScene |
| GPU 工作表达 | RenderGraph 是唯一 GPU pass/resource/barrier/alias/async 表达 |
| RHI 原则 | 薄、显式、能力诚实、bindless/timeline/indirect/AS 一等 |
| 几何主线 | meshlet GPU-driven 延迟；VisBuffer/软光栅为研究叠加 |
| GI 分级 | Probe/烘焙/SSGI 为 Proven 兜底；Radiance Cache 为 M6 目标能力/Advanced，S6 关闭后才可升格主线；ReSTIR 为 Research/高端档 |
| 资产原则 | Source asset 与 cooked runtime asset 分离；GUID/DDC/versioning 必备 |
| World 原则 | World Partition + data layer + 单一权威 spatial index |
| 项目生态 | Project/package/plugin manifest + resolved lock 是项目生态和 CI/Shipping 的事实输入 |
| Editor 原则 | Editor 与 runtime 同源，编辑真实 World，PIE 隔离 |
| 测试原则 | C10 可测性即架构；真实行为测试，不用源码字符串冒充验证 |
| Shipping 原则 | Shipping cook、PSO 预烘焙、Pak/VFS、Crash/Telemetry 纳入目标架构 |
| v1.1 目标口径 | 先交付可编译、可测试、可诊断、可扩展、可工具化的生产级框架底座，再按 Proven / Advanced / Research 分级兑现高端能力；不得把研究级终态写成首期硬承诺 |

## 3. 非冻结项

以下内容不在 v1.0 框架定稿中冻结，后续通过 Spike 或实施方案决定：

- 具体代码结构、类名、文件布局和模块拆包方式。
- 当前引擎 keep/adapt/rewrite 判断。
- JobSystem 默认是否启用 fiber。
- ECS 主存储最终采用 archetype、sparse-set 或混合实现细节。
- Radiance Cache 的 hash grid / clipmap 具体策略。
- Meshlet cook 工具、cluster 数据格式和 HLOD 过渡细节。
- DDC 后端、Pak 压缩/加密算法、patch delta 粒度。
- Editor UI 技术栈。
- Golden image 容差、GPU farm 和测试硬件矩阵。
- 第三方中间件选择：Physics、Audio、Text shaping、Scripting VM、Anti-cheat。
- 远程 package registry、online provider、source-control provider 和平台 SDK 具体接入顺序。
- 各平台 compliance waiver、license 审批流和商店运营流程。

## 4. Spike 与定稿关系

`spike-register.md` 中的 Spike 不阻止框架 v1.0 定稿，因为它们已经被约束在以下规则内：

- 每个 Spike 都有 Proven fallback 或明确的非主线定位。
- Spike 关闭前不得把相关能力升为 Proven。
- Spike 结果可以改变实现策略，但不得破坏 v1.0 冻结的分层和边界。
- 如果 Spike 结果要求改变冻结项，必须新增架构变更记录。

## 5. 架构变更规则

v1.0 之后，以下变更需要写入 ADR：

- 改变模块依赖方向。
- 改变某能力的 Proven / Advanced / Research 档位。
- 改变跨切面约束，如线程模型、内存预算、测试门禁、确定性边界。
- 将当前实现迁移需求反向写入北极星目标架构。
- 删除某个已冻结的 fallback。

ADR 必须包含：

- 背景与问题。
- 被修改的冻结项。
- 备选方案。
- 决策与理由。
- 对测试、里程碑、文档的影响。

## 6. 下一阶段输入

框架定稿后，下一阶段有两条并行线：

1. **Spike 验证线**：关闭 S0-S13，或把相关 fallback 固化为主线。
2. **实施方案线**：另起当前 RenderVerseX 的 keep/adapt/rewrite、迁移阶段、兼容层和工程排期。

实施方案不得直接修改北极星框架冻结项；如确有必要，先走 ADR。

## 7. 最终检查清单

| 检查项 | 状态 |
|---|---|
| README 指向所有主要文档 | 通过 |
| 主要模块 Tier-1 文档齐全 | 通过 |
| 总控文档齐全 | 通过 |
| 分层与依赖方向明确 | 通过 |
| 能力分级与 fallback 明确 | 通过 |
| 里程碑拆分到可评审子阶段 | 通过 |
| 实施/迁移内容后置 | 通过 |
| Spike 保留并不阻塞框架定稿 | 通过 |



