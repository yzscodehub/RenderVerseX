# RenderVerseX 引擎综合评估报告（完整版）

**日期**：2026-05-22
**状态**：已跟踪的审计快照。CI / CTest / presets / macOS ARM64 覆盖等流程项在 `engine-remediation` 分支已开始修复；精确构建命令和工作流以当前 `CMakePresets.json`、`.github/workflows/ci.yml` 为准，本文保留为问题证据和优先级背景。
**本文定位**：在 `2026-05-22-engine-code-quality-audit.md`（行级代码质量审查）之上，**合并**一份宏观架构/成熟度评估，并对两份结论中**相互冲突或未覆盖的高危项逐条 review 源码核实**，给出统一的、可执行的完整结论。原文档保留不动，本文为其超集（superset）。

**三个数据来源**：
1. **行级质量审查**（原文档）：4 个 Explore Agent 逐行查 bug + 4 个独立 Agent 复核，产出 61 项（7 P0 / 31 P1 / 23 P2），复核后误报率 ~18%。
2. **宏观架构审查**（本次）：6 个 architect Agent 按域并行评估设计成熟度、与业界最佳实践的差距。
3. **定向源码核实**（本次）：对两份报告冲突/未覆盖的高危项,直接读源码判定。证据见附录。

---

## 0. 本次核实相对原文档的关键修订（先读这张表）

> 这些是“继续 review 代码”的产出。每一条都带当前工作树的证据。

| 编号 | 原文档判定 | 核实后结论 | 证据 |
|---|---|---|---|
| **C-01** RefCounted memory_order | P0（高并发 UAF） | ⚠️ **争议，倾向误报** — `AddRef` 用 `relaxed`、`Release` 用 `acq_rel` 正是 libc++/Boost `intrusive_ptr` 的**教科书无锁引用计数惯用法**：增引用时调用者必已持有有效引用，无需 ordering；减引用用 acq_rel 保证析构可见性。**不构成 UAF**。建议从 P0 降级 | `Core/Include/Core/RefCounted.h:54-63` |
| **R-01** EnqueueAsyncJob 任务静默丢失 | P0（复核也确认） | ✅ **当前树已不可复现** — `runInline = m_asyncWorkers.empty() \|\| m_asyncStopping`，`m_asyncStopping==true` 时强制 `runInline=true` 走内联执行，不会丢任务。可能审查后已修 | `Resource/Private/ResourceManager.cpp:499-515` |
| **D12-01** RingBuffer 竞争 | 原 P0 → 复核撤销（有 CAS+mutex） | ✅ **确认撤销正确**；且复核“新增 P1：Allocate 无限循环”**也是误报** — 空间不足时第 288 行 `return result` 退出，`while(true)` 仅是有界 CAS 重试 | `RHI_DX12/Private/DX12Upload.cpp:275-289` |
| **W-02** Prefab 组件工厂注册不全 | 原 P0 → 复核撤销（“混淆两套工厂”） | ❗ **撤销结论本身有误，问题真实存在且应恢复 P0** — Prefab 用的 `ComponentFactory::CreateComponentByClassName`（`ComponentClassFactory`）其注册函数 `RegisterComponentClass` 的**全部调用点都在 `Tests/` 内**，引擎/启动路径零注册。非测试运行下 Prefab 实例化对所有组件类型返回 `nullptr` | `Scene/Private/Prefab.cpp:956,982,1027` + `RegisterComponentClass` 调用点仅见于 `Tests/ActorComponentValidation`、`Tests/ResourceInstantiationValidation` |
| **W-01** Prefab restore 中间态 | 原 P0 → 复核降 P2 | ✅ 维持 P2（上层实体级清理兜底） | 复核 appendix |
| **P-01/P-02** Particle 模拟被注释 | P0（复核确认） | ✅ **确认** | `Particle/Private/ParticleSubsystem.cpp:137,143`；`ParticleSystemInstance.cpp:154` |
| **Physics 端到端失效** | 原文档**未覆盖** | ❗ **新增 P0** — Jolt step 被注释、刚体从不注册进物理世界 | `Physics/Private/PhysicsWorld.cpp:70`；`Scene/Private/Components/RigidBodyComponent.cpp:326-327,207,220,260,275` |
| **无 CI / 无 CTest / 无依赖锁定** | 原文档**未覆盖** | ❗ **新增 P0（流程级）** — 掩盖以上一切 | 无 `.github/`；`CMakeLists` 无 `enable_testing/add_test`；`vcpkg.json` 无 `builtin-baseline` |
| **“Validation”测试是源码字符串 grep** | 原文档**未覆盖** | ❗ **新增 P0（测试有效性）** — 验证文本而非行为 | `Tests/RHIDX12SourceValidation/main.cpp:31-61`（`source.find(...)`） |
| **Editor 与 Engine 完全脱节** | 原文档**未覆盖** | ❗ **新增 P1** — `EditorApplication.cpp` 零 `Engine/GetSubsystem/RenderSubsystem` 引用，自跑 GLFW+GL+ImGui 壳 | `Editor/Private/EditorApplication.cpp`（grep 0 命中） |

