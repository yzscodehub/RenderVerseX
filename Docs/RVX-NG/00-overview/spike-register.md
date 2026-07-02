# RVX-NG · Spike 注册表

**日期**：2026-06-26  
**状态**：北极星风险账本 · v1.0 待验证

本文记录 RVX-NG 进入实施方案前必须关闭或明确降级的关键 Spike。每个 Spike 必须有问题、实验输出和判定标准。

## S0 · JobSystem continuation vs fiber

- **问题**：无 fiber 后端是否能提供 worker 内非阻塞 `Wait`，API 应如何表达 continuation。
- **输出**：API 决策、微基准、调试/profiler 影响报告。
- **判定**：默认后端无直线阻塞式 `Wait` 歧义；fiber 后端作为可选能力不污染契约。
- **阻塞**：M0。

## S1 · ECS archetype vs sparse-set

- **问题**：主数据层采用 archetype SoA 是否适合 RVX-NG 的结构变更、流式和 authoring 负载。
- **输出**：1M entity 迭代、结构变更 churn、cell load/unload、query cache 对比。
- **判定**：选择主存储方案，并给出组件设计规范防 archetype 爆炸。
- **阻塞**：M1。

## S2 · Reflection codegen toolchain

- **问题**：libclang codegen 的增量构建速度、跨平台稳定性和注解语法。
- **输出**：最小 codegen 原型、增量构建数据、错误诊断样例。
- **判定**：生成元数据可稳定驱动 serialization/editor/network。
- **阻塞**：M0/M1。

## S3 · RHI capability honesty matrix

- **问题**：D3D12/Vulkan/Metal3/console 接缝下 bindless、timeline、AS、mesh shader、VRS、residency 如何分级。
- **输出**：能力矩阵、fallback 表、capability test fixtures。
- **判定**：每个 caps 位都有可执行测试，不支持则明确 false。
- **阻塞**：M2。

## S4 · RenderGraph async/aliasing policy

- **问题**：哪些 pass 放 async compute 真有收益，aliasing interval coloring 使用何种启发式。
- **输出**：典型帧图原型、barrier/alias stats、GPU timing。
- **判定**：async/aliasing 的使用有 stats 证明，fallback 诚实。
- **阻塞**：M3。

## S5 · Meshlet cook and GPU culling

- **问题**：meshlet 构建工具、cluster 数据格式、两阶段 HiZ、HLOD/impostor 切换如何定型。
- **输出**：cook 原型、GPU culling fixture、CPU reference compare、性能基线。
- **判定**：M4 主线无需 VisBuffer 即可稳定出 G-Buffer。
- **阻塞**：M4。

## S6 · Radiance cache GI

- **问题**：Radiance cache 使用 hash grid、clipmap 还是混合结构；screen probe 如何采样和复用。
- **输出**：GI 原型、RT on/off fallback、噪声/稳定性/性能数据。
- **判定**：主线 GI 能降级到 probe/SSGI，不依赖 ReSTIR。
- **阻塞**：M6。

## S7 · Virtual texture / virtual geometry feedback

- **问题**：反馈 readback 还是 GPU 端调度，页大小、atlas、cluster 池如何配置。
- **输出**：快速相机飞行测试、缺页可视化、预算曲线。
- **判定**：未驻留不黑屏，反馈到驻留延迟在目标范围内。
- **阻塞**：M8。

## S8 · Runtime asset format and DDC

- **问题**：运行时二进制格式、版本迁移、DDC key、共享缓存后端如何设计。
- **输出**：runtime format spec、迁移样例、DDC 命中测试。
- **判定**：source asset 与 runtime asset 完整分离。
- **阻塞**：M8。

## S9 · Physics determinism boundary

- **问题**：物理中间件能否满足确定性子域需求，哪些玩法必须使用定点/自研路径。
- **输出**：多线程/多平台 fixed-step replay hash、误差报告。
- **判定**：明确 physics deterministic subset 与非确定性边界。
- **阻塞**：M9 Physics/Networking。

## S10 · UI text shaping and localization

- **问题**：CJK/RTL/fallback font/emoji/IME 支持使用哪些库和 atlas 策略。
- **输出**：文本 golden、atlas miss 行为、输入法测试。
- **判定**：UI 可以作为游戏和工具共同文本系统。
- **阻塞**：M9 UI / M10 Editor。

## S11 · Editor PIE world fork

- **问题**：PIE 使用复制 world、copy-on-write 还是多 world fork。
- **输出**：内存成本、状态隔离测试、事务回滚测试。
- **判定**：PIE 不能污染编辑世界，且大型场景成本可接受。
- **阻塞**：M10。

## S12 · Golden image tolerance and GPU farm

- **问题**：跨后端 golden 如何在“严格发现真实回归”和“容忍 GPU/driver 浮点微差”之间定标；GPU farm 如何分层，避免把全部后端矩阵压到 PR 门禁。
- **输入**：一组代表性 fixture（纯色/HDR/几何边缘/阴影/GI/透明/TAA 稳态）、至少一个 reference GPU lane、一个非 reference backend lane、RenderGraph capture、driver/GPU metadata。
- **实验**：对同一 fixture 采集多轮 reference 输出和跨 backend 输出，比较逐像素、通道容差、HDR 亮度容差、区域 mask、SSIM/感知指标组合的误报/漏报；同时验证 artifact bundle 是否足够定位 pass/resource。
- **输出**：golden policy v0、tolerance profile 表、reference/promotion 流程、GPU farm lane 分层、失败 artifact schema、fixture 最小集。
- **判定**：PR smoke 能稳定发现明显渲染错误；nightly reference lane 对同硬件重复运行稳定；跨 backend lane 不因已知微差高频误报；每次失败都有可复现 seed/profile/capture/diff。
- **Fallback**：未关闭前，主线使用 headless/contract + 少量 reference GPU smoke；跨 backend golden 只作为趋势/诊断，不作为硬阻断。
- **阻塞**：M10/C10。

## S13 · Build meta-manifest

- **问题**：模块 manifest 已作为目标架构约束事实源；需要验证 schema、后端生成/导出策略、include graph、profile 裁剪、codegen/cook/test discovery 如何进入同一可验证构建图。
- **输入**：合成模块图（Foundation/RHI/Render/Asset/Feature/Editor/Tool/Test/Shipping/Plugin）、public/private 依赖样例、codegen/shader/cook/test 节点样例、Debug/Development/Test/Shipping profile。
- **实验**：编写 manifest schema v0，生成后端构建图，运行 DAG 校验、public header include audit、Shipping 裁剪、codegen 增量、shader/cook cache key、test discovery、impact selection。
- **输出**：manifest schema v0、graph validator 原型、后端生成/导出策略、profile policy、include audit 报告格式、generated node registry、最小 CI gate 定义。
- **判定**：依赖环、private leak、Shipping→Editor 依赖、陈旧 codegen、runtime shader compile、source asset 打包都能自动失败；局部改动能映射到受影响 build/test 节点；后端生成策略不破坏 manifest 事实源。
- **Fallback**：若 manifest 生成完整后端成本过高，仍保留 manifest 作为架构约束事实源，并要求构建后端导出等价 graph 供 validator 比对。
- **阻塞**：M0/M11。




