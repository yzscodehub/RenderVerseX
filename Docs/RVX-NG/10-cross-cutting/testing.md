# RVX-NG · Cross-cutting/测试与可测性 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自契约 C10  
**里程碑**：M0-M11 贯穿  
**状态**：详细设计 · 待 S12 Spike

---

## 1. 目标与范围

**目标**：把可测性作为架构约束，而不是上线前补丁：每个 capability、公共接口、预算模型、fallback、错误路径和跨后端行为都必须有自动化验证入口、可归因 artifact 和明确门禁。

**范围内**：测试层级、fixture、headless RHI、capability 验证、golden image、性能基线、determinism replay、fuzz、压力、故障注入、GPU farm 分层、CI gate、测试数据管理。  
**范围外**：具体 CI 服务商配置、人工 QA 流程、平台认证流程本身。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 行为验证 | 测试真实行为和可观察输出，禁止用源码字符串或注释存在性冒充 |
| 可重复 | 每次运行记录 build id、平台、toolchain、RHI backend、GPU/driver、资产 manifest、seed、profile |
| 分层 | 无 GPU 环境可运行大多数单元、契约、序列化、fuzz 和确定性测试 |
| 能力诚实 | 每个 capability bit 必须有 pass/fail/skip 可执行 fixture，不支持时报告 false 与 fallback |
| 可诊断 | 失败必须产出 trace、diff、capture、日志片段和最小复现输入 |
| 门禁 | regression 阻断对应 profile；flaky 只能进入隔离池，不能保护主线通过 |
| 预算 | PR 快速门禁、nightly 完整矩阵、长期 soak/fuzz 分层运行，不能互相拖垮 |

## 3. 公共接口面（设计契约）

```cpp
enum class TestLane : uint8 {
    Unit,
    Contract,
    Integration,
    Golden,
    Performance,
    Determinism,
    Fuzz,
    Soak
};

struct TestEnvironment {
    PlatformId platform;
    RHIBackendId backend;
    GpuProfileId gpuProfile;
    BuildProfile buildProfile;
    AssetManifestId assetManifest;
    uint64 seed = 0;
};

struct TestManifest {
    Name name;
    ModuleId owner;
    TestLane lane;
    Span<const Tag> tags;
    Span<const CapabilityId> requiredCaps;
    Span<const AssetFixtureId> fixtures;
    Duration timeout;
    bool destructive = false;
};

class ITestRegistry {
public:
    virtual void Register(TestManifest manifest, TestEntryPoint entry) = 0;
    virtual Span<const TestManifest> Enumerate() const = 0;
};

class ITestRunner {
public:
    virtual TestRunResult Run(TestId test, const TestEnvironment& env) = 0;
    virtual ArtifactBundle CollectArtifacts(TestRunId run) = 0;
};

class TestWorldFixture {
public:
    World& CreateWorld(TestWorldDesc desc);
    void StepFixed(uint32 steps);
    StateHash CaptureStateHash(StateHashMask mask) const;
};

class GoldenImageTest {
public:
    void RenderFixture(Name fixture, RenderProfile profile);
    ImageDiff Compare(GoldenId golden, ToleranceProfile tolerance);
    ArtifactBundle ExportFailureBundle() const;
};

class PerfGate {
public:
    void RecordMetric(Name metric, double value, MetricUnit unit);
    GateResult CompareBaseline(Name baseline, PerfPolicy policy);
};

class FaultInjectionScope {
public:
    explicit FaultInjectionScope(FaultInjectionConfig config);
    ~FaultInjectionScope();
};
```

**不变量**：

- 每个 capability bit 都有至少一个 contract test；capability 为 false 时必须验证 fallback 或 skip reason。
- Golden 失败必须输出原图、参考图、diff、mask、RenderGraph capture、RHI backend、GPU/driver 与 tolerance profile。
- 性能门禁只在稳定环境阻断；非稳定环境只能记录趋势，不得伪装为硬门禁。
- Test fixture 拥有引擎生命周期，测试之间不能共享可变全局状态。
- Release/Shipping profile 的安全断言、解析器失败和 package 验签路径必须有测试覆盖。

## 4. 数据结构与内存布局

- **Test Manifest**：测试名、owner、lane、tags、capability、fixture、硬件需求、timeout、artifact policy。
- **Environment Stamp**：OS、CPU、GPU、driver、RHI backend、feature tier、toolchain、build profile、asset manifest hash。
- **Golden Record**：fixture id、render profile、reference image、颜色空间、曝光、tolerance profile、mask、历史通过范围。
- **Tolerance Profile**：逐像素阈值、区域 mask、HDR 亮度容差、感知指标阈值、允许波动原因。
- **Perf Baseline**：metric、单位、平台 lane、样本窗口、方差、硬阈值、趋势阈值。
- **Artifact Bundle**：stdout/stderr、结构化日志、trace、crash dump、RenderGraph capture、GPU timing、diff image、最小输入。
- **Fault Injection Config**：OOM、device lost、IO failure、shader compile fail、packet loss、corrupt asset、权限拒绝。
- **Fuzz Corpus**：资产、序列化、网络包、脚本、replay、package manifest 输入样本及最小化结果。