**净结果**：原文档 7 个 P0 中，**2 个降级/撤销**（C-01 降级、R-01 视为已修），1 个被原复核错误撤销而**应恢复**（W-02），其余（W-01 已 →P2、P-01/P-02 确认、D12-01 已撤销）。**本次新增 4 类未覆盖的 P0 级问题**（Physics、CI/流程、测试有效性、外加架构性 RenderGraph/JobSystem，下文详述）。

---

## 1. 总体判断与成熟度评分

**一句话**：约 195K 行、35 模块，**分层清晰、抽象现代、内核扎实**，但贯穿全局一个模式——**“骨架强、上层空”**：大量高价值能力已**声明/抽象好接口却被注释或未接线**，而这一切被**几乎不存在的验证体系**长期掩盖。

**两维评分**（设计 vs 实现完整度，合并两份报告）：

| 域 | 设计质量 | 实现完整度 | 风险 | 综合成熟度 |
|---|---|---|---|---|
| Core / HAL / Geometry / Spatial | ★★★★ | ★★★☆ | 中 | 3.0 |
| RHI 抽象 + 5 后端 + ShaderCompiler | ★★★★ | ★★★★ | 中 | 3.5 |
| Render / RenderGraph / Material | ★★★★★ | ★★★ | 高 | 3.0 |
| Resource 管理 | ★★★★ | ★★★ | 中 | 3.0 |
| Scene / Actor-Component / Prefab | ★★★★ | ★★★ | 高 | 2.8 |
| World / Runtime / Animation | ★★★★ | ★★★ | 中 | 3.0 |
| Particle / Terrain / Water | ★★★ | ★★ | 高 | 2.3 |
| Physics / AI / Audio / Net / Script / UI | ★★★★ | ★★★ | 高 | 3.0 |
| Engine / Editor / Tools / Debug / Build | ★★★★★(内核) | ★★ | 高 | 2.5 |

**整体：架构设计 ~4.0/5，实现质量 ~3.0/5。** 这是一个结构良好的**中期引擎**，距离“可投产”仍有明确差距。

---

## 2. 经核实的统一 P0 清单

> 合并两份报告并按本次核实修订。分两类：**A 功能/正确性级**、**B 流程/有效性级**（后者优先级最高，因为它让前者无法被发现）。

### B 类 — 流程与验证（最高优先，先修这些）

| # | 问题 | 证据 | 后果 |
|---|---|---|---|
| **B1** | **无 CI、无 CTest**（`enable_testing/add_test` 零命中、无 `.github/`） | 仓库根 | 任何回归、任何被注释掉的功能都无人发现 |
| **B2** | **多个“Validation”测试是对源码做字符串 grep**，非行为测试；集成测试用裸 `assert()`（Release/`NDEBUG` 下静默通过） | `Tests/RHIDX12SourceValidation/main.cpp:31-61`；`Tests/SystemIntegration/main.cpp` | 测试“绿”不代表功能正确，重构即误报 |
| **B3** | **vcpkg 无 `builtin-baseline`** | `vcpkg.json` | 依赖解析不可复现，跨机/跨时构建漂移 |

