# RVX-NG · Foundation/Input 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §5 Input
**里程碑**：M0
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：设备无关的输入抽象——把原始设备事件映射为**逻辑 action/axis**，支持上下文栈、重绑定、录制（供 replay）。

**范围内**：设备抽象（键鼠/手柄/触摸）、action/axis 映射、输入上下文栈、死区/曲线、重绑定持久化、输入录制/回放、（预留）haptics。
**范围外**：UI 焦点路由（UI 子系统消费本系统）、相机控制逻辑（gameplay）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 设备无关 | gameplay 只见逻辑 action/axis，不见物理键 |
| 上下文 | 栈式（菜单/游戏/对话），上层可吞噬/穿透 |
| 确定性 | 输入可录制为帧序列，回放逐位重现（供 replay D11） |
| 低延迟 | 主线程采集（C9），最小化采样→消费延迟 |
| 重绑定 | 运行时改键 + 持久化 |

## 3. 公共接口面（设计契约）

```cpp
enum class Action : u16;   // 逻辑动作（Jump/Fire/…），由配置/反射定义
enum class Axis   : u16;   // 逻辑轴（MoveX/LookY/…）

struct InputState {
    bool  Pressed(Action) const;  bool Down(Action) const;  bool Released(Action) const;
    f32   GetAxis(Axis) const;     // 已应用死区/曲线
};

class IInput {
    const InputState& State(ContextId) const;   // 当前帧快照
    ContextId PushContext(Name);  void PopContext(ContextId); // 栈式
    void Bind(Action, DeviceInput);  void BindAxis(Axis, DeviceAxis, Curve);
    void SaveBindings()/LoadBindings();
    // 录制/回放（replay）
    void BeginRecord(Sink);  void BeginPlayback(Source);
};
```

**不变量**：gameplay 只读 `InputState`（不碰原始事件）；上下文栈决定哪层收到 action；录制模式下 `InputState` 完全由录制源驱动（设备被旁路）。

## 4. 数据结构

- **设备层**：每设备一个原始状态缓冲（按钮位图 + 轴浮点 + 时间戳）。
- **映射表**：`DeviceInput→Action`、`DeviceAxis→(Axis,Curve)`，按上下文分层。
- **上下文栈**：`Array<Context>`，每 Context 持映射子集 + 穿透策略。
- **录制流**：每帧 `{frameIndex, actionBits, axisValues}`，紧凑二进制（与 replay 系统对接）。

## 5. 核心算法

- **帧采集**（主线程）：poll 设备 → 更新原始状态 → 按上下文栈自顶向下解析为各层 `InputState`（上层吞噬则下层不见）。
- **死区/曲线**：轴值 → 死区裁剪 → 响应曲线（线性/指数）→ 归一。
- **回放**：跳过设备 poll，直接用录制帧填 `InputState`（确定性）。

## 6. 线程与内存模型

- 采集在**主线程**（C9：OS 输入事件）；消费在 sim 阶段（读快照，无竞争）。
- 帧快照三缓冲，sim 读上一帧采集结果。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 设备热插拔 | 动态增删设备槽；绑定按设备类保留 |
| 绑定冲突 | 后绑定覆盖 + WARN；UI 可提示 |
| 回放流损坏/版本不符 | 拒绝回放 + 报错（非崩溃） |

## 8. 性能目标

- 每帧采集 + 映射 < 0.1 ms（典型设备数）。
- 输入→消费延迟 ≤ 1 帧（低延迟模式下测量并暴露给 Profiler）。

## 9. 测试计划

- 单元：映射解析、上下文栈吞噬/穿透、死区/曲线数值、重绑定持久化往返。
- 确定性：同一录制流多次回放 `InputState` 逐位一致。
- 热插拔/冲突边界。

## 10. 开放问题 / Spike

- haptics/force-feedback 抽象（后置）。
- 触摸手势识别层（移动）归属（Input vs UI）。
- 多本地玩家（split-screen）的设备↔玩家分配。

## 11. 依赖与被依赖

- **依赖**：Platform（原始设备事件）、Core/Config（绑定持久化）、Event。
- **被依赖**：数据层（Input TickGroup）、UI、Gameplay、Replay。

