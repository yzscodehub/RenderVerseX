# RVX-NG · Feature/Online & Platform Services 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 Networking、Platform、Shipping/Security  
**里程碑**：M9 / M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供跨平台在线服务抽象：身份、认证、好友、presence、session/lobby、matchmaking、achievements、leaderboards、cloud save、entitlements、store/IAP、platform external UI，使游戏逻辑不直接绑定 Steam/EOS/PSN/Xbox/Nintendo/移动平台 SDK。

**范围内**：account/auth、platform user、session/lobby、matchmaking、friends/presence、achievements、leaderboards、cloud save、entitlement/store、invites、external UI、安全与隐私接缝。  
**范围外**：具体平台后台运营、商业策略、反作弊服务商实现、游戏特定匹配算法细节。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 平台隔离 | Runtime public API 不泄露平台 SDK 类型 |
| 异步 | 所有网络/平台调用返回 request handle/future，不阻塞 game thread |
| 权限明确 | 隐私、年龄、跨平台、UGC、好友数据需要 capability/consent |
| 可降级 | 离线模式、无平台服务、开发 sandbox 都有明确 fallback |
| 可测试 | mock provider、fault injection、rate limit、auth fail 可自动化 |

## 3. 公共接口面（设计契约）

```cpp
enum class OnlineFeature : uint16 {
    Auth,
    Friends,
    Presence,
    Sessions,
    Matchmaking,
    Achievements,
    Leaderboards,
    CloudSave,
    Store,
    ExternalUi
};

struct OnlineUser {
    PlatformUserId platformId;
    Name displayName;
    PrivacyFlags privacy;
};

class OnlineServices {
public:
    AsyncResult<LoginResult> Login(LocalUserIndex user, LoginOptions options);
    AsyncResult<SessionHandle> CreateSession(SessionDesc desc);
    AsyncResult<JoinResult> JoinSession(SessionHandle session);
    AsyncResult<void> UnlockAchievement(Name id);
    AsyncResult<CloudBlob> LoadCloudSave(Name slot);
    bool Supports(OnlineFeature feature) const;
};
```

**不变量**：平台 SDK 对象只存在 provider 私有实现；用户隐私/permission 未通过时不得返回敏感数据；所有回调进入主线程/online dispatcher；跨平台 ID 与本地 profile ID 明确区分。

## 4. 数据结构与内存布局

- **Provider**：Steam/EOS/PSN/Xbox/Nintendo/Mobile/Null provider，实现统一接口。
- **Platform User**：local index、platform id、crossplay id、display name、privacy flags。
- **Auth Token**：scope、expiry、refresh policy、secure storage handle。
- **Session/Lobby**：owner、members、connection string、join policy、attributes、version。
- **Presence**：state、rich text key、joinable flag、privacy filter。
- **Cloud Save Blob**：slot、etag/version、size、hash、conflict metadata。
- **Entitlement**：product id、ownership state、platform receipt/ref。

## 5. 核心算法

- **Provider Resolve**：按 platform/profile 选择 provider，Development 可使用 Null/Mock。
- **Auth Flow**：login → token → platform capabilities → privacy/age gate → user ready。
- **Session Flow**：create/find/join/leave/update attributes；Networking 只消费 resolved endpoint/session ticket。
- **Cloud Save Conflict**：etag/version 比较，本地/云端/手动 merge 策略。
- **Achievement/Leaderboard**：本地 queue + rate limit + retry；离线缓存到 profile。
- **External UI**：邀请、账号、商店等通过 provider 发起，不在引擎 UI 中模拟平台受控流程。

## 6. 线程与内存模型

平台 SDK 回调进入 Online dispatcher，再在主线程安全点发布事件。Online provider 可以拥有平台要求的内部线程，但必须在 App/Platform 声明，不允许 Feature 私自创建长期线程。敏感 token 不进入普通日志、crash dump 或 telemetry。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| provider 不支持能力 | `Supports=false`，功能 UI 隐藏或走 offline fallback |
| auth 失败/取消 | 返回明确错误码，保留离线 profile |
| 网络超时/rate limit | 指数退避，保留 pending queue |
| cloud save 冲突 | 进入 conflict resolution，不覆盖数据 |
| 隐私/年龄限制 | 不返回敏感字段，不绕过平台限制 |
| platform SDK crash/fail | provider 隔离，记录诊断，必要时禁用在线功能 |

**禁止**：gameplay 直接调用平台 SDK；日志输出 token/PII；offline fallback 冒充 online success。

## 8. 性能目标

- Online callbacks 每帧处理有预算，避免大量好友/session 更新卡主线程。
- Cloud save 和 entitlement 校验异步，不阻塞启动关键路径。
- Session browse 有分页/rate limit，结果缓存有 TTL。

## 9. 测试计划

- Mock provider：login、cancel、timeout、rate limit、permission denied。
- Session create/find/join/leave、invite、connection handoff to Networking。
- Cloud save conflict、etag mismatch、offline queue replay。
- Privacy redaction、token 不进 log/crash/telemetry。
- Shipping profile：unsupported feature 不显示或正确降级。

## 10. 开放问题 / Spike

- 首批 provider：Null/EOS/Steam/主机平台接缝的优先级。
- Crossplay ID 与平台 ID 的映射和合规边界。
- Cloud save 与本地 Save/Profile 的冲突 UI 归属。
- Store/IAP 是否进入首期或只预留接口。

## 11. 依赖与被依赖

- **依赖**：Platform、Networking、Security/Anti-cheat、Save/Profile、Crash/Telemetry、Localization/UI。
- **被依赖**：Multiplayer UX、Achievements、Cloud Save、DLC/Entitlements、Shipping platform readiness。