大型 artifact 进入内容寻址存储；测试结果只保存 hash、metadata 和 retention policy。Golden 与 fuzz corpus 按 schema 版本管理，避免测试数据本身成为不可追踪的隐式依赖。

## 5. 核心算法

- **Test Selection**：按 changed module、lane、tags、capability、hardware、profile 选择测试；无法满足环境的测试返回 `SKIP` 并记录原因。
- **Capability Coverage**：从 RHI/Feature capability matrix 生成 required tests，缺 fixture 时 meta-test 失败。
- **Fixture Lifecycle**：创建隔离 temp root、VFS mount、asset registry、world、RHI device；运行结束按逆序关闭并检查泄漏。
- **Golden Compare**：先校验 metadata，再统一颜色空间/曝光；contract buffer 使用精确或数值容差，最终图像使用 mask + 像素阈值 + 感知指标组合。
- **Golden Promotion**：参考图只能由授权 lane 生成；promotion 必须绑定 fixture、profile、artifact 和原因，不能静默覆盖。
- **GPU Farm Scheduling**：PR lane 跑 headless/contract 和少量 smoke GPU；nightly 跑 reference GPU；scheduled lane 覆盖后端矩阵；perf lane 固定指定硬件。
- **Performance Gate**：丢弃 warmup，采集多样本，按中位数/分位数和方差判断；高噪环境降级为趋势记录。
- **Determinism Gate**：重放输入日志，按系统级、entity/component、physics/network 层级比较 state hash。
- **Fuzz**：解析器、反序列化、网络包、脚本输入、package/replay reader 持续 fuzz；崩溃样本最小化后进入 corpus。
- **Fault Injection**：统一注入 OOM、IO fail、device lost、shader compile fail、package corrupt 等失败，验证错误语义和 fallback。

## 6. 线程与内存模型

测试 runner 是 engine owner，负责启动和关闭所有子系统。并行测试必须隔离 temp root、VFS、asset registry、network port、RHI device 和随机 seed。CPU-only 测试可按模块并行；GPU 测试按设备池串行或限并发调度；实时音频、socket、GPU capture 等独占资源必须声明 lock。

测试 fixture 使用专用 allocator category，运行结束检查泄漏。Artifact 写入后台线程，不能阻塞实时音频线程或 GPU submit 路径。Fuzz 与 soak lane 使用独立进程隔离，避免破坏 runner 主进程状态。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 环境不满足 | `SKIP` 并记录 capability/hardware/profile 原因，不算 `PASS` |
| 测试崩溃 | `FAIL`，保存 crash dump、seed、最小输入和 runner heartbeat |
| Golden mismatch | `FAIL`，输出 reference/current/diff/mask/capture |
| Perf 波动 | 自动重跑确认；仍超阈值则 `REGRESSION`，噪声过高则转趋势记录 |
| Flaky | 标记 quarantine、owner、到期日；不能计入主线通过率 |
| Artifact 丢失 | 测试视为 infra failure，不得降级为 pass |
| Fuzz timeout | 保存输入并归类为 hang，和 crash 一样进入 triage |

**禁止**：跳过测试后报成功；无断言 sample 作为验证；Release assert 静默无覆盖；Golden 自动覆盖参考图；性能测试在未知硬件上硬阻断。

## 8. 性能目标

- PR 快速门禁覆盖 unit/contract/changed integration，目标在开发循环可接受时间内完成。
- GPU smoke 只覆盖少量高信号 fixture；完整 golden/perf/backend matrix 放入 nightly 或 scheduled lane。
- 单个 test fixture 默认 timeout 必须显式声明，长测进入 soak/fuzz lane。
- Artifact 生成开销不应改变被测帧的同步语义；GPU timestamp 查询延迟解析。
- Test selection 能按模块影响范围收敛，避免所有改动触发全矩阵。

## 9. 测试计划

本文件本身通过 meta-test 验证：

- Test manifest schema、owner、tags、timeout、artifact policy 完整。
- Capability matrix 每个 bit 有对应 fixture 或明确 non-applicable 记录。
- Golden record 包含 metadata、tolerance、mask、reference hash 和 promotion 记录。
- Perf baseline 存在样本窗口、方差、硬阈值和趋势策略。
- Fault injection 被真实调用，失败路径产生可断言结果。
- Fuzz corpus 可加载、最小化样本可复现。
- Quarantine 测试必须有 owner 和到期日，过期自动 fail。

## 10. 开放问题 / Spike

- **S12 Golden image tolerance and GPU farm**：关闭 reference GPU 分层、backend tolerance、artifact 格式和 promotion 流程。
- Golden store 的大文件存储、retention、review 权限和复制策略。
- HDR/色彩管理 fixture 的统一观察空间与容差单位。
- Fuzz/soak 的长期算力预算和最小化服务形态。

## 11. 依赖与被依赖

- **依赖**：Build System、RHI headless、Profiler/Trace、Asset fixtures、Crash/Telemetry、Determinism、Replay。
- **被依赖**：所有模块、CI gates、Release qualification、capability honesty、performance regression。M10/M11 之前必须形成可评审门禁。
