# RVX-NG · Shipping/Packaging & Release 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §16 Shipping  
**里程碑**：M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供从 cooked 内容到可发行包的完整生产化链路：Pak/VFS、平台分包、Shipping Cook、PSO/shader 预缓存、patch/DLC、崩溃上报、遥测、无障碍、反作弊与构建配置裁剪。

**范围内**：package 格式、VFS 挂载策略、cook manifest、patch diff、DLC、build config、crash dump、symbol、telemetry hook、platform SDK 接缝。  
**范围外**：具体平台认证流程文档、商店后台运营、反作弊服务商实现细节。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 可复现 | 同输入 cook/package 输出可复现 |
| 可增量 | patch/DLC 以 chunk/manifest 差异生成 |
| 可裁剪 | Shipping 移除 editor/debug-only 代码和资产 |
| 可恢复 | 崩溃、device lost、资产损坏有报告和安全路径 |
| 平台化 | 平台 SDK 差异收敛到 Platform/Shipping 接缝 |

## 3. 公共接口面（设计契约）

```cpp
class PackageBuilder {
    PackageManifest Build(CookManifest cook, PackageProfile profile);
    PatchManifest BuildPatch(PackageManifest oldPkg, PackageManifest newPkg);
};

class RuntimePackageSystem {
    void Mount(PackageId package, MountPriority priority);
    void Unmount(PackageId package);
    VfsFile Open(VfsPath path);
};

class CrashReporter {
    void Capture(CrashContext context);
    void Submit(CrashReport report);
};
```

**不变量**：运行时只读取 mounted VFS，不直接依赖 loose source；package manifest 记录 hash、版本、依赖、平台；Shipping 禁止 editor-only 依赖；patch 应可验证完整性并回滚。

## 4. 数据结构与内存布局

- **Package Manifest**：文件表、chunk id、hash、compression、encryption、dependencies。
- **Cook Manifest**：asset id、cooked blob hash、shader/PSO entries、target profile。
- **Patch Manifest**：old/new chunk map、delta、校验、回滚信息。
- **Crash Report**：minidump、callstack、build id、device info、last trace ring。
- **Build Profile**：Debug/Development/Test/Shipping 特性裁剪表。

## 5. 核心算法

- **Packaging**：按 profile 选择 cooked blobs → chunk 分组 → 压缩/加密 → manifest。
- **Patch**：比较 manifest hash，生成 chunk delta 或 whole-chunk 替换。
- **Runtime Mount**：VFS 按 priority 解析 pak/loose/DLC/mod 接缝。
- **Shader/PSO Shipping Cook**：收集 permutation 与 pipeline library，发行前预烘焙。
- **Crash**：异常捕获 → minidump + trace ring → symbolicated backend 或本地报告。
- **Build裁剪**：编译期/链接期/资产 manifest 三层剥离 editor/debug-only 内容。

## 6. 线程与内存模型

Package build 是离线工具，可并行压缩、hash、delta。运行时 mount/unmount 在安全点执行，VFS 读路径线程安全。Crash capture 在受限环境中只做 async-signal-safe/平台允许的最小工作。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| package hash 不匹配 | 拒绝挂载，提示修复/回滚 |
| patch 失败 | 回滚旧 manifest |
| shader/PSO 缺失 | Development 可后台编译，Shipping 记录严重错误并走 fallback |
| crash submit 失败 | 本地缓存待下次提交 |

**禁止**：Shipping 依赖源资产；无 manifest 校验直接加载 package；崩溃处理路径再分配大量内存或调用复杂引擎 API。

## 8. 性能目标

- package mount 时间与文件数规模可控。
- patch 尽量按 chunk 差异最小化下载。
- Shipping 首次运行无运行时 shader 编译尖峰。

## 9. 测试计划

- package reproducibility、manifest hash、VFS priority、patch rollback。
- Shipping build dependency audit。
- crash capture 注入与 symbol 匹配。
- PSO/shader cache 完整性门禁。

## 10. 开放问题 / Spike

- Pak compression/encryption 算法选型。
- Patch delta 粒度：文件、chunk、binary diff。
- Crash/telemetry 后端接口。
- Mod 支持与反作弊/签名策略冲突。

## 11. 依赖与被依赖

- **依赖**：Asset Cook、VFS、ShaderCompiler、Build System、Platform SDK、Telemetry。
- **被依赖**：发行流程、CI/CD、Runtime loading、Support/debug。


