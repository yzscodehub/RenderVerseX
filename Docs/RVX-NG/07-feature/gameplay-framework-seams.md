# RVX-NG · Feature/Gameplay Framework Seams 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 App/Engine、World、Authoring Facade、Scripting  
**里程碑**：M1 / M9  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义引擎级 gameplay 框架接缝，而不是提供具体游戏玩法框架。该层提供 GameInstance、WorldContext、GameMode/GameRules、Player、Controller、Pawn/Avatar、HUD/UI bridge、Gameplay Event、Subsystem 生命周期和脚本/原生扩展点，使项目可以构建玩法而不破坏数据导向 World、ECS、网络、存档和工具链。

**范围内**：GameInstance、WorldContext、GameMode/GameRules、PlayerSession、Controller/Pawn/Avatar 接缝、HUD bridge、GameplaySubsystem、GameplayEvent、spawn/possession、level travel、server/client 权威边界。  
**范围外**：具体 ability system、inventory、quest、dialogue、combat、AI behavior content 和项目玩法规则。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 引擎不绑定玩法 | 只提供生命周期、所有权、事件和扩展点，不内置项目规则 |
| 数据导向兼容 | 热路径状态仍在 ECS/World，门面只做 authoring 和控制接缝 |
| 网络一致 | GameMode/GameRules 权威在 server；client 只预测/显示允许数据 |
| 可序列化 | Player/Profile/World state 使用 Reflection/Save/Profile 统一版本化 |
| 可测试 | GameMode、spawn、possession、travel、HUD bridge 可用 headless fixture 验证 |

## 3. 公共接口面（设计契约）

```cpp
class GameInstance {
public:
    void Initialize(AppContext context);
    void Shutdown();
    WorldHandle CreateWorld(WorldDesc desc);
    PlayerHandle CreateLocalPlayer(LocalUserIndex index);
};

class GameRules {
public:
    virtual Result<void> OnPlayerJoin(PlayerHandle player) = 0;
    virtual Result<EntityId> SpawnDefaultAvatar(PlayerHandle player, SpawnRequest request) = 0;
    virtual void TickRules(FixedStep step) = 0;
};

class PlayerController {
public:
    void Possess(EntityId pawn);
    void SubmitInput(InputFrame input);
    EntityId GetPawn() const;
};

class GameplaySubsystem {
public:
    virtual void OnWorldStart(WorldHandle world) = 0;
    virtual void OnWorldStop(WorldHandle world) = 0;
};
```

**不变量**：GameRules 不直接调用 RHI；Controller 不直接拥有 GPU/Asset raw pointer；server-authoritative 项目中 GameRules 权威只运行在 server world；HUD 通过 UI bridge 读取 view-model，不直接修改 simulation state。

## 4. 数据结构与内存布局

- **GameInstance State**：project runtime context、local players、persistent services、current worlds。
- **WorldContext**：world handle、mode、network role、time domain、game rules instance。
- **PlayerSession**：local/remote user id、controller、profile、online session、permissions。
- **Pawn/Avatar Binding**：entity id、controller id、input mapping context、camera target、network owner。
- **Gameplay Event**：type id、payload schema、source/target handles、reliability/recording flags。
- **HUD ViewModel**：反射/数据绑定对象，连接 gameplay state 与 UI。

## 5. 核心算法

- **Startup Flow**：AppMode → GameInstance → WorldContext → GameRules → subsystem graph。
- **Player Join**：online/local user → PlayerSession → Controller → spawn/possess → camera/HUD binding。
- **Possession**：Controller 绑定 Pawn/Avatar，输入路由、camera target、network owner 同步更新。
- **Level Travel**：冻结输入 → 保存/卸载旧 world → 加载新 world → 恢复 persistent player/session。
- **Gameplay Events**：同步事件走固定步队列；非确定性 UI/audio 事件走 presentation queue。
- **HUD Bridge**：simulation state → view-model snapshot → UI system，避免 UI 直接写 ECS 热数据。

## 6. 线程与内存模型

Gameplay framework 运行在 World update 阶段和 App lifecycle 阶段。GameRules 的固定步逻辑必须遵守 deterministic scope；presentation 事件可以在 render/UI 阶段消费。跨线程事件只通过 EventBus 或 command buffer，不允许直接回调任意系统。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| GameRules spawn 失败 | 返回 Result，玩家进入 spectator/default safe pawn |
| possession target 无效 | 保持旧 pawn 或 unpossessed 状态，记录错误 |
| level travel 失败 | 回退到 last stable world 或 safe map |
| player profile 缺失 | 使用 default profile 并触发恢复流程 |
| HUD view-model schema mismatch | UI 显示 fallback/debug state，simulation 不受影响 |

**禁止**：把项目 ability/inventory/quest 等具体玩法写入引擎核心；HUD 直接改 simulation；GameRules 绕过 World/ECS 创建裸对象。

## 8. 性能目标

- Gameplay framework 只调度 active players/worlds，不随全场景实体数量增长。
- Gameplay Event 队列有预算和 backpressure，避免 UI/audio 事件淹没 simulation。
- Headless server world 可以裁剪 UI、render、editor-only subsystem。

## 9. 测试计划

- GameInstance/WorldContext 生命周期、subsystem init/shutdown。
- player join/leave、spawn default avatar、possession/unpossession。
- level travel 成功/失败回滚。
- server/client role gate、prediction 输入转交 Networking。
- HUD view-model snapshot 和 schema mismatch fallback。

## 10. 开放问题 / Spike

- 是否提供内置 lightweight ability/tag/message system，还是保持项目层实现。
- GameRules 是否完全脚本化，或 C++/script 双路径。
- 多 World/PIE/Server travel 的统一事件顺序。
- Gameplay Event 与 Replay/Networking event log 是否共享二进制格式。

## 11. 依赖与被依赖

- **依赖**：App/Engine、World、Authoring Facade、Input、Networking、Scripting、Save/Profile、UI、Camera。
- **被依赖**：项目 gameplay、Samples/Templates、Online sessions、Replay、Editor PIE、Headless server。
