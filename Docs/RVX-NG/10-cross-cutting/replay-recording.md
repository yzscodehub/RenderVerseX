# RVX-NG · Cross-cutting/Replay 录制回放 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §15 Replay 与决策 D11  
**里程碑**：M9-M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供统一的录制、回放、调试和确定性验证基础设施，服务 gameplay replay、网络调试、性能复现、bug report 与自动化测试。

**范围内**：输入日志、状态快照、事件流、state hash、回放控制、时间轴 seek、网络 packet capture、determinism gate。  
**范围外**：视频录制、用户生成内容分享平台、观战 UI。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 可复现 | 同版本、同初始状态、同输入流应复现 deterministic domain |
| 可诊断 | divergence 输出系统级 hash 与最近事件 |
| 可裁剪 | Shipping replay 可关闭或只保留轻量 crash ring |
| 可版本化 | replay 文件记录 build id、schema、asset manifest |
| 安全 | 外部 replay 作为不可信输入解析 |

## 3. 公共接口面（设计契约）

```cpp
class ReplayRecorder {
    void Begin(ReplayDesc desc);
    void RecordInput(FrameIndex frame, InputSnapshot input);
    void RecordEvent(FrameIndex frame, ReplayEvent event);
    void RecordStateHash(FrameIndex frame, Hash128 hash);
    ReplayBlob End();
};

class ReplayPlayer {
    Result<void> Load(ReplayBlob replay);
    void Seek(FrameIndex frame);
    void Step();
    ReplayDivergence LastDivergence() const;
};
```

**不变量**：replay 文件必须包含版本、配置、seed、asset manifest hash；回放不访问 wall-clock；外部 replay 解析走 fuzz 覆盖的安全 reader。

## 4. 数据结构与内存布局

- **Replay Header**：engine version、build id、platform、schema、asset manifest、determinism config。
- **Input Stream**：按 frame 编码的输入、网络命令、随机种子事件。
- **Event Stream**：spawn/despawn、subsystem markers、debug annotations。
- **Snapshot Index**：关键帧状态偏移，用于 seek/rollback。
- **Hash Track**：分系统 state hash，用于 divergence 定位。

## 5. 核心算法

- **录制**：固定 tick 采样输入和命令，帧末写 hash，周期性写关键帧。
- **回放**：加载初始状态/最近关键帧，按记录输入推进 fixed tick。
- **Divergence**：比较分层 hash，定位到 system/entity/component 粒度。
- **Seek**：跳到最近快照，重放到目标帧。
- **压缩**：输入与事件 delta 编码，快照按 chunk 压缩。

## 6. 线程与内存模型

录制写入 per-frame ring，后台 job 压缩落盘。回放控制在测试/游戏线程，资产加载和快照解压异步，但 state apply 在帧安全点。Replay buffer 计入 memory budget。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 版本/schema 不兼容 | 拒绝或走迁移器 |
| asset manifest 不匹配 | 标记不可确定回放，可允许 best-effort preview |
| hash divergence | 停止 strict replay，导出 divergence report |
| replay 文件损坏 | 安全失败，不崩溃 |

**禁止**：回放中读取真实时间或实时输入；replay parser 无界读取；把 replay 成功等同于视频画面一致。

## 8. 性能目标

- 轻量录制可在 Development 长期开启。
- seek 使用快照避免从头重放长 replay。
- divergence report 足够定位系统边界。

## 9. 测试计划

- 输入录制/回放 state hash 一致。
- seek/rollback、损坏文件、schema mismatch。
- 网络 packet capture 与 replay。
- replay parser fuzz。

## 10. 开放问题 / Spike

- 快照粒度：World 全量、component chunk、系统自定义。
- Replay 与 Networking rollback 日志格式是否统一。
- 长时 replay 的资产版本锁定策略。
- 是否支持跨平台 replay strict mode。

## 11. 依赖与被依赖

- **依赖**：Determinism、Serialization、Input、Networking、Asset manifest、Testing。
- **被依赖**：Debug, QA, Networking prediction, Performance reproduction, Crash reports。


