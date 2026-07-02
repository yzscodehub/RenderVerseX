# RVX-NG · Cross-cutting/构建系统 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自契约 C1/C7  
**里程碑**：M0-M11 贯穿  
**状态**：详细设计 · 待 S13 Spike

---

## 1. 目标与范围

**目标**：定义模块化、可裁剪、可验证、可复现、可跨平台的构建系统。构建系统必须把模块依赖 DAG、feature flags、build profiles、codegen、shader compile、asset cook、test discovery 和 shipping package 串成可审计的图，而不是一组隐式脚本约定。

**范围内**：模块声明、依赖图、配置矩阵、平台 toolchain、公共/私有 include 约束、codegen、shader compile、asset cook、test discovery、build cache、packaging hooks、CI 门禁。  
**范围外**：具体 IDE 工程 UI、第三方包管理服务运营、平台商认证流程。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| DAG | 模块依赖单向无环，配置阶段 fail-fast |
| 可裁剪 | Debug/Development/Test/Shipping profile 明确定义模块、feature 和符号策略 |
| 可复现 | toolchain、第三方依赖、codegen、shader、cook 输入可锁定并可哈希 |
| 可发现 | tests、assets、shaders、codegen schema、fixtures 自动注册到构建图 |
| 可验证 | public/private dependency、include graph、Shipping 裁剪、capability fixtures 自动门禁 |
| 可扩展 | 插件/feature 可声明依赖、平台支持、ABI 边界和 profile 可用性 |
| 可诊断 | 每个生成产物能追溯输入、工具版本、命令行、环境 stamp |

## 3. 公共接口面（设计契约）

模块 manifest 是目标架构的构建事实源；具体后端可以是生成的 CMake/Ninja/IDE 工程或其他后端，但后端不得绕过 manifest 约束。

```toml
[module]
name = "Render"
type = "runtime"
visibility = "public"
public_deps = ["Foundation", "RHI", "Asset"]
private_deps = ["ShaderCompiler"]
platforms = ["windows", "linux", "macos"]
profiles = ["Debug", "Development", "Test", "Shipping"]
features = ["raytracing", "mesh_shader", "virtual_shadow_map"]

[headers]
public = ["Include/Render"]
private = ["Private"]
forbidden_public_includes = ["Private", "Editor"]

[generated]
reflection = false
shaders = ["Shaders/**/*.hlsl"]
assets = ["Fixtures/**/*.rvxasset"]
tests = ["Tests/Render*"]
```

```cpp
enum class ModuleType : uint8 {
    Foundation,
    Runtime,
    Editor,
    Tool,
    Test,
    Plugin,
    Shipping
};

struct BuildProfile {
    Name name;
    bool enableAsserts = true;
    bool enableEditor = false;
    bool enableInstrumentation = true;
    bool allowRuntimeShaderCompile = false;
};

class BuildGraphValidator {
public:
    ValidationResult Validate(ModuleGraph graph, BuildProfile profile);
    IncludeAuditResult AuditPublicHeaders(ModuleId module);
    ProfileAuditResult AuditProfileClosure(BuildProfile profile);
};

class GeneratedNodeRegistry {
public:
    void AddCodegenNode(CodegenDesc desc);
    void AddShaderNode(ShaderCompileDesc desc);
    void AddCookNode(AssetCookDesc desc);
    void AddTestNode(TestManifest manifest);
};
```

**不变量**：

- Public headers 只能 include public dependency 的 public headers；private dependency 类型不得出现在 public API。
- Runtime/Shipping 模块不能依赖 Editor/Tool/Test 模块；Test profile 可以依赖 test fixtures，但必须显式声明。
- Codegen、shader compile、asset cook 都是构建图节点，必须声明输入、输出、工具版本和 cache key。
- 生成文件写入 build/generated 命名空间；source tree 中的手工文件不得依赖生成文件的未声明路径。
- Unsupported feature 在 optional profile 中禁用并记录；required feature 不满足时 fail-fast。

## 4. 数据结构与内存布局

- **Module Manifest**：name、type、visibility、public/private deps、platforms、profiles、features、include roots、generated nodes。
- **Module Graph**：module node、dependency edge、visibility edge、profile closure、plugin boundary。
- **Build Node**：tool id、input files、input hashes、output files、environment stamp、cache key、dependency edges。
- **Feature Matrix**：platform × backend × feature × profile，区分 required、optional、unsupported。
- **Toolchain Lock**：compiler、linker、SDK、shader compiler、codegen tool、cook tool、third-party package 版本。
- **Generated Registry**：reflection glue、shader reflection headers、asset metadata、test manifests、package manifests。
- **Profile Policy**：Debug/Development/Test/Shipping 的 asserts、symbols、instrumentation、editor-only、runtime compile 规则。
- **Audit Report**：include leak、dependency cycle、profile violation、stale generated output、non-reproducible input。

