# RVX-NG · Cross-cutting/Third-party License Compliance 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 Build System、Project Package、Shipping、Security  
**里程碑**：M0-M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义第三方依赖、开源许可证、平台 SDK、SBOM、credits、漏洞/安全公告、版本升级和发行合规流程。引擎必须能证明每个 Shipping artifact 使用了哪些 third-party 组件、许可证是否允许、是否需要 attribution、是否存在已知漏洞。

**范围内**：third-party manifest、license policy、SBOM、attribution/credits、vulnerability advisory、NDA SDK isolation、dependency upgrade、binary redistribution。  
**范围外**：法律意见本身、平台私有认证条款全文、远程漏洞数据库运营。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 可追踪 | 每个第三方组件有版本、来源、hash、license、用途、owner |
| 可审计 | Build/Shipping 生成 SBOM 和 attribution bundle |
| 可阻断 | 禁止许可证、未知来源、高危漏洞可 fail gate |
| ABI 隔离 | 第三方类型不泄露 public API，除非声明公开 ABI 依赖 |
| 平台合规 | NDA SDK 不进入公开包/日志/文档泄露路径 |

## 3. 公共接口面（设计契约）

```toml
[third_party]
name = "HarfBuzz"
version = "x.y.z"
source = "registry-or-vendor-path"
license = "MIT"
usage = ["runtime", "tools"]
owner = "text-system"
redistributable = true
public_abi = false
```

```cpp
class ComplianceValidator {
public:
    ComplianceResult ValidateDependencyGraph(ProjectLock lock, BuildProfile profile);
    Sbom GenerateSbom(BuildArtifact artifact);
    AttributionBundle GenerateAttribution(BuildArtifact artifact);
};
```

**不变量**：未知 license/source/hash 不进入 Shipping；禁止 license policy violation；public ABI dependency 必须显式声明；platform SDK headers/types 不进入普通 public API。

## 4. 数据结构与内存布局

- **Third-party Manifest**：name、version、source、hash、license id、usage scope、owner、redistribution rule。
- **License Policy**：allowed、restricted、forbidden、review-required、attribution-required。
- **SBOM**：artifact id、component list、versions、hashes、licenses、dependency graph。
- **Attribution Bundle**：license text、credits、notices、source offer metadata。
- **Vulnerability Record**：component、version range、severity、CVE/advisory id、mitigation state。
- **SDK Isolation Record**：NDA platform, include roots, binary redistribution rule, log redaction rule。

## 5. 核心算法

- **Dependency Scan**：Project Lock + Build Graph + Package manifests → third-party closure。
- **License Resolve**：SPDX/license id 归一化 → policy check → attribution requirements。
- **SBOM Generate**：按 build artifact 输出 component graph 和 hash。
- **Vulnerability Gate**：匹配 advisory DB/cache；critical/high 可阻断 Shipping。
- **Public ABI Audit**：扫描 public headers 和 generated bindings，检查第三方类型泄露。
- **Attribution Pack**：按 platform/package 生成 notices、credits 和 license files。

## 6. 线程与内存模型

合规扫描是离线构建/CI 工具，不进入 runtime。advisory DB 可缓存并记录更新时间；离线 CI 使用固定快照。生成的 SBOM/attribution 是 Shipping artifact 元数据，随 release 存档。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 未知 license/source/hash | Test/Shipping fail |
| forbidden license | fail，需要替换或人工豁免 ADR |
| attribution 缺失 | packaging fail |
| high vulnerability | fail 或要求 mitigation record |
| public ABI 泄露 private third-party | include audit fail |
| NDA SDK 泄露到公开 artifact | security incident + fail |

**禁止**：手工复制第三方 binary 无 manifest；Shipping 无 SBOM；平台 SDK 私有类型进入公开头。

## 8. 性能目标

- 常规 license/SBOM 扫描可增量运行。
- Advisory 匹配不阻塞本地开发主循环，只阻断 Test/Shipping gate。
- Attribution 生成可复现，输出排序稳定。

## 9. 测试计划

- license policy allowed/restricted/forbidden。
- unknown dependency、hash mismatch、missing attribution。
- public ABI leak fixture。
- vulnerability severity gate。
- SBOM reproducibility、Shipping artifact coverage。

## 10. 开放问题 / Spike

- SPDX 数据库和 advisory 数据源选型。
- 商业中间件 license 是否进入同一 manifest schema。
- 人工豁免流程：ADR、security approval，还是 release waiver。
- credits/license UI 由 Shipping 统一还是项目自定义。

## 11. 依赖与被依赖

- **依赖**：Build System、Project Package、Security、Shipping、Global DAG。
- **被依赖**：Release qualification、Plugin ecosystem、Third-party upgrades、Platform submission readiness。
