# RVX-NG · Cross-cutting/崩溃与遥测 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §15/§16  
**里程碑**：M10-M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供崩溃捕获、诊断上下文、最小遥测、性能/质量事件和安全上报接缝，支持开发调试、测试回归和 Shipping 质量监控。

**范围内**：minidump、callstack、symbol id、last trace ring、device removed、OOM、assert、telemetry event、privacy flags、offline cache。  
**范围外**：具体云服务实现、数据仓库、用户运营分析。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 崩溃安全 | 崩溃路径只做最小安全工作 |
| 隐私 | 遥测字段分级，用户数据默认不采集 |
| 可关联 | report 包含 build id、asset/package manifest、GPU/driver、last trace |
| 离线缓存 | 无网络时本地缓存，下次提交 |
| 可裁剪 | Shipping/Development/Test 字段和采样率不同 |

## 3. 公共接口面（设计契约）

```cpp
class CrashReporter {
    void InstallHandlers(CrashConfig config);
    void Capture(CrashContext context);
    void SubmitPendingReports();
};

class Telemetry {
    void Event(Name name, TelemetryPayload payload, PrivacyClass privacy);
    void Counter(Name name, double value);
    void Flush();
};
```

**不变量**：crash handler 不调用复杂引擎 API；遥测事件必须声明 privacy class；Shipping telemetry 可以全局关闭；报告不包含源资产路径以外的敏感本地路径。

## 4. 数据结构与内存布局

- **CrashContext**：exception、thread、module list、GPU state、memory pressure、frame id。
- **Trace Ring**：最近 N 秒关键事件和 counters。
- **Report Envelope**：build id、platform、package manifest hash、user consent state。
- **Telemetry Queue**：bounded queue，按 priority/采样率丢弃。

## 5. 核心算法

- **Crash capture**：异常/terminate/device lost/OOM → 写 minidump + trace ring → 标记 pending。
- **Symbolication**：本地或后台服务按 build id 匹配 symbols。
- **Telemetry batching**：按时间/大小批量写出，失败缓存。
- **Privacy filter**：提交前根据 privacy class 和用户设置过滤字段。
- **Safe mode signal**：连续崩溃记录用于下次启动禁用非核心模块。

## 6. 线程与内存模型

正常遥测由后台 job flush。崩溃路径使用预分配缓冲和平台安全 API。Telemetry queue bounded，不能阻塞游戏线程或实时音频线程。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| report 写入失败 | 尝试最小文本报告 |
| 网络不可用 | 本地 pending cache |
| queue 满 | 丢弃低优先级事件并计数 |
| 用户禁用遥测 | 只保留本地 crash 或完全关闭提交 |

**禁止**：崩溃 handler 触发二次复杂分配；未标 privacy class 的字段提交；telemetry 阻塞帧。

## 8. 性能目标

- 常规 telemetry event 开销极低。
- 崩溃报告足够定位 build、平台、最后帧状态。
- Pending report 不影响下次启动主路径。

## 9. 测试计划

- crash/assert/device lost/OOM 注入。
- symbol id 匹配、pending cache、网络失败。
- privacy filter 单测。
- telemetry queue 满与采样率测试。

## 10. 开放问题 / Spike

- 后端协议与 report schema。
- Trace ring 大小与字段策略。
- Safe mode 触发阈值。
- 平台隐私合规要求矩阵。

## 11. 依赖与被依赖

- **依赖**：Log、Profiler trace、Memory budget、RHI device removed、Shipping package manifest。
- **被依赖**：Support, QA, CI stress, Shipping quality monitoring。


