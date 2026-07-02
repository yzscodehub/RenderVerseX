# RVX-NG · Ecosystem/Project Package Plugin 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 Build System、App/ModuleGraph、Editor、Shipping  
**里程碑**：M0 / M10 / M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义 RVX-NG 的项目、包、插件、模板、feature set、依赖解析和版本兼容体系，使引擎具备商业引擎常见的可扩展生态：项目可声明依赖，插件可安装/禁用/裁剪，模板可创建新项目，package 可锁版本并参与构建、cook、测试和发布。

**范围内**：Project manifest、package manifest、plugin descriptor、feature set、template、dependency lock、semantic version、capability/profile、install/enable/disable、sample content。  
**范围外**：公开市场运营、支付系统、远程包托管服务实现。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 声明式 | 项目、包、插件都通过 manifest 声明依赖、平台、profile、能力 |
| 可复现 | dependency lock 固定版本、hash、source、toolchain 兼容 |
| 可裁剪 | Shipping 不包含 Editor/Test/sample-only 插件 |
| ABI 安全 | native plugin 遵守 global DAG 的 ABI 冻结规则 |
| 可发现 | Editor/CLI 能列出包、版本、samples、feature set 和冲突 |
| 可治理 | license、security、capability、platform support 可审计 |

## 3. 公共接口面（设计契约）

```toml
[project]
name = "SampleGame"
engine_version = "1.0"
default_map = "Assets/Maps/Main.rvxworld"

[dependencies]
"rvx.render.forward_plus" = ">=1.0 <2.0"
"rvx.feature.online" = "1.0.3"

[profiles.shipping]
features = ["render.meshlet", "asset.streaming", "online.sessions"]
disabled_plugins = ["rvx.editor.*", "rvx.test.*"]
```

```cpp
class PackageManager {
public:
    ResolveResult Resolve(ProjectManifest project);
    InstallResult Install(PackageId id, VersionRange range);
    ValidationResult ValidateLock(ProjectLock lock);
    Span<const PluginDesc> EnumeratePlugins(ProjectId project);
};
```

**不变量**：构建和 cook 只使用 resolved lock；插件不能绕过 ModuleGraph；plugin public API 不泄露 private third-party types；package 安装不自动启用运行时代码，必须由 project/profile 显式选择。

## 4. 数据结构与内存布局

- **Project Manifest**：engine version、dependency list、feature set、platform profiles、default content、settings schema。
- **Project Lock**：package id、resolved version、content hash、source URL/path、license id、transitive deps、tool compatibility。
- **Package Manifest**：modules、assets、samples、templates、licenses、capabilities、platform support。
- **Plugin Descriptor**：runtime/editor/tool/test modules、startup phase、permissions、ABI kind、settings schema。
- **Feature Set**：一组 package/plugin/config/capability 的组合，如 OpenWorld、Multiplayer、Mobile。
- **Template**：项目骨架、sample content、default settings、CI preset。

## 5. 核心算法

- **Dependency Resolve**：读取 project manifest → semver/range solve → platform/profile filter → lock generation。
- **Compatibility Check**：engine version、manifest schema、ABI version、platform support、license policy。
- **Plugin Enable**：更新 project manifest/profile，重算 ModuleGraph，检测环和非法依赖。
- **Package Import**：assets/samples/templates 按 GUID 保持稳定，避免复制导致 ID 冲突。
- **Feature Set Apply**：批量启用依赖、settings、sample scenes 和 tests，生成 review diff。
- **Update**：新 lock 与旧 lock diff，运行 migration notes、license/security checks、affected tests。

## 6. 线程与内存模型

Package resolve/install 是离线工具行为，可由 Editor 或 CLI 触发。下载/解压/校验走后台任务，但 manifest 更新必须原子写入。Runtime 只消费已解析的 ModuleGraph、package registry 和 cooked assets，不执行 package manager。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 依赖冲突 | resolve 失败，输出冲突链 |
| package hash mismatch | 拒绝安装，标记安全事件 |
| engine/plugin ABI 不兼容 | 禁用插件或要求升级，不尝试加载 |
| license policy 不允许 | resolve 阶段失败或需要人工批准 |
| plugin 启动失败 | App safe mode 禁用非核心插件 |
| template 创建失败 | 回滚目录或保留诊断日志 |

**禁止**：未锁版本进入 CI/Shipping；安装包时执行不受信任脚本；Shipping 夹带 sample/source/editor-only content。

## 8. 性能目标

- 常规项目 dependency resolve 在交互预算内完成。
- package cache 使用内容寻址，重复项目共享下载/解压结果。
- ModuleGraph 受 package 变更影响的测试可通过 impact selection 收敛。

## 9. 测试计划

- dependency solve、version conflict、lock reproducibility。
- plugin enable/disable/profile裁剪、ABI mismatch、safe mode。
- template create、sample import、GUID stability。
- license/security policy、hash mismatch、offline cache。
- Shipping audit：无 editor/test/sample/source 泄露。

## 10. 开放问题 / Spike

- 是否需要远程 registry 协议，还是首期仅支持本地/源码包。
- Package lock 是否与 build manifest 合并或独立。
- Native plugin ABI versioning 与热重载边界。
- Feature set 的 UX：Editor 向导、CLI、还是项目模板。

## 11. 依赖与被依赖

- **依赖**：Build System、Global DAG、App/ModuleGraph、Asset Pipeline、Security/License、Editor。
- **被依赖**：Project creation、Plugin ecosystem、Feature onboarding、CI/release reproducibility、Samples/templates。
