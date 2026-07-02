# RVX-NG · 数据层/Authoring 门面与桥 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §6、决策 D1
**里程碑**：M1
**状态**：详细设计 · v1.0 可评审
**地位**：D1 混合模型的 gameplay 一侧——**一等** entity/actor 门面（非"皮肤"），经桥与数据导向内核（`ecs.md`）双向同步。

---

## 1. 目标与范围

**目标**：给 bespoke gameplay 一个**人机友好的对象模型**（retained entity/actor + 组件 + 可虚行为），同时其变换/渲染/物理等"热数据"以**数据导向列**存储，被系统并行处理。两全：工效 + 扩展性。

**范围内**：Entity 门面、Component（数据组件 vs 行为组件）、生命周期（spawn/destroy/BeginPlay）、attach/层级、门面↔ECS 桥（数据组件映射到列）、Prefab 实例化接口。
**范围外**：ECS 存储/调度（`ecs.md`）、具体玩法（游戏层）、序列化（Reflect）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 工效 | gameplay 用对象式 `entity.Get<T>()`/`AddComponent`/虚 `Tick`，不必写系统 |
| 热数据导向 | transform/render/physics 等热组件**仍是 ECS 列**（系统并行处理） |
| 一致性 | 门面视图与 ECS 列**单一真相**——不双存储、不双 transform |
| 生命周期 | `BeginPlay` 显式一次；destroy 延迟帧边界（无 GC） |
| 引用安全 | 门面引用 = `Entity` 代际句柄（悬垂安全） |

## 3. 公共接口面（设计契约）

```cpp
// Entity 门面：薄包装 ECS Entity，提供对象式 API
class EntityRef {
    Entity id;                                   // 底层 ECS 句柄（单一真相）
    template<class C> C*   Get();  template<class C> C& Add(C={}); 
    template<class C> bool Has() const;  template<class C> void Remove();
    Transform& Xform();                          // 直达 ECS Transform 列（非副本）
    void  Attach(EntityRef parent);  EntityRef Parent() const;
    bool  Valid() const;                         // gen 校验
};

// 行为组件：需要 per-instance 逻辑/虚函数的"冷"组件（区别于纯数据热组件）
class BehaviorComponent {            // 注册进 ECS 为"行为列"，但可虚
    virtual void BeginPlay() {}
    virtual void Tick(f32 dt) {}     // 由 Behavior 系统在 ECS 调度内批量驱动
    EntityRef Owner() const;
};

// 门面世界视图（包装 World）
class Scene {
    EntityRef Spawn(Prefab* = null);  void Destroy(EntityRef);
    EntityRef Find(Name) const;       // 命名查找（编辑器/脚本）
    EntityRef Instantiate(Prefab&, const Transform&);  // 经 Reflect 工厂
};
```

**不变量**：`EntityRef`/`Transform` 直读写 **ECS 列**（无影子副本，杜绝"双 transform"）；`BehaviorComponent::Tick` 由内核 Behavior 系统在调度内驱动（不是各自起线程/裸虚调用循环）；destroy 延迟帧边界。

## 4. 数据结构与桥设计

- **门面 = 句柄 + 模板访问**：`EntityRef` 仅持 `Entity`；`Get<C>` 转 `World::Get<C>`。零额外存储。
- **热组件**：纯数据 struct（Transform/RenderProxyRef/RigidBodyRef…），ECS 列存储，系统并行处理。
- **行为组件**：注册为一种特殊列（存指针/小对象 + vtable），由 `BehaviorTickSystem` 遍历该列调用 `Tick`——**对象式工效 + 数据式批量调度**。
- **层级**：复用 ECS `Relation`（attach=parent 关系）；变换传播由 TransformSystem 批量（脏标记），门面只读结果。
- **Prefab**：资产（组件集 + 默认值，经 Reflect 序列化）；`Instantiate` = 批量 `Spawn` + 反射工厂填值。

## 5. 核心算法

- **AddComponent**：转 `World::Add`（结构性变更→CommandBuffer，帧边界回放）。
- **BeginPlay**：spawn 后登记"待 BeginPlay"集；World begin/帧边界统一**显式触发一次**（非隐式惰性）。
- **Behavior tick**：`BehaviorTickSystem`（一个 ECS 系统）按行为列迭代调用虚 `Tick`——并行度受其读写集约束（默认保守串行或按声明并行）。
- **门面查找/命名**：`Find(Name)` 经 Name→Entity 侧表（编辑器/脚本用，非热路径）。

## 6. 线程与内存模型

- 门面访问在 sim 阶段（gameplay 系统内）；热组件系统经 Scheduler 并行。
- 行为 `Tick` 默认在 Behavior 系统的 job 内；若行为间无共享写可并行（按读写集）。
- destroy/add 经 CommandBuffer 帧边界单线程回放（与 ECS 一致）。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 失效 EntityRef | gen 校验 → `Valid()==false`，访问返回 null |
| 重复 BeginPlay | 登记集去重，保证一次 |
| 行为 Tick 抛异常 | 边界禁异常；逃逸→崩溃管线 |
| Prefab 缺组件类型 | Reflect `Find` 失败 → 报缺类型（非静默 null） |

**禁止**：门面持有热数据副本（双存储）；裸虚 `Tick` 循环绕过调度；隐式每帧 BeginPlay。

## 8. 性能目标

- `EntityRef::Get<C>` 与 `World::Get` 同级（O(1) <20 ns）。
- 行为 Tick：N 个行为组件批量驱动，开销 ≈ N × 虚调用（可接受，仅 bespoke 用）；热逻辑应走纯数据系统而非行为组件。
- Instantiate(Prefab) ≈ 批量 spawn，O(组件数)。

## 9. 测试计划

- **单元**：门面增删组件 = ECS 列一致（无影子）、attach/层级传播、BeginPlay 恰一次、EntityRef 代际失效。
- **桥一致性**：经门面改 Transform → 系统读到同值（单一真相断言）。
- **Prefab**：生产路径实例化全组件类型成功（防"工厂零注册"回归，与 Reflect 门禁协同）。

## 10. 开放问题 / Spike

- 行为组件的并行策略（默认串行 vs 声明式并行）——工效/性能权衡。
- 门面 API 的"对象式糖"边界（多少糖不损数据导向纪律）。
- 与脚本（Lua）绑定的统一（脚本组件 = 行为组件特例？）——与 Scripting 协调。

## 11. 依赖与被依赖

- **依赖**：ECS 内核（`ecs.md`）、Reflect（Prefab/组件工厂）、Core。
- **被依赖**：World（场景/Prefab）、Gameplay、Editor（编辑真实门面）、Scripting。


