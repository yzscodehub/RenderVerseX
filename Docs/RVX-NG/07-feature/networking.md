# RVX-NG · Feature/Networking 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §12 Networking、决策 D11  
**里程碑**：M9  
**状态**：详细设计 · 待 S9 Spike

---

## 1. 目标与范围

**目标**：提供服务器权威、可回放、可预测、可扩展的网络框架：传输抽象、会话、复制、快照、插值、预测/回滚、relevancy、延迟补偿、安全与限流。

**范围内**：可靠/不可靠传输通道、连接握手、序列与 ack、复制模型、快照压缩、兴趣管理、客户端预测、服务器回溯、反作弊接缝。  
**范围外**：具体后端匹配服务、账号平台 SDK 细节、游戏玩法协议内容。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 权威性 | 默认服务器权威，客户端输入预测但不可信 |
| 带宽 | 按 relevancy、优先级、delta 压缩控制带宽 |
| 可回放 | 网络状态可录制、回放、调试 |
| 确定性 | 回滚/预测子域使用固定 tick 与确定性数据 |
| 安全 | 验证输入、速率限制、重放保护、包大小上限 |

## 3. 公共接口面（设计契约）

```cpp
class NetDriver {
    ConnectionId Connect(NetEndpoint endpoint);
    void Listen(NetEndpoint endpoint);
    void Send(ConnectionId, NetChannel channel, Span<const byte> payload);
    void Tick(NetTick tick);
};

class ReplicationSystem {
    void RegisterReplicatedType(TypeId type, ReplicationDesc desc);
    void BuildSnapshot(ConnectionId client, NetSnapshot& outSnapshot);
    void ApplySnapshot(const NetSnapshot& snapshot);
};

class PredictionSystem {
    void SubmitInput(InputFrame input);
    void Reconcile(ServerState state);
};
```

**不变量**：复制只通过反射/声明 schema；服务器状态优先于客户端预测；网络包解析必须有大小与版本校验；relevancy 决定发送集合，不允许全世界无条件复制。

## 4. 数据结构与内存布局

- **Connection**：序列号、ack window、rtt、丢包率、加密/认证状态。
- **NetChannel**：reliable ordered、unreliable sequenced、state sync 等通道。
- **Replication Schema**：字段 id、quantization、condition、priority。
- **Snapshot Ring**：服务器历史状态，用于 delta、回溯、lag compensation。
- **Interest Grid**：按空间/团队/可见性/订阅关系筛选复制对象。

## 5. 核心算法

- **传输**：UDP/平台 socket 上构建 packet framing、ack、重传、拥塞预算。
- **复制**：按 schema 量化字段，基于上次 ack snapshot 做 delta。
- **兴趣管理**：每客户端计算 relevant set，按带宽预算排序发送。
- **预测/回滚**：客户端本地预测输入；收到服务器状态后 rewind/replay。
- **Lag compensation**：服务器保存历史碰撞/命中数据，按客户端命令时间回溯验证。
- **安全**：输入白名单、速率限制、包签名/加密接缝、异常 telemetry。

## 6. 线程与内存模型

网络 IO 可在专用 socket 线程或平台回调中收包，但解析和状态应用在 NetTick 阶段进入 JobSystem。跨线程队列只传 ownership 明确的 packet buffer。复制构建可按 connection 并行。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| schema/version 不匹配 | 拒绝连接或走兼容迁移 |
| 包损坏/越界 | 丢弃并计数，严重时断开 |
| 带宽超预算 | 降低低优先级复制频率 |
| 预测偏差过大 | 强制校正并上报 debug marker |

**禁止**：信任客户端权威状态；无上限反序列化；复制裸指针/不稳定运行时地址。

## 8. 性能目标

- 每客户端复制构建在带宽/CPU 预算内稳定。
- 高实体数下 relevancy 计算可分片并行。
- snapshot history 内存受 tick rate、玩家数、回溯窗口预算控制。

## 9. 测试计划

- packet fuzz、schema 兼容、delta 往返、丢包/乱序/延迟模拟。
- prediction/reconciliation golden。
- lag compensation 命中验证。
- 带宽预算与 relevancy 压力测试。

## 10. 开放问题 / Spike

- 传输层自研 vs ENet/Steam Networking Sockets/平台方案。
- lockstep、rollback、server-authoritative 三种玩法模型的支持边界。
- 反作弊信号与隐私/平台要求。
- 大规模 session 的 replication graph 分区策略。

## 11. 依赖与被依赖

- **依赖**：Reflection、ECS、Determinism、Serialization、Platform socket、Security hooks。
- **被依赖**：Gameplay、Replay、AI、Physics prediction、Telemetry。


