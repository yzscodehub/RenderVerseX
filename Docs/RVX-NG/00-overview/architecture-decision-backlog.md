# RVX-NG · 架构决策 Backlog

**日期**：2026-06-27  
**状态**：v1.0 框架治理输入 · v1.1 目标口径已登记

本文记录 RVX-NG 北极星框架中的架构级决策。它用于区分三类事项：

- **已冻结**：属于 v1.0 框架边界，后续修改必须走 ADR。
- **待 Spike**：方向已有 fallback，但需要实验关闭才能升格或进入实施方案。
- **实施期细化**：不改变框架边界，由实施方案或子系统设计决定。

## 1. 已冻结决策

| ID | 决策 | v1.0 规则 | 后续变更 |
|---|---|---|---|
| AD-001 | 插件 ABI | 跨 DLL/动态库边界只传 C ABI、POD descriptor、opaque handle 或版本化接口；禁止 STL/第三方 SDK 类型/allocator ownership 外泄 | ADR |
| AD-002 | ShaderCompiler 归属 | ShaderCompiler 是 Build/Cook 与 Development runtime 共用的工具服务；不依赖 Render pass/material 业务类型；Shipping 只消费 cooked shader/PSO artifact | ADR |
| AD-003 | Networking transport | Platform 提供 socket/平台网络 primitive；Networking Feature 拥有 session、replication、prediction、rollback、gameplay protocol | ADR |
| AD-004 | Editor graph compiler | Editor graph 只产版本化 IR/source asset；Build/Cook 编译 IR 到 runtime artifact；Runtime 不依赖 Editor graph UI | ADR |
| AD-005 | 依赖注入边界 | ModuleGraph/App 负责显式装配；运行时域模块不使用全局 service locator，只允许 composition layer 的 typed subsystem lookup | ADR |
| AD-006 | Commandlet/Cook | Commandlet 是 headless AppMode/工具入口；生产 cook 可独立进程运行，但必须复用同一 Build/Cook graph 与 manifest 约束 | ADR |
| AD-007 | GI 升格规则 | Probe/烘焙/SSGI 是 Proven 兜底；Radiance Cache 在 S6 关闭前保持 Advanced/M6 目标能力，不作为 Proven 主线 | ADR |
| AD-008 | Build manifest | 模块 manifest 是目标架构约束事实源；S13 验证 schema、后端生成/导出、include audit、profile 裁剪和 CI 门禁 | ADR |
| AD-009 | Project/package lock | CI/Shipping 只允许使用 resolved project lock；package 安装不等于运行时启用，必须经 profile/feature set 显式选择 | ADR |
| AD-010 | Platform provider 隔离 | Online/Platform SDK 类型只存在 provider 私有实现；gameplay 使用 OnlineServices 抽象，不直接调用平台 SDK | ADR |
| AD-011 | Third-party compliance gate | Unknown license/source/hash、forbidden license、high vulnerability、SBOM 缺失在 Test/Shipping profile 阻断 | ADR |
| AD-012 | v1.1 目标口径 | RVX-NG 的实施目标是先建立可编译、可测试、可诊断、可扩展、可工具化的生产级 C++20 实时渲染/游戏引擎框架底座；完整开放世界、高端 GI/RT、主机/移动/XR、发行生态等作为分级演进上限，不作为首期一次性交付承诺 | ADR |

## 2. 待 Spike 决策

| ID | 关联 Spike | 决策问题 | 关闭标准 |
|---|---|---|---|
| AD-S0 | S0 | Job continuation API 与 fiber 后端价值 | 默认后端无 worker blocking 语义歧义；fiber 不污染契约 |
| AD-S1 | S1 | ECS 主存储 archetype/sparse-set/混合 | 以目标工作负载数据决定，并给出组件设计规范 |
| AD-S3 | S3 | RHI capability 分级与 backend fallback | 每个 caps bit 有 fixture，unsupported 明确 false |
| AD-S4 | S4 | RenderGraph async/aliasing 策略 | GPU timing 与 alias stats 证明收益，fallback 可观测 |
| AD-S6 | S6 | Radiance Cache 结构与主线升格 | RT on/off fallback、稳定性、性能和噪声达标 |
| AD-S9 | S9 | 物理/网络确定性边界 | 多线程/多平台 replay hash 和误差边界明确 |
| AD-S10 | S10 | UI text shaping/font/IME 技术组合 | CJK/RTL/fallback font/IME golden 通过 |
| AD-S11 | S11 | PIE world fork 策略 | 编辑世界隔离、内存成本和事务回滚测试通过 |
| AD-S12 | S12 | Golden tolerance/GPU farm | PR/nightly/scheduled lane 分层稳定，artifact 可定位 |
| AD-S13 | S13 | Build manifest 工具链落地方式 | manifest schema、graph validator、include audit 和 profile gate 可运行 |

## 3. 实施期细化

以下事项不改变 v1.0 框架边界，进入实施方案或子系统技术方案处理：

- 具体第三方库版本和授权方案。
- 具体文件布局、类名、模块拆分和构建后端。
- DDC 后端、Pak 压缩/加密算法、patch delta 粒度。
- Runtime asset 二进制字段布局。
- Profiler trace 文件格式。
- 各平台 SDK 接入细节。
- 远程 package registry 运营方式。
- 首批 online provider 和 source-control provider。

## 4. 使用规则

- 如果实施方案要改变“已冻结决策”，先写 ADR。
- 如果 Spike 关闭结果只影响实现策略，不改变冻结边界，更新对应 Tier-1 文档和本 backlog。
- 如果发现新的架构级开放问题，先登记到本文，再判断是否需要新增 Spike 或 ADR。