构建图数据是离线工具数据，不进入 runtime 热路径。运行时只消费生成后的 registry、package manifest、shader/asset cook 产物和能力表。

## 5. 核心算法

- **Manifest Parse**：读取所有模块 manifest，schema 校验后生成 module graph；未知字段按 schema 规则 fail-fast 或警告。
- **Graph Validate**：拓扑排序依赖 DAG，检测环、非法方向、重复 module id、public/private 越界。
- **Include Audit**：扫描 public headers 的 include graph，验证只触达 public deps；第三方公开 ABI 必须显式声明。
- **Profile Resolve**：根据 platform/profile/features 计算 target closure、defines、toolchain、link set、generated nodes。
- **Feature Resolve**：required feature 不支持则失败；optional feature 不支持则关闭并写入 capability/fallback manifest。
- **Incremental Codegen**：以源文件、注解 schema、tool version、compiler args 计算 hash；输入变更才重跑，失败不复用陈旧产物。
- **Shader/Cook Integration**：shader permutation、PSO precompile、asset cook、DDC key 作为 build graph 节点，参与缓存命中和测试影响分析。
- **Test Discovery**：从 manifest 和 generated registry 收集测试，输出 testing 系统可消费的 TestManifest。
- **Impact Selection**：根据 changed files → build nodes → modules → tests 的反向图选择 PR gate 范围。
- **Shipping Audit**：验证 package manifest、runtime shader compile 禁止、editor-only 符号裁剪、源资产不进入 runtime package。

## 6. 线程与内存模型

构建系统离线并行执行独立节点。每个工具节点必须有明确输入输出，禁止读取隐式全局状态；必须读取环境的部分进入 environment stamp。生成文件写入专用 build 目录，节点之间通过声明产物传递。多个节点写同一路径是配置错误。

Codegen、shader compile、asset cook 可以并行，但共享 cache/DDC 写入需使用内容寻址和原子提交。构建工具自身的内存峰值纳入 CI 资源预算，避免 shader/cook 批处理把 worker 打满后拖垮测试 lane。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 依赖环 | 配置阶段失败，输出最短环路径 |
| Public header 泄露 private 类型 | fail-fast，输出 include chain 与违规 symbol |
| Profile 非法依赖 | fail-fast，输出 profile closure 和违规模块 |
| Codegen 失败 | 停止构建，不使用陈旧产物 |
| Shader/cook cache key 冲突 | 视为构建系统错误，隔离 cache entry 并失败 |
| Feature unsupported | optional 关闭并记录；required 失败 |
| Toolchain 未锁定 | Development 可警告，Test/Shipping 失败 |
| 非复现输入 | 失败或降为非门禁工具节点，不能进入 Shipping artifact |

**禁止**：公共 API 泄露 private/third-party 类型；Shipping 静默包含 Editor；生成文件手工修改；构建脚本隐式扫描未声明目录；codegen/cook 失败后继续打包。

## 8. 性能目标

- 增量构建只重跑受影响 codegen、shader、cook 和测试节点。
- 构建图解释、验证、include audit 的开销应低于一次小型编译批次。
- Impact selection 能把大多数局部改动限制在相关模块和 contract tests。
- Shader permutation 与 asset cook 必须支持 cache/DDC 命中统计，避免无界重烘焙。
- CI 能分离 PR gate、nightly matrix、release qualification，避免所有改动触发最重 profile。

## 9. 测试计划

- Manifest schema：缺字段、未知 module type、重复 name、非法 feature/profile。
- DAG：依赖环、跨层非法依赖、public/private include leak、第三方 ABI 泄露。
- Profile：Debug/Development/Test/Shipping closure、Editor 裁剪、runtime shader compile 禁止。
- Codegen：增量 hash、失败不复用旧产物、清理 generated 目录后可复现。
- Shader/Cook：cache key、include invalidation、DDC hit/miss、package manifest 输出。
- Test discovery：manifest 到 TestManifest 的完整性、tags、capability、fixture 关联。
- Impact selection：文件变更映射到 build nodes/modules/tests 的正确性。
- Shipping artifact audit：无 source asset、无 editor-only 模块、符号/telemetry/privacy profile 正确。

## 10. 开放问题 / Spike

- **S13 Build meta-manifest**：验证 manifest schema、后端生成/导出策略、include graph 工具和 profile 裁剪门禁。
- Build cache 与 Asset DDC 是否共享内容寻址存储、权限和 eviction policy。
- 插件 ABI 与版本策略，尤其跨 DLL 边界是否允许 STL/第三方类型。
- ShaderCompiler 是 RHI 下层工具、Render 旁路服务，还是 Build/Cook 独立工具节点。

## 11. 依赖与被依赖

- **依赖**：Foundation conventions、Global Dependency DAG、Reflection codegen、ShaderCompiler、Asset Cook、Testing。
- **被依赖**：所有模块、CI、Shipping、Plugin system、DDC、capability fixtures。M0 需要最小 manifest/graph validator，M11 需要完整 Shipping audit。