### A 类 — 功能/正确性

| # | 域 | 问题 | 证据 | 后果 |
|---|---|---|---|---|
| **A1** | Physics | Jolt step 被注释；刚体从不注册进物理世界（“deferred until physics world is available”）；CCD/约束/碰撞掩码全 TODO | `PhysicsWorld.cpp:70`；`RigidBodyComponent.cpp:326-327,207,220,260,275` | 物理子系统端到端不可用 |
| **A2** | Scene/Prefab | 生产路径下 `ComponentClassFactory` 零注册，`CreateComponentByClassName` 对所有类型返回 null | `Prefab.cpp:956,982,1027` + 注册仅见于 `Tests/` | Prefab 组件实例化静默失败（仅测试内因就地注册而“通过”） |
| **A3** | Particle | `SetSimulator()` 与 `m_simulator->Simulate()` 均被注释 | `ParticleSubsystem.cpp:137,143`；`ParticleSystemInstance.cpp:154` | 粒子模拟不运行 |
| **A4** | Render | `ExecuteRenderGraphAsync` 忽略 compute 队列/fence，静默回退同步路径 | `RenderGraphExecutor.cpp:137-151` | 异步计算“名存实亡”，对外 API 误导 |
| **A5** | Render | 内存别名 Aliasing Barrier 算出却被注释，不发射 | `RenderGraphExecutor.cpp:19-27`；`RenderGraphCompiler.cpp:821` | Placed Resource 共享堆下 WAR/WAW 隐患 |
| **A6** | Core | “JobSystem”实为单全局锁 `priority_queue` 线程池，无 work-stealing；`JobGraph::Wait()` 自旋、就绪调度 O(N) 重扫 | `Core/.../Job/ThreadPool.h:174`；`JobGraph.cpp:132-169` | 全引擎并行吞吐被锁死（性能天花板） |

> 说明：A6 是“设计级 P0”——不是崩溃 bug，而是它从根上限制了渲染/动画/资源等所有可并行子系统的扩展性，且修复成本随代码增长而上升，故置于 P0。

**已从 P0 移除/降级**：C-01（争议，→P2 文档化）、R-01（当前树已修）、W-01（→P2）、D12-01（误报，撤销）。

---

## 3. 跨模块架构性问题（宏观四大主题）

**主题一：“声明了但没接线”/ Demo-ware 模式（头号系统性风险）**
跨多个独立 Agent 反复印证的同一形态——接口/抽象写得很完整，实现被注释或未挂接：
- RenderGraph 异步计算（A4）、别名 barrier（A5）
- `GPUCulling`（HiZ 两阶段遮挡）整套实现，`SceneRenderer` 从不实例化，仅测试引用
- GPU 粒子模拟绑定全注释（`GPUParticleSimulator.cpp:158`），CPU 计数门控
- Terrain/Water 渲染 Pass 的 `Setup` 不声明资源、`Execute` 不绘制（`TerrainRenderer.cpp:47`/`WaterRenderer.cpp:49`）
- Physics（A1）、Prefab 工厂（A2）、Particle（A3）
- `DebugSubsystem` 从未 `AddSubsystem` 注册；`StatsHUD` 无 ImGui 渲染路径——数据采集后永不显示

**主题二：并行化系统性缺失（吃掉性能上限）**
- A6 的线程池/JobGraph；Animation 全单线程（`Animation/` 内零 JobSystem 调用）。业界标配（UE5 Tasks、enkiTS、fiber-based）在此完全缺位。

