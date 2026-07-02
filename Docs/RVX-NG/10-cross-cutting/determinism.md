# RVX-NG · Cross-cutting/确定性 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自决策 D11  
**里程碑**：M0-M11 贯穿  
**状态**：详细设计 · 待 S9 Spike

---

## 1. 目标与范围

**目标**：定义 RenderVerseX 的确定性边界：sim/physics/network prediction/replay 所需子域逐位或强一致，渲染、音频、工具等非确定性域不进入 lockstep 要求。

**范围内**：固定 tick、确定性 JobSystem 模式、RNG、数学子集、排序/规约、序列化、replay/rollback、测试门禁。  
**范围外**：跨 GPU 渲染逐像素一致、音频混音逐位一致。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 边界清晰 | 明确哪些系统在 deterministic domain |
| 输入固定 | 同初始状态 + 同输入流 = 同输出状态 |
| 调度固定 | 子域内迭代顺序、切片、规约顺序固定 |
| 数学固定 | 禁 fast-math/FMA 重排，必要时定点 |
| 可回放 | 状态 hash、输入日志、版本/schema 记录 |

## 3. 公共接口面（设计契约）

```cpp
class DeterminismScope {
    DeterminismScope(World& world, DeterminismConfig config);
    ~DeterminismScope();
};

class ReplayRecorder {
    void RecordInput(FrameIndex frame, InputSnapshot input);
    void RecordStateHash(FrameIndex frame, Hash128 hash);
};
```

**不变量**：确定性子域禁止未排序容器迭代影响结果；并行规约必须固定树形顺序；所有随机数来自命名 seed stream；状态 hash 覆盖 gameplay-relevant 状态。

## 4. 数据结构与内存布局

- **Input Log**：frame index、input snapshot、network command、seed。
- **State Hash**：系统级 hash、entity/component hash、physics hash。
- **Seed Streams**：系统/实体/事件分流，避免全局 RNG 顺序耦合。
- **Rollback Ring**：关键帧状态快照与增量日志。

## 5. 核心算法

- **固定 Tick**：deterministic domain 只在 fixed dt 更新。
- **稳定排序**：entity/archetype/chunk/system 顺序固定，ID 稳定。
- **固定规约**：sum/min/max 等并行结果用固定 tree reduce。
- **RNG**：counter-based 或 splitmix/pcg stream，不依赖执行顺序。
- **Replay**：加载初始状态 → 喂输入日志 → 每 N 帧比较 state hash。
- **Rollback**：保存快照，收到校正输入后恢复并重放。

## 6. 线程与内存模型

确定性 scope 调用 JobSystem deterministic mode；可并行但切片固定。浮点环境在 scope 进入时配置并在测试中验证。跨线程 command buffer 按稳定顺序合并。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| hash mismatch | 停止回放并导出 divergence report |
| 非确定 API 调用 | Debug 断言，Release 记录严重错误 |
| 浮点环境不支持 | 降级单线程/定点或标记平台不支持该模式 |
| rollback 快照不足 | 回退失败，触发重同步 |

**禁止**：确定性域使用 wall-clock、全局随机、unordered iteration、先到先合规约。

## 8. 性能目标

- deterministic mode 可比普通模式慢，但成本可测且局限于子域。
- replay hash 计算分层可配置，开发构建高覆盖，Shipping 轻量。

## 9. 测试计划

- workerCount 1/2/8/16 多次运行 state hash 一致。
- RNG stream 独立性测试。
- rollback/replay divergence 注入。
- 跨编译器/平台数学子集验证。

## 10. 开放问题 / Spike

- 物理中间件确定性能力边界。
- fixed-point 覆盖哪些 gameplay/math 类型。
- rollback snapshot 粒度与内存成本。
- 网络预测与本地 replay 共享日志格式。

## 11. 依赖与被依赖

- **依赖**：JobSystem deterministic mode、ECS stable iteration、Math/RNG、Serialization。
- **被依赖**：Physics、Networking prediction、Replay、Testing。


