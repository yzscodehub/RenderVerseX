# RVX-NG · Feature/Physics 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §12 Physics、决策 D11
**里程碑**：M9
**状态**：详细设计 · 待 S9 Spike
**地位**：以 ECS System 接入；确定性子域（D11）；物理模拟必须是真实运行时能力。

---

## 1. 目标与范围

**目标**：Jolt Physics 真接线——固定步长确定性模拟，刚体/角色/载具/ragdoll/破坏/布料，与 ECS 双向同步，render 端插值。

**范围内**：物理世界生命周期、刚体/碰撞体/角色控制器注册、固定步、确定性、ECS↔物理同步、碰撞查询、CCD/约束/碰撞掩码、render 插值。
**范围外**：空间查询索引（World/Spatial 复用）、渲染。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 接线 | 刚体**真注册**进物理世界 + step **真执行** |
| 确定性 | 固定步长 + 确定性子域调度（D11），回放/回滚可复现 |
| 同步 | ECS Transform ↔ 物理 body 双向，固定步周围 |
| 插值 | 变步长 render 对固定步物理插值（平滑） |
| 全特性 | CCD、约束、碰撞掩码、ragdoll、破坏、布料 |

## 3. 公共接口面（设计契约）

```cpp
struct RigidBody {                    // ECS 组件
    BodyId      jolt;                 // 物理世界句柄
    BodyType    type;                 // Static/Kinematic/Dynamic
    ShapeRef    shape; float mass; u32 collisionMask;
};

class PhysicsSystem {                 // ECS System（WorldSubsystem）
    void Register(Entity, const RigidBody&);   // 注册进物理世界
    void Unregister(Entity);
    void StepFixed(f32 dt);                     // 固定步，确定性子域
    void SyncToEcs();  void SyncFromEcs();      // 双向，固定步周围
    bool Raycast(const Ray&, HitResult&) const;
    Span<Entity> Overlap(const Shape&) const;
};
```

**不变量**：刚体真注册 + step 真执行（非注释）；模拟在确定性子域（固定步 + 有序）；render transform = 上两物理态插值；碰撞掩码/CCD/约束生效。

## 4. 数据结构与组织

- **物理世界**：Jolt `PhysicsSystem` 封装（pImpl，不漏 Jolt 类型到公共头）。
- **body↔entity 映射**：BodyId↔Entity 双向表。
- **插值缓冲**：每 body 上两固定步的 transform（render 插值用）。
- **碰撞层/掩码**：层矩阵配置。

## 5. 核心算法

- **注册**：实体加 `RigidBody` → 创建 Jolt body + 加入世界 + 映射。
- **固定步**：累加器；每固定 dt：`SyncFromEcs`（kinematic/外力）→ `Jolt.Update(dt)`（确定性子域）→ `SyncToEcs`（dynamic transform）。
- **render 插值**：变步 render 帧用 `lerp(prevStep, curStep, alpha)` 写 render transform（不改物理态）。
- **确定性**：固定步 + Jolt 确定性配置 + 确定性子域调度（D11）。

## 6. 线程与内存模型

- 固定步在 sim 阶段（Physics TickGroup）；Jolt 内部 jobified（对接 JobSystem 而非自起线程）。
- 确定性子域：有序/固定调度（D11）。
- pImpl 隔离 Jolt；body 数据池化。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| body 创建失败 | Result 错误 + 实体无物理（不崩） |
| 形状/质量非法 | 校验 + 默认/拒绝 |
| 确定性破坏（变步喂物理） | 禁止：物理只吃固定步 |
| 大穿透/爆炸 | CCD + clamp；监控 |

**禁止**：物理 step 只是空壳；刚体不进入物理世界；变步长喂物理（破坏确定性）。

## 8. 性能目标

- 固定步在 sim 预算内；Jolt jobified 多核扩展。
- 插值零物理开销（仅 lerp）。
- 大量刚体（数千）固定步 < 预算。

## 9. 测试计划

- **接线门禁**：刚体注册后真参与模拟（落体/碰撞断言）。
- **确定性回归**：同种子 + 固定步，多次/多线程数逐位一致。
- **插值**：render transform 平滑、无抖动。
- **特性**：CCD/约束/碰撞掩码/ragdoll/布料各单测。

## 10. 开放问题 / Spike

- Jolt 确定性配置跨平台（与 D11 浮点约束协调）。
- 破坏（fracture）的网格/资产管线（与 Asset 协调）。
- 布料/软体与渲染 skinning 的对接。
- 大世界物理流式（cell 物理 body 加载/卸载）。

## 11. 依赖与被依赖

- **依赖**：数据层（RigidBody 组件/System）、JobSystem（Jolt jobified + 确定性子域）、World/Spatial、Foundation/Math（确定性数学）。
- **被依赖**：Gameplay、Animation（ragdoll）、Audio（物理遮挡）、AI（导航障碍）。