**主题三：验证 / CI 的系统性空洞（掩盖以上一切）**
- B1/B2/B3。没有跨后端 golden-image（`CrossBackendValidation` 只比创建参数不比渲染输出，尽管 `ImageCompare` 已存在）。`BACKEND_ANALYSIS.md` 已过期（其列为 P0 的 Vulkan barrier batching 实际已实现）。

**主题四：重复 / 双轨制（维护与正确性负担）**
- Scene 层 `Actor` + 旧 `Component` 双存储、双 transform、`AddComponent` 编译期分支两路径——一次未收尾的迁移（近期 commit “Bridge legacy component lifecycle”）。
- 两套空间索引并存：`SceneManager::m_spatialIndex`（每帧重建）与 `SpatialSubsystem::m_index`。
- 5 个 RHI 后端零共享基类，`RHIResourceState`→原生枚举映射重复 ~5 份（215 处 case）。
- `Core/Math.h` 遗留手写数学且 `Perspective` 为**左手系**，与 glm 右手系路径冲突——正确性陷阱。
- 两套引用计数：`Core::RefCounted`/`Ref<T>` 与 `Resource::ResourceHandle` 叠加（原 R-08）。

---

## 4. 按域详解（行级 bug × 架构）

> 行级条目沿用原文档编号（C-/E-/R-/W-/G-/M-/S-/PA-/D12-/VK-/D11-/RHI-），状态以本次核实为准。

### 4.1 Core / HAL / Geometry / Spatial（3.0）
- **架构**：类型词汇表干净（`Handle` 代际句柄、`NonCopyable`）；glm 经 `MathTypes.h` 封装而非裸用；`Geometry/Batch/SIMDPlatform.h` 有真实 AVX2/SSE/NEON SoA 批量数学。
- **P0**：A6（JobSystem 非工作窃取 + JobGraph 自旋/O(N) 重扫）。
- **P1/P2**：C-02 `WaitAll()` 从 worker 线程调用死锁（已确认）；C-03 `DetectCycles()` 无缓存（已确认）；`RVX_ASSERT` Release 仍 `abort`；`LinearAllocator` 用 `std::malloc` 非对齐分配；`FrameAllocator` 宣称线程安全却每次分配上锁。
- **争议**：C-01（见 §0，倾向误报，仅需文档化 ordering 契约）。

### 4.2 RHI + 5 后端 + ShaderCompiler（3.5，全域最高）
- **架构**：显式 API 形态的好抽象（split barrier、placed heap、timeline fence、4-set+push-constant 绑定模型）；统一单源着色器管线（HLSL→SPIR-V→MSL/GLSL + 反射 + 两级缓存）质量高；能力位 gating 诚实。
- **架构缺口**：5 后端**零共享基类**，barrier 批处理/状态跟踪/枚举映射各写一遍；队列是 enum 而非对象，跨队列同步隐式；**无 bindless/descriptor-array binding 类型**（阻塞 GPU-driven）；caps 宣称 raytracing/mesh-shader 但无接口（aspirational stub）。
- **行级**：D12-03 Placed 资源析构序无运行时断言（确认）；D12-04 描述符堆无边界检查（确认）；D12-05 D3D12MA 失败仅 WARN 续跑（确认）；VK-01 deferred semaphore 销毁未验证队列完成（确认）；VK-02 aspectMask 纯格式推导无显式入参（确认）；VK-04 直调 `vkCmdPipelineBarrier2` 未查 `VK_KHR_synchronization2`（确认）；D11-01 帧索引硬编码 0（确认，注释说明 DX11 单 immediate context）。
- **误报**：D12-01（CAS+mutex 完备）；D11-02（`Submit()` 有委托实现）；以及本次发现原复核“RingBuffer 无限循环”亦误报（§0）。

