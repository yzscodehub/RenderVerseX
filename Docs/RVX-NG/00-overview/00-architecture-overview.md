# RVX-NG 次世代引擎架构设计（RenderVerseX Next-Gen · 定稿）

**日期**：2026-06-20
**状态**：框架 v1.0 定稿（对标最佳实践校准 · ambition 分级兑现）；Spike 与实施方案后置
**性质**：RenderVerseX **次世代目标引擎架构**——从第一性原理设计、并对标 UE5/Frostbite 级务实 SOTA 校准的理想终态。机制层全面对齐 SOTA；高风险能力（VisBuffer/ReSTIR/分层材质/Work Graphs）均**分级为"可叠加档/Phase-2"**，首期主线全部跑已验证路线（见 §25）。本文只设计**框架**（分层、模块职责、边界、数据流、决策、里程碑、工程化骨架），**不含代码**。各子系统细化另起子 spec。
**完整性**：三轮迭代——①横向模块广度（Input/Camera/探针/体积/贴花/发行等）②工程化骨架（测试/构建/线程/PSO/驻留）③**对标最佳实践校准**（D1–D12 + 分级路线 §25）。
**设计关系**：本文是北极星目标架构；实施、迁移与兼容策略另起文档，不进入目标架构约束。

---

## 0. 定位与范围

- **目标**：跨平台 AAA / 开放世界级实时引擎框架。
- **后端**：D3D12 / Vulkan / Metal3（桌面/移动）**+ 主机原生 API 接缝（GNM/AGC、NVN）**。
- **范围内**：引擎运行时框架、渲染、资产、世界、仿真系统骨架、工具骨架、发行管线、测试/构建工程化、跨切面。
- **范围外（显式非目标）**：具体玩法逻辑/内容；**可视化脚本（Blueprint 等价）**（用 Lua/原生热重载替代）；**Gameplay 脚手架**（能力系统/物品/状态机 GAS 类——仅设计接缝，不提供实现）；**多人协作编辑器**；平台发行认证流程本身。
- **范围决策（默认取向见括号）**：
  - **MSAA**：**不走 MSAA，TAA/上采样 only**（延迟管线取向）。
  - **可见性缓冲**：**Phase-2 可叠加几何前端**（首期主线为 meshlet GPU-driven 延迟，见 D5/§25）。
  - **VR/XR**（默认不在首期，但帧结构/输入预留 stereo 接缝）。
  - **多 GPU**（默认单 GPU，显式 explicit-multi-adapter 后置）。
  - **Mod 支持**（默认预留 VFS 挂载接缝，完整 Mod 工具后置）。

## 1. 设计哲学（6 铁律）

1. **数据导向是地基**——海量域 day-1 即 SoA/archetype 存储；bespoke gameplay 叠**一等 authoring 门面**（混合，非纯 ECS）。
2. **GPU-driven 是默认不是高端项**——CPU 永不逐物体 draw。
3. **光追是一等公民不是 bolt-on**——管线从设计即混合栅格 + RT；**GI 分级**（探针→辐照缓存→ReSTIR）。
4. **确定性内建**——固定步长 sim、限定确定性子域，可回滚/录制重放。
5. **诚实可验证**——无 stub 谎报成功，可测性是架构约束（见 §17）。
6. **Ambition 分级兑现**——高风险能力分 proven→advanced→research 三档；首期跑已验证路线，研究级可叠加，**任一档翻车不阻塞主线**（见 §25）。

## 2. 分层架构总览（依赖严格单向，下层不知上层）

