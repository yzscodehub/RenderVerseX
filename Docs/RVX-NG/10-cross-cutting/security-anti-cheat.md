# RVX-NG · Cross-cutting/安全与反作弊接缝 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §15/§16  
**里程碑**：M9-M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义引擎级安全边界和反作弊接缝：不可信输入解析、脚本沙箱、网络验证、包签名、mod 策略、telemetry 安全事件、第三方反作弊 SDK 集成边界。

**范围内**：asset/package 验签、network input validation、script sandbox、replay/package parser fuzz、rate limiting、security telemetry、anti-cheat hook interfaces。  
**范围外**：具体反作弊服务实现、平台账号风控、运营封禁策略。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 默认不可信 | 网络包、replay、mod、外部资产、脚本都按不可信输入处理 |
| 最小权限 | 脚本、插件、工具命令使用 capability whitelist |
| 可审计 | 安全相关拒绝、限流、校验失败进入 telemetry |
| 可裁剪 | 单机/开发/服务器/Shipping profile 安全策略不同 |
| 第三方隔离 | 反作弊 SDK 通过窄接口集成，不污染核心系统 |

## 3. 公共接口面（设计契约）

```cpp
enum class Capability : u32 {
    FileRead,
    FileWrite,
    Network,
    ReflectionWrite,
    ProcessLaunch,
    NativePlugin
};

class SecurityPolicy {
    bool Allows(ModuleId module, Capability capability, ResourceId resource) const;
    void Report(SecurityEvent event);
};

class AntiCheatBridge {
    void Initialize(AntiCheatConfig config);
    void SubmitSignal(AntiCheatSignal signal);
    bool IsSessionTrusted(ConnectionId connection) const;
};
```

**不变量**：任何越权访问必须拒绝并记录；网络权威逻辑不信任客户端；package/replay/parser 失败必须安全退出；反作弊桥可禁用但接口语义保持。

## 4. 数据结构与内存布局

- **Policy Table**：module、capability、resource pattern、profile。
- **Security Event**：source、capability、resource、result、rate-limit state。
- **Trust State**：connection/session trust level、anti-cheat status、platform auth status。
- **Signed Manifest**：package hash、signature、certificate/issuer metadata。

## 5. 核心算法

- **Capability check**：所有敏感 API 入口统一查询 SecurityPolicy。
- **Package verification**：mount 前验证 manifest/hash/signature。
- **Network validation**：schema、range、rate、sequence、auth 全部通过才入队。
- **Sandbox**：脚本 VM 和工具插件仅能访问授权 API。
- **Rate limiting**：按 connection、command、resource 维度限速。
- **Anti-cheat bridge**：提交引擎信号，不把核心 gameplay 权威交给 SDK。

## 6. 线程与内存模型

安全策略读多写少，运行时使用只读快照。安全事件写入 bounded queue，后台 flush。反作弊 SDK 线程隔离，跨线程回调转入引擎命令队列。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 签名/哈希失败 | 拒绝加载/mount |
| 网络输入越界 | 丢弃并计数，严重时断开 |
| capability 拒绝 | 返回错误，不执行副作用 |
| anti-cheat 不可用 | 根据 profile 决定拒绝联网或降级离线 |

**禁止**：解析不可信数据时无界分配；脚本/插件任意访问 OS；客户端状态直接覆盖服务器权威状态。

## 8. 性能目标

- capability check 在热路径可缓存。
- rate limiting 和 validation 不成为网络瓶颈。
- 安全事件不会阻塞主线程。

## 9. 测试计划

- Package/replay/network/script fuzz。
- capability whitelist/deny、rate limit、signature failure。
- anti-cheat bridge unavailable/failure injection。
- malicious packet and malformed asset corpus。

## 10. 开放问题 / Spike

- Mod 支持与 package signing 的策略边界。
- 第三方 anti-cheat SDK 的线程和隐私要求矩阵。
- Plugin sandbox 是否需要进程隔离。
- Security telemetry 与隐私设置的冲突处理。

## 11. 依赖与被依赖

- **依赖**：Platform, Networking, Scripting, Packaging, Telemetry, Build profiles。
- **被依赖**：Shipping, Multiplayer, Mod loading, Tools plugin system。