### 4.3 Render / RenderGraph / Material / Resource（3.0）
- **架构**：FrameGraph 内核**真实强大**——读写依赖 DAG、Kahn 拓扑、死 pass 剔除、每子资源状态机、barrier 合并/冗余消除、区间着色的瞬态内存别名（RDG/Granite 级）。
- **P0**：A4 异步计算 stub、A5 别名 barrier 注释。
- **架构缺口**：`OpaquePass` 每物体虚调用提交、逐项重绑 VB、无 instancing/indirect；`GPUCulling`/`MeshletRenderer` 为死代码；材质每 draw 描述符 churn，无 bindless 材质表；遗留 `ExecutePasses` 路径 + 手写 back-buffer barrier 与图职责重叠。
- **行级**：G-03 拓扑失败回退 passIndex（确认）；M-02 `MaterialDescriptorKeyHash` 用 XOR 易碰撞；S-03 profile/backend 一致性未校验；PA-01/PA-02 ParticlePass 句柄/Device 注入注释。
- **误报**：M-01 cbuffer 溢出（`AllocateMaterialConstantSlot` 有边界检查并 `RVX_VERIFY` 夹紧）；S-01 着色器缓存失效逻辑（`validateOnLoad`+`HasChanged` 已实现）。
- **Resource**：异步 worker + 帧同步完成派发 + LRU/refcount 驱逐 + 依赖图 + 热重载，结构良好。R-01 当前树已修（§0）；R-02 `WaitForLoad()` 忙轮询（确认，注释明写 "Busy wait for now"）；R-05 `Load<T>` 无类型约束（确认）；R-06 缓存 TOCTOU 窗口（确认）；R-03/R-04 复杂度类误报（驱逐目标计算正确、`GetMemoryUsage` 在循环外）。

### 4.4 Scene / Actor-Component / Prefab / World / Animation（2.8–3.0）
- **架构**：是 **OOP “对象汤”而非数据导向 ECS**——组件存 `unordered_map<type_index, unique_ptr>`，指针追逐 + 每组件一次堆分配 + 虚 `Tick`；无原型/稀疏集存储、无系统式迭代。Transform 脏标记传播与惰性世界矩阵做得不错。
- **P0**：A2（Prefab 类工厂生产路径零注册）。
- **架构缺口**：Actor/旧 Component 双轨未收尾、双 transform、双空间索引；`Actor::GetComponent` O(n)+`dynamic_cast`；Animation 全单线程、pose 每次分配不池化、`AnimatorCullingMode` 声明未接线。
- **行级（Prefab）**：W-05 ordinal 隐式跳过致数据丢失（确认）；W-06 `AssignComponentName` 覆盖 `SetName`（部分成立）；W-08 unnamed 组件无 binding 追踪（确认）；W-09 `InstantiatePrefab()` API 被注释无说明。
- **误报**：W-03（不会产生 `Name_1_1` 双后缀）；W-07（双 transform 是兼容设计同步更新）；W-04 降 P2（局部 unique_ptr 析构兜底无泄漏）。

### 4.5 Particle / Terrain / Water（2.3）
- **架构**：粒子**绘制路径**真为 GPU-driven（`DrawIndexedIndirect`，`ParticleRenderer.cpp:180`），但**模拟侧 stub**（A3 + 绑定注释 + CPU 计数门控）。Terrain/Water 渲染 Pass 为不绘制的空壳，绕过 RenderGraph。
- **建议**：要么补全（GPU 计数驱动 indirect dispatch、按 RenderGraph 实现 Setup/Execute），要么明确标注 WIP，避免被读成“已实现”。

### 4.6 Physics / AI / Audio / Networking / Scripting / UI（3.0）
- **封装纪律强**：6 家中间件 5 家良好隐藏（Audio 用 `void* m_backendData` 藏 MiniAudio；Net 用 pImpl 藏 Asio；recast 限于 NavMeshBuilder；UI 自有 retained 树不漏 ImGui）。**唯一真漏**：Sol2 类型（`sol::table/usertype`）漏到 `LuaState.h`/`ScriptComponent.h` 公共头。
- **逐子系统**：AI 最成熟（recast 真用、Nav/BT/Perception 数据驱动）；Audio 生产级；Networking 中（真 Asio UDP + 溢出保护的 bitstream，但**无加密/鉴权/重放校验**，`ClientToServer` 信任客户端、delta 默认是假 delta）；Scripting 中（Sol2 RAII + 默认 Safe 预设好，但 `instructionLimit/memoryLimit` **声明却无 `lua_sethook` 强制**，且 `GetRawState()` 逃逸口）；**Physics 最差（A1，端到端失效）**；UI 功能最薄。