```
App        装配：主循环 · 模块系统 · 启动器 · Gameplay 接缝 · 样例
Tools      引擎客户端：Editor(含 PIE/材质图/Sequencer) · Profiler · FrameDebugger · Console
Feature    仿真系统（皆 System）：Physics·Anim·Audio·AI·Net·Script·VFX·Terrain/Water·UI
Gameplay   GameInstance/GameRules/Player/Controller/HUD bridge 接缝；项目玩法规则不进入引擎核心
Render     meshlet GPU-driven 延迟(主线) · Clustered 光照 · 混合 RT · 体积/天空/贴花/探针 · RenderGraph · FrameRenderer
Asset      Import(DCC) → Cook(离线/DDC) → Runtime(mmap) → Stream(虚拟纹理/几何) → Registry → VFS/Package(发行)
World      数据导向 World · World Partition(流式) · 增量 BVH · 层级(关系) · Prefab/序列化
ShaderComp HLSL→DXC→SPIR-V→跨编译 · permutation · 两级缓存 · 异步 PSO 预编译
Core数据层  archetype 列存 · Query · System · Scheduler · Resource · Relation · Authoring 门面 · 变更检测
RHI        D3D12/Vulkan/Metal3 + 主机接缝 · bindless · timeline · indirect · AS · mesh shader · query · VRS · residency
Foundation Jobs(task-graph) · Memory · Reflect(codegen) · Handle · Name · Input · Math · Container · Platform/VFS · Log · Config · Event · Time · RNG/Hash/Compress
Ecosystem  Project/Package/Plugin · templates/samples · resolved lock · license/SBOM · provider isolation
```

**两条边界铁律**：① `Feature` 永不直接调 `RHI`——要渲染就声明组件，由 Render/Extract 搬运。② `Render`/`RHI` 永不引用 gameplay 类型——只认 Extract 快照。

## 3. 核心架构决策（已对标最佳实践校准）

  #   决策   取向（校准后）   理由 / 对标  
 --- --- --- --- 
  D1   数据模型   **混合数据导向**：数据导向内核（渲染/变换/粒子/人群/动画走 archetype）+ **一等 entity/actor 门面**（bespoke gameplay）   对齐 UE5(Actor+Mass)/Unity(GO+DOTS)；纯 ECS 工效不足  
  D2   并行基底   **工作窃取 task-graph + 依赖图**；render/sim 为逻辑阶段；**fiber 降为可选实现**   对齐 UE5 Tasks/enkiTS；fiber 有 TLS/调试/移植代价  
  D3   游戏↔渲染边界   **单向 Extract 快照**（三缓冲）   SOTA 正确（UE proxy 模型）  
  D4   后端   D3D12/Vulkan/Metal3 **+ 主机原生接缝（GNM/AGC、NVN）**   真跨平台；Vulkan 不覆盖主机  
  D5   几何   **meshlet GPU-driven 延迟为首期主线**；**VisBuffer + 软光栅 = Phase-2**   多家 AAA 验证；VisBuffer 仅 UE Nanite 出货、风险高  
  D6   光照/阴影   **Clustered 延迟 + 虚拟阴影图(VSM)**   海量动态光 + SOTA 阴影  
  D7   全局光照   **Probe/烘焙/SSGI 为 Proven 兜底**；**Radiance Cache 为 M6 目标能力/Advanced，S6 关闭后才可升格主线**；**ReSTIR 为 Research/高端档**   避免未验证 GI 技术阻塞主线；Lumen 式混合路线保留为可升级方向  
  D8   反射   **libclang codegen（现在）→ C++26 静态反射（后续）**，一套通吃   标准 C++ 当前无静态反射；AAA 普遍用 codegen  
  D9   资源引用   **代际句柄**，非裸指针   悬垂安全、可序列化、可重定位  
  D10   内存   **帧竞技场 + 类型池 + 分类预算**，热路径零 malloc   确定性、cache 友好、可控  
  **D11**   **确定性**   **限定 sim/physics 确定性子域**（有序/固定规约调度 + 定点/确定性数学），渲染/音频在 lockstep 边界外   消解"自动并行 + 确定性"张力  
  **D12**   **延迟**   **可配置流水线深度**：默认 2–3 帧吞吐优先 + 低延迟模式（减重叠，Reflex 式）   消解"深流水 vs 低延迟"张力  

## 4. 贯穿契约（所有模块遵守）