### 4.7 Engine / Editor / Tools / Debug / Build（2.5）
- **亮点**：`SubsystemCollection`（Kahn 拓扑 + DFS 环检测带路径 + 逆序关闭）是全仓最严谨的逻辑之一；`Engine::Tick` 管线清晰（含 2ms GPU 上传预算）；模块 CMake 分层基本无环。
- **P0/P1**：B1/B2/B3（CI/测试/依赖）；Editor 与 Engine **完全脱节**（自跑 GLFW+GL+ImGui，`InitializeRHI()` 是 TODO stub，菜单动作多为空 `{}`，FPS `1/dt` 首帧除零）；`DebugSubsystem` 从未注册；Tests 直接重编引擎 `.cpp`（ODR/耦合内部布局）。
- **配置漂移**：root `RVX_ENABLE_OPENGL OFF` 但 preset 强制 `ON`，Editor 无条件 `find_package(OpenGL REQUIRED)`；`Engine.h` 文档与 `TickWorlds` 实际行为不符（按文档会双 tick world）。

---

## 5. 合并优先级路线图

### 阶段 0：让真相可见（1 周，最高杠杆）
1. **建 CI**：`.github/workflows`，debug+release × DX12/Vulkan，接 `enable_testing()/add_test`（B1）。
2. **修测试有效性**：`*SourceValidation` 改真实行为测试或明确标注为 lint；测试框架用 Release 安全断言宏；用已有 `ImageCompare` 做跨后端 golden-image（B2）。
3. **锁定 vcpkg `builtin-baseline`**（B3）。

### 阶段 1：即刻功能修复（1–3 天）
4. Particle：取消注释 `SetSimulator`/`Simulate`（A3，~30 分钟）。
5. Prefab：在引擎启动注册全部 ActorComponent 子类到 `ComponentClassFactory`（A2，~半天）。
6. RenderGraph：发射别名 barrier，或在别名资源首用非 Undefined 时断言（A5）；`ExecuteAsync` 要么真提交 compute 队列要么显式禁用并改名（A4）。
7. 文档化 `RefCounted` 的 ordering 契约（C-01 争议项，关闭误报）。

### 阶段 2：地基与正确性（2–4 周）
8. **重写线程池**：per-worker work-stealing deque + 原子依赖计数 `JobGraph`，去掉 `Wait()` 自旋与 O(N) 重扫（A6）——全引擎吞吐最高杠杆。
9. 接通 Physics：`PhysicsWorld`↔`IPhysicsBackend`、`RigidBodyComponent` body 注册/步进，补 render 端插值（A1）。
10. 强制脚本沙箱：`lua_sethook(LUA_MASKCOUNT)` + 自定义 allocator 落实 limit。
11. Resource：`WaitForLoad` 改 condition_variable（R-02）；`Load<T>` 加 `static_assert`（R-05）；合并 check-load-cache 临界区（R-06）。
12. Prefab 生命周期与 ordinal 修复（W-05/W-06/W-08）。
13. RHI 后端：描述符堆边界检查（D12-04）、D3D12MA 失败即 abort（D12-05）、显式 aspectMask（VK-02）、`synchronization2` 扩展检查 + 回退（VK-04）、deferred semaphore fence 守护（VK-01）。

### 阶段 3：架构演进（1–3 月）
14. 收尾 Actor/Component 双轨、统一空间索引、删 `Core/Math.h` 遗留。
15. RHI 引入共享 `RHICommandContextBase` + 统一枚举转换；加 bindless binding 类型。
16. Render：接通 `GPUCulling`、`OpaquePass` 改 instanced/indirect + bindless 材质表；删 legacy `ExecutePasses`。
17. Animation 并行化 + pose 池化；Editor 与 Engine 复合（经 RHI 渲染视口）；注册 `DebugSubsystem` 并给 HUD/Console 真实渲染路径。
18. Networking 加握手/鉴权/序列校验 + 服务器权威 reconciliation；Scripting facade 去 `sol::` 公共头泄漏。

---

## 6. 误报与争议项（透明记录，勿追）

| 项 | 结论 | 原因 |
|---|---|---|
| C-01 RefCounted UAF | 倾向误报（→仅文档化） | relaxed-add/acq_rel-release 是标准无锁引用计数惯用法 |
| R-01 任务静默丢失 | 当前树不可复现 | stopping 强制内联执行 |
| D12-01 RingBuffer 竞争 | 误报 | CAS(head) + mutex(Reset/tail) 完备 |
| “RingBuffer 无限循环”（原复核新增） | 误报 | 空间不足时 `return`，仅有界 CAS 重试 |
| W-03 名称双后缀 | 误报 | `AssignComponentName` 以现名为 base |
| W-07 双 Transform 不一致 | 误报 | 兼容设计，同步更新 |
| M-01 Material cbuffer 溢出 | 误报 | 有边界检查 + `RVX_VERIFY` |
| S-01 着色器缓存失效 | 误报 | include 变更/`HasChanged` 已处理 |
| R-03/R-04 驱逐复杂度 | 误报 | 目标计算正确、O(n) 非 O(n²) |
| D11-02 Deferred Context | 误报 | `Submit()` 有委托实现 |

---

## 附录：本次核实证据（命令级）

- **Physics**：`grep "m_physicsSystem->Update" Physics/Private/PhysicsWorld.cpp` → 仅 `:70 // m_physicsSystem->Update(dt, ...);`；`RigidBodyComponent.cpp:326-327` "TODO: Get physics world… body creation will be deferred"。
- **Prefab 工厂**：`grep -rn "RegisterComponentClass(" --include=*.cpp --include=*.h .` 调用点仅 `Tests/ActorComponentValidation`、`Tests/ResourceInstantiationValidation`；生产 `Prefab.cpp:956/982/1027` 调 `CreateComponentByClassName`。`ComponentFactory::RegisterDefaults` 仅注册 `"StaticMesh"`（model-import 工厂，另一套）。
- **Particle**：`ParticleSubsystem.cpp:137,143` `// instance->SetSimulator(...)`；`ParticleSystemInstance.cpp:154` `// m_simulator->Simulate(...)`。
- **RefCounted**：`RefCounted.h:56` `fetch_add(1, relaxed)`；`:62` `fetch_sub(1, acq_rel)`。
- **EnqueueAsyncJob**：`ResourceManager.cpp:503` `runInline = m_asyncWorkers.empty() || m_asyncStopping;` → stopping 强制内联。
- **DX12 RingBuffer**：`DX12Upload.cpp:283-288` 满则 WARN+`return`；`:301` `compare_exchange_weak`；`:320` `Reset` 持 `mutex`。
- **CI/构建**：无 `.github/`；`grep enable_testing|add_test` 零命中；`vcpkg.json` 无 `builtin-baseline`。
- **测试**：`Tests/RHIDX12SourceValidation/main.cpp:31-61` 多处 `source.find("...")`（字符串 grep）。
- **Editor**：`grep -n "Engine|GetSubsystem|RenderSubsystem" Editor/Private/EditorApplication.cpp` 零命中。

---

*方法：6 architect Agent（宏观）+ 原文档 8 Explore Agent（行级+复核）+ 本次定向源码核实。*
*所有 file:line 基于 2026-05-22 当时工作树，修复前仍建议在当前分支逐条二次确认（trust but verify）。*