- **C1 子系统拓扑生命周期**：依赖拓扑 init、逆序 shutdown；缺依赖/环 = fail-fast 中止。
- **C2 单一并行基底**：一切可并行工作经 **task-graph 调度（工作窃取 + 依赖图）**，禁止私有线程；fiber 为可选实现后端。
- **C3 帧级内存**：热路径零堆分配；高频对象池化；按类别预算 + OOM 处理。
- **C4 句柄化**：资源/实体全用代际句柄。
- **C5 一套反射**（codegen 驱动）：序列化/编辑器/网络/组件注册。
- **C6 能力诚实 + 分级**：能力位与实现一致（CI 校验）；low→ultra 可干净降级。
- **C7 依赖单向无环**：中间件类型不漏公共头（facade 隔离）；CI 强制 acyclic（见 §18）。
- **C8 渲染只读快照**：Render 只消费 Extract 产物，绝不反向操作世界。
- **C9 线程模型契约**：render/sim 是 task-graph **逻辑阶段**非固定 OS 线程；硬约束仅 OS 消息泵(主线程)与 GPU 提交(按 API 规则)；公共 API 标注线程安全；跨线程只经 Jobs/命令队列。
- **C10 可测性即架构**：每模块可孤立测试；无源码 grep 冒充验证；Release 安全断言；render 产出必过真实 golden（见 §17）。

---

## 5. Foundation 层

  模块   职责   关键决策  
 --- --- --- 
  **Core**   类型别名、`Result<T,E>`、`Span`、`Handle<T>`、GUID/ID   边界用 Result 不用异常；句柄代际化  
  **Name（字符串驻留）**   驻留字符串 / FName 等价   O(1) 比较、省内存；资产/标签/实体 ID 的基石  
  **Log**   分级分类日志、结构化字段、多 sink   编译期可裁剪、异步落盘  
  **Memory**   帧竞技场、类型池、TLSF、**分类预算 + 追踪 + OOM 处理**   每帧 reset；分配器可注入；按类别（纹理/网格池）限额  
  **Jobs**   **工作窃取 task-graph 调度、依赖图、ParallelFor、continuation、主线程任务队列**   continuation 式等待；**fiber 可选后端**，非契约  
  **Reflect**   类型元数据（字段/偏移/属性）   **libclang codegen（现阶段）→ C++26 静态反射（后续迁移）**，一套通吃  
  **Input**   action/axis 映射、设备抽象（键鼠/手柄/触摸）、输入上下文栈、重绑定、死区、**录制（供 replay）**   独立子系统，非 Platform 附属  
  **Math**   SIMD 向量/矩阵/四元数、包围体、变换、右手系统一；**确定性数学子集（定点/有序）供 sim**   单一手系；确定性子域专用路径  
  **Container**   数据导向容器（SparseSet、PagedArray、HandleMap、Ring）   无 `std::` 节点容器于热路径  
  **Platform/VFS**   窗口、时间、线程原语、动态库、**主机 SDK 接缝**；**虚拟文件系统**（pak + loose + mod 叠加挂载）   平台/主机差异收敛于此层  
  **Config/CVars**   运行时可调变量、分级配置、特性开关   统一注册 + 反射驱动 UI  
  **Event**   类型化事件/消息总线   帧内派发，避免跨帧悬挂  
  **Time**   sim 时钟（固定步长）vs 真实时钟；定时器；time dilation；**帧节奏/限帧/低延迟（Reflex/anti-lag + 输入延迟治理，D12）**   确定性 + 低延迟来源  
  **Util**   确定性 **RNG**、**Hashing**、**Compression**、**序列化格式（二进制 + 文本）**   网络/资产/存档/确定性共用  

## 6. 数据导向内核 + Authoring 门面（混合世界表示）

- **混合 authoring（D1）**：海量/热域走 archetype（渲染 proxy/变换/粒子/人群/动画）；bespoke gameplay 用**一等 entity/actor 门面**（retained 对象 + 组件 + 可虚行为），经桥把门面的变换/渲染数据写入数据导向列。**门面是一等公民，非"皮肤"。**
- **存储**：archetype + 列式（SoA）；同 archetype 实体组件连续，系统线性迭代。
- **Query**：按组件签名匹配 archetype，迭代 cache 友好。
- **System**：纯函数 + 声明读写集；**Scheduler 据读写冲突自动并行**（读读并行、写写/读写串行），编译成 task-graph。**确定性子域内改用有序/固定规约调度（D11）**。
- **TickGroup**：`Input → PrePhysics → Physics(固定步长) → PostPhysics → Animation → Extract`。
- **Resource**：全局单例（渲染器/资产器）不作实体。
- **Relation**：层级/attach 用关系表达，非指针树。
- **Command Buffer**：结构性变更（增删组件/实体）延迟到帧边界回放，迭代期安全。
- **变更检测**：组件脏标记驱动 Extract 增量同步。
- **序列化**：经 C5 反射统一驱动（存档/Prefab/网络）。
- **多世界**：支持并行 world（主世界/预览/编辑器），为 World Partition 流式与 PIE 铺路。

## 7. RHI（薄 · 显式 · 现代特性一等）

- **设备/能力**：adapter 枚举、capabilities 分级（tier），差异显式可查。
- **队列**：graphics/compute/copy 独立队列对象 + **timeline semaphore** 跨队列同步（非隐式）。
- **命令**：录制式 command context；**indirect**（ExecuteIndirect/DrawIndirect/DispatchMesh）一等；predication/条件渲染。
- **资源**：buffer/texture/view；**placed resource + 显式堆**支持别名。
- **Bindless**：全局描述符堆，注册即得 shader 索引——**无 per-draw 绑定**。
- **管线**：graphics/compute/**RT/mesh shader** 一等。
- **加速结构**：BLAS/TLAS 创建/构建/更新（DXR / VK_KHR_ray_tracing 对齐）。
- **Query/Timestamp**、**VRS**、**显存驻留/预算/驱逐**（与资产流式 §10 联动）。
- **Swapchain/呈现**（含 **HDR 输出格式**）、**barrier/状态**、**debug 标记 + 抓帧集成（PIX/RenderDoc）**。
- **后端**：D3D12 / Vulkan / Metal3 **+ 主机原生接缝（GNM/AGC、NVN、Xbox D3D12 变体）**在同一抽象之下、不漏原生类型；**共享命令翻译基类**避免 N 份枚举映射。

## 8. ShaderCompiler

- 单源 **HLSL → DXC → SPIR-V → 跨编译**（MSL/必要时 GLSL）。
- 反射元数据；**permutation/variant** 管理；**离线 + 运行时两级缓存**；**热重载**；**发行期全 permutation 预烘焙**。
- **PSO 防卡顿**：**异步 PSO 预编译 + 持久化 PSO 缓存 + 后台编译队列**，杜绝运行时着色器编译 hitch（AAA 必修）。
- **bindless 友好**绑定模型；profile/backend 一致性校验。

## 9. Render（meshlet GPU-driven 延迟主线 + 混合 RT，能力分级）

  子模块   职责   关键决策 / 分级  
 --- --- --- 
  **Extract**   ECS → 渲染世界快照（唯一跨界点）   三缓冲、增量、并行  
  **Camera/View**   多相机、视图栈、投影/视锥、cinematic、jitter   驱动 FrameRenderer 多视图  
  **GpuScene**   持久化 bindless 大表（mesh/instance/material/light/transform）   增量更新，非每帧重建  
  **Geometry**   meshlet/cluster GPU-driven（**首期主线**）、LOD/HLOD/Impostor   **VisBuffer + 软光栅 = Phase-2 可叠加前端**  
  **Culling**   两阶段 HiZ 遮挡 + frustum，per-instance/cluster；**ExecuteIndirect（首期）**   **GPU Work Graphs 评估/预留**  
  **RenderGraph**   唯一表达 GPU 工作：pass/瞬态资源/自动 barrier   **真 async compute + 真别名**  
  **Lighting**   Clustered 光剔除 + 延迟着色 + 面光源；**GI 分级：Probe/烘焙/SSGI(Proven)→Radiance Cache(M6 目标能力)→ReSTIR(高端)**   主线先由 Probe/SSGI 兜底，Radiance Cache 经 S6 后升格  
  **Shadows**   虚拟阴影图(VSM, clipmap 缓存) + RT 阴影   弃传统 CSM  
  **RayTracing**   AS 管理、RT 阴影/反射(命中真受光)、AO、降噪   **ReSTIR GI = 高端档**（主 GI 见 Lighting）  
  **AtmosphereVolumetrics**   Sky-Atmosphere 天空模型、体积雾/光、日夜   纳入框架  
  **Decals**   延迟贴花    
  **PostProcess**   TAA、上采样(DLSS/FSR/XeSS 抽象)、tonemap、bloom、DOF、motion blur、**HDR 输出/色彩管理**   ping-pong 链  
  **Transparency**   Forward+（统一 clustered 光照）/ 可选 OIT   接入阴影  
  **Material**   bindless 材质表 + 多 shading model；**分层 BSDF(Substrate 式)预留**   无 per-draw 描述符 churn  
  **FrameRenderer**   多视图（主/阴影/反射/探针）、分级、单一 RenderGraph 提交   编排者  
  **DebugDraw/Viz**   调试几何、G-buffer/overdraw/复杂度可视化、RenderGraph 可视化   工具支撑  

## 10. Asset

- **Import（DCC 前端）**：glTF/FBX/纹理/音频导入 → 中间表示。
- **Cook（离线）**：→ 紧凑运行时格式（mmap、GPU-ready，BCn/mip/tangent/LOD）；稳定 GUID + 内容寻址缓存；**共享/分布式派生数据缓存（DDC）+ cook farm**（团队规模）。
- **Runtime**：句柄化、异步加载、引用计数、LRU 驱逐、依赖图、软引用/惰性加载。
- **Stream**：**虚拟纹理 + 虚拟几何**流式 + 每帧驻留预算（time-sliced）；与 RHI 显存驻留（§7）联动。
- **Registry/Database**：GUID↔资产映射、重导入、跨重启稳定。
- **Versioning**：烘焙格式/组件 schema **版本化 + 数据迁移**（前后兼容）。
- **HotReload**：源变更 → 重 cook → 运行时热替换。

## 11. World / Scene

- **World Partition**：网格/八叉树 cell 流式 + data layer（开放世界）。
- **Spatial**：单一权威空间索引，增量 BVH refit + 脏列表。
- **层级**：经关系；变换脏传播批量、无逐节点 RTTI。
- **Prefab/序列化**：经反射；实例化 = 组件批量 spawn / 门面实例化。
- **Level/子关卡 + GameInstance 作用域**：流式单元 + 跨关卡会话状态。

## 12. Feature 系统（皆 System，不碰 RHI）

  系统   框架要点（含 AAA 深度）  
 --- --- 
  **Physics**   固定步长、确定性子域、刚体/角色/载具、**ragdoll/破坏/布料**、jobified broadphase、render 端插值  
  **Animation**   骨骼、GPU skinning、混合树/状态机、IK、retarget、root motion、**动画压缩**、**morph/blendshape**、**motion matching**、布料/头发、jobified + 池化  
  **Audio**   3D 空间化、物理遮挡、混音、**DSP/混响/总线 bus**、流式；**中间件决策（自研 vs Wwise/FMOD）待定**  
  **AI**   tiled navmesh(Recast/Detour) + 动态障碍、funnel string-pull、行为树 per-agent 克隆、感知、**AI LOD（人群）**  
  **Networking**   **传输层抽象**、反射驱动复制、服务器权威、快照 + 插值、回滚/预测、**兴趣管理/relevancy**、**延迟补偿（服务器回溯）**、会话/匹配、语音、鉴权/序列校验/重放保护/限流、心跳  
  **Scripting**   沙箱 VM（指令/内存/路径/API 白名单强制）、热重载、反射绑定  
  **VFX/Particle**   GPU-driven 模拟（GPU alive-count 驱动 indirect dispatch）  
  **Terrain/Water/Foliage**   GPU heightmap/material、LOD crack mask、Water RenderGraph 资源、植被 instancing/impostor  
  **UI**   retained 树、经 RHI 渲染（DrawRect/Text/Image）、布局、真实字体度量（CJK）、输入  

## 13. Tools（引擎客户端）

- **Editor**：跑同一运行时；经引擎 RHI 离屏视口渲染；inspector 经反射编辑**真实世界**；gizmo 写回；场景存/读、undo/redo、多选/复制粘贴；AssetBrowser GUID + 缩略图；**Play-in-Editor (PIE)**；**材质/Shader Graph 编辑器**；**Sequencer 过场编辑**；源码管理集成。**绝非独立假壳。**
- **Project/Package/Plugin**：project manifest、package/plugin descriptor、resolved lock、feature set、templates/samples；CI/Shipping 只消费 lock 后依赖，插件不能绕过 ModuleGraph。
- **Online/Platform Services**：账号、认证、session/lobby、presence、achievements、leaderboards、cloud save、entitlement/store；平台 SDK 类型只存在 provider 私有实现。
- **Gameplay Framework Seams**：GameInstance、WorldContext、GameRules、Player、Controller/Pawn、HUD bridge；只提供项目玩法接缝，不内置具体 ability/inventory/quest。
- **Media/Video**：cooked media asset、decode backend、A/V/subtitle sync、video texture、Sequencer track、平台 codec profile。
- **Profiler**：CPU/GPU 时间轴、计数器（依赖 RHI query）。
- **FrameDebugger / RenderGraphViz**：逐 pass 资源/barrier 可视化。
- **Console/CVars UI**：运行时调参（反射驱动）。
- **HotReload**：代码（可选，game 模块 DLL 重载）+ 资产。

## 14. App / Engine

- **主循环**：固定步长 sim + 变量步 render，task-graph 流水线重叠（深度可配，D12）。
- **子系统生命周期**：拓扑 init/逆序 shutdown（C1）。
- **模块/插件系统**：特性可装拆；依赖声明式（见 §18）。
- **Gameplay 接缝**：spawn/possession/GameMode 钩子（仅设计**接缝**，实现属游戏层）。
- **应用模式**：game vs editor vs PIE，同运行时不同装配。
- **配置/命令行**：分级配置入口。

## 15. 跨切面（贯穿全层）

确定性模型（固定步长 + 种子 + 确定性 RNG + 确定性子域 D11）· 能力/分级 tier · 遥测/分析 · **崩溃处理（捕获→minidump→恢复/安全模式）** · **Replay 录制回放** · **Save/Profile/UserData（原子写、版本迁移、cloud conflict）** · 本地化（文本/语音/字体/RTL）· **无障碍（字幕/重绑定/色盲）** · **反作弊/安全钩子** · **Platform Profile（Desktop/Console/Mobile/XR/Web）** · **Scalability/Quality Profile（全局质量档、压力降级）** · **第三方合规（SBOM/license/attribution）** · 内存分类预算 + OOM · 模块依赖环检测 · 编码约定 · sanitizer 门禁。

## 16. 生产化 / 发行模块（shipping）

  模块   职责  
 --- --- 
  **Package/Pak + VFS**   资产打包、压缩、按平台分包；VFS 挂载（pak + loose + mod 叠加）  
  **Shipping Cook**   目标平台内容烘焙 + 全 shader permutation 预烘焙 + PSO 预缓存  
  **Patching/DLC**   增量补丁、可下载内容、版本校验  
  **Console Platform**   主机 SDK 集成（账号/存储/成就/认证要求）  
  **Telemetry/Crash 后端**   上报管线、symbol 服务、聚合  

## 17. 测试与可测性架构（C10 的落地）

> **可测性是架构约束**，不是事后补丁。测试必须验证真实行为，不能用源码字符串或静默断言冒充验证。

- **测试层级**：单元（Foundation/容器/数学/反射）· 集成（系统、资产加载、RenderGraph 编译）· **跨后端 golden-image**（DX12/Vulkan/Metal3 逐像素比对）· **性能回归门禁**（帧时/draw 数/显存基线）· **确定性回归**（固定种子重放逐位比对）· **fuzz**（序列化/网络/资产解析）。
- **机制**：测试经**真实行为**而非源码字符串；**Release 安全断言宏**；系统可注入隔离 world 单测；RHI 有 headless/参考设备路径；golden 由 designated GPU 环境**实跑（硬门禁）**——"跑不了-记原因"不算通过。
- **确定性子域调度约束（D11）**：sim/physics 在确定性子域内用**有序/固定规约调度**（不吃自动并行的浮点规约非确定性）+ 定点/确定性数学；确定性回归测试守护该边界。
- **禁止反模式**：源码 grep 冒充验证、裸 `assert`（Release 静默）、stub 报成功。

## 18. 模块依赖图与构建系统

- **模块定义**：每模块声明 公共接口 / 依赖 / 可选性；构建系统据此链接，**CI 强制依赖 DAG 无环**（C7）。
- **依赖 DAG（文本）**：
  ```
  Foundation ─┬─ 数据层 ─┬─ World ─┬─ Feature ─┬─ App ─ Tools
              ├─ RHI ─ ShaderComp ─ Render ─┘          │
              └─ Asset ─────────────────────┘          │
  (Render 经 Extract 读数据层快照，单向；Feature 不依赖 RHI/Render)
  ```
- **插件/可装拆**：Feature 模块可选装；ABI 边界经接口/句柄，不漏第三方类型。
- **构建配置**：**Debug / Development / Shipping / Test**；Shipping 剥离 editor/console/assert/profiler 标记；特性按配置裁剪。

## 19. 帧结构（task-graph 流水线，深度可配）

```
Sim 阶段 (固定步长):   帧 N+1 ── Input → 系统(task-graph) ── Extract→快照
Render 阶段:           帧 N   ── 消费快照 ── 构建 RenderGraph ── 提交
GPU:                   帧 N-1 ── 执行 RenderGraph（async compute 重叠）
```
sim/render 是 task-graph **逻辑阶段**（非固定 OS 线程，见 C9）；**Extract 是唯一同步点**（三缓冲）；**流水线深度可配置（D12）**：默认 2–3 帧吞吐优先，竞技走低延迟模式（减重叠）。

## 20. 三大数据流

- **游戏→渲染**：数据层 World ─Extract(并行)→ 快照(三缓冲) ─→ GpuScene 增量 ─→ GPU-driven。
- **资产→GPU**：源 ─Import→Cook(离线/DDC)→ 运行时格式 ─Stream(预算)→ RHI 上传 ─→ GpuScene 表。
- **一帧主干**：Extract → 刷新 AS → 各视图[GPU 剔除→间接 G-Buffer] → 主视图[Clustered 光剔除→阴影(VSM+RT)→延迟着色→RT 反射 + 辐照缓存 GI→体积/天空] → 合成 → 贴花 → 透明(Forward+) → 后处理(TAA/上采样/tonemap/HDR 输出) → UI。

## 21. 构建里程碑（依赖序；首期主线全用已验证技术）

  M   里程碑   解锁  
 --- --- --- 
  M0   Foundation：task-graph Jobs + 分配器 + Handle + Name + 反射(codegen) + Input + **模块/构建系统 + 测试地基**   一切  
  M1   数据导向内核：archetype + Query + Scheduler + **Authoring 门面**   世界  
  M2   RHI：桌面三后端 + 主机接缝 + bindless + timeline + AS + indirect + query + residency   GPU  
  M3   RenderGraph(真 async/别名) + GpuScene + Extract + Camera + **PSO 异步预编译**   渲染地基  
  M4   **meshlet GPU-driven 延迟**（两阶段 HiZ 剔除 + 间接 G-Buffer + HLOD）   海量几何（主线）  
  M5   Clustered 延迟 + VSM 阴影 + 天空/体积/贴花   光照  
  M6   混合 RT：AS + RT 阴影/反射 + **探针/辐照缓存 GI** + 降噪   画质（主线 GI）  
  M7   合成/透明/后处理（TAA/上采样/HDR 输出）   成帧  
  M8   Asset Import/Cook/DDC + 流式（虚拟纹理/几何）+ 版本迁移 + VFS   开放世界内容  
  M9   Feature 系统（Physics/Anim/AI/Audio/Net/Script/VFX/UI）   玩法  
  M10   Editor（含 PIE/材质图/Sequencer）+ Profiler + 跨后端 golden 硬门禁   生产化  
  M11   发行：Package/VFS + Shipping Cook + Patching + Console + 崩溃/无障碍/反作弊 + 构建配置   出货  
  **M12**   **研究级可叠加档（可选开关，不阻塞主线）**：VisBuffer 软光栅 · ReSTIR GI · 分层材质 · GPU Work Graphs   画质/几何上限  

## 22. 验收标准

- 数据导向内核为世界存储 + **一等 authoring 门面**；系统经 Scheduler 跑 task-graph（确定性子域有序）。
- GPU-driven indirect 为主提交路径；bindless 无 per-draw 描述符绑定。
- RenderGraph 真异步 + 真别名；能力位与实现一致（CI 校验）。
- 主线用**已验证技术**（meshlet 延迟 + 辐照缓存 GI + VSM）；研究级档（VisBuffer/ReSTIR/分层材质/Work Graphs）为**可选开关，关闭不影响出帧**。
- 混合 RT 在 DX12/Vulkan 对等、命中点真受光。
- **PSO 异步预编译 + 缓存，运行时无着色器编译 hitch**。
- 渲染只读 Extract 快照、与游戏线解耦；sim 确定性子域可复现、可录制重放。
- Editor 经引擎渲染、编辑真实世界、PIE、存读 + undo/redo。
- Asset 流式有预算、与显存驻留联动；importer 产真实产物、稳定 GUID 跨重启、版本可迁移。
- RHI 主机接缝可接入而不漏原生类型；可一键产出**可发行包**。
- **测试为真实行为非源码 grep；跨后端 golden 实跑硬门禁；性能/确定性回归门禁绿**；模块依赖 DAG 无环（CI 强制）。

## 23. 取舍与待定

**有意接受的代价**：弃 DX11/GL；混合数据模型需维护门面↔数据层桥；多年级别系统工程。研究级档（VisBuffer/ReSTIR/分层材质）复杂度高，故置于 M12 可选。

**已知约束**：**跨平台/跨编译器浮点确定性**是硬问题——已由 **D11 确定性子域**（sim/physics 定点/有序）框定，非全引擎逐位一致；§17 确定性回归守护。

**显式范围决策（见 §0）**：MSAA(否,TAA-only)、**VisBuffer(Phase-2/M12)**、**GI(Probe/烘焙/SSGI Proven 兜底；Radiance Cache M6 目标能力；ReSTIR 高端)**、VR/XR(否,预留接缝)、多 GPU(否)、Gameplay 脚手架(仅接缝)、可视化脚本(否)、Mod(预留 VFS 接缝)、多人协作编辑器(否)。

**待定（不阻塞主线）**：辐照缓存混合的探针布局/世界缓存策略（M6 前 spike）；上采样器选型；脚本语言（LuaJIT vs 自定义 VM）；音频中间件（自研 vs Wwise/FMOD）；World Partition 流式粒度；codegen 工具链（libclang 版本/集成方式）。

## 24. 后续实施与迁移评估（informative）

本文只定义北极星目标架构。当前实现的 keep/adapt/rewrite、桥接策略、阶段迁移、兼容层和排期均在目标设计完成后另起实施方案。

## 25. 最佳实践校准与分级路线

> 本节记录"为何这么校准"，使每个高风险决策对标到出货引擎实践。

**对标结论**：机制层（bindless/GPU-driven/RenderGraph/VSM/混合 RT/数据导向快照/可测性）已贴 SOTA。四项原激进押注降为分级、两项可行性缺口补齐、三项张力消解：

  原押注   业界最佳实践   校准  
 --- --- --- 
  纯 ECS-first   UE5=Actor+Mass、Unity=GO+DOTS（混合）   D1 混合，门面一等  
  VisBuffer 首期一等   仅 UE Nanite 出货；多数 AAA 用 meshlet 延迟   D5 meshlet 延迟主线，VisBuffer→M12  
  ReSTIR 为主 GI   Lumen=辐照缓存混合，非纯 ReSTIR   D7 Probe/SSGI Proven 兜底，Radiance Cache M6 目标能力，ReSTIR 高端  
  强制 fiber   UE5 Tasks/enkiTS 无 fiber   D2 task-graph，fiber 可选  
  仅 3 桌面后端   主机用 GNM/AGC、NVN   D4 加主机接缝  
  无 codegen 反射   标准 C++ 当前无静态反射   D8 libclang codegen→C++26  

**分级路线（ambition 通过分层兑现）**：

  能力   proven（首期主线）   advanced（可选）   research（M12 可叠加）  
 --- --- --- --- 
  几何   meshlet GPU-driven 延迟   —   VisBuffer 软光栅  
  GI   探针/烘焙   辐照缓存混合(Lumen 式)   ReSTIR  
  材质   固定 shading model   —   分层 BSDF(Substrate 式)  
  剔除→draw   ExecuteIndirect   —   GPU Work Graphs  
  反射   libclang codegen   —   C++26 静态反射  
  并行   task-graph   —   fiber 后端  

**元原则**：每个研究级能力都有低档兜底，使引擎**早期即可出画面/过 golden gate**，高档作为开关叠加——任一研究级翻车不阻塞主线。一句话：**不赌"终态多先进"，赌"每一步都出货、先进度可叠加"。**






