# RVX-NG · Foundation/Reflection 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §5 Reflect、契约 C5、决策 D8
**里程碑**：M0
**状态**：详细设计 · 待 S2 Spike

---

## 1. 目标与范围

**目标**：**一套**类型元数据，驱动序列化、编辑器 inspector、网络复制、组件/系统注册——序列化、编辑器 inspector、网络复制、组件/系统注册共享同一元数据来源。

**范围内**：类型/字段/属性元数据、运行时类型注册表、`Archive`（反射驱动序列化）、codegen 工具链、属性（attribute）系统。
**范围外**：具体序列化格式字节布局（见 Core/Util 序列化）、网络复制策略（见 Networking，消费本系统元数据）。

**决策（D8）**：标准 C++ 当前无静态反射 → **libclang codegen 现阶段**，扫描标注源生成元数据 glue；待 C++26 反射成熟迁移，接口不变。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 单一来源 | 序列化/编辑器/网络/注册共用同一元数据，无第二套 |
| 无侵入运行时 | 元数据在编译期生成，运行时零反射开销（直查表） |
| 字段遍历 | 类型字段名/类型/偏移/属性可枚举 |
| 注册自动化 | 标注类型自动注册到 ClassRegistry（消除手写注册） |
| 属性 | 字段可挂 attribute（`Serialize`/`Replicate`/`EditorHidden`/`Range`…） |
| 版本 | 字段增删可迁移（与 Asset Versioning 协同） |

## 3. 公共接口面（设计契约）

```cpp
// 字段描述
struct FieldInfo { Name name; TypeId type; u32 offset; AttrSet attrs; };

// 类型描述（运行时直查，编译期生成）
struct TypeInfo {
    Name             name;
    TypeId           id;
    TypeId           parent;          // is-a 链
    usize            size, align;
    Span<FieldInfo>  fields;
    void*          (*construct)(IAllocator&);  // 工厂（spawn-by-name）
    void           (*serialize)(void* obj, Archive&);
};

namespace Reflect {
    const TypeInfo* Get(TypeId);
    const TypeInfo* Find(Name typeName);       // spawn-by-name 基础
    bool            IsA(TypeId derived, TypeId base);
    Span<const TypeInfo*> AllOfBase(TypeId base); // 枚举子类（子系统/组件自动发现）
}

// 反射驱动序列化：一份实现服务 存档/Prefab/网络/undo
class Archive {
    bool Saving() const;
    template<class T> void Serialize(T& obj);   // 经 TypeInfo.fields 递归
    void Field(Name, void* data, TypeId);       // 版本容错：缺字段→默认
};
```

**标注语法（codegen 输入）**：
```cpp
RVX_TYPE(MyComponent, Component)          // 注册 + 父链
struct MyComponent {
    RVX_FIELD(Serialize, Replicate) float health;
    RVX_FIELD(EditorRange(0,1))    float ratio;
};
```

**不变量**：`Find(name)` 对所有标注类型非空（生产路径，非仅测试）；`Archive` 版本容错——读到缺失字段用默认、读到未知字段跳过（向前/向后兼容）。

## 4. codegen 工具链与数据布局

- **codegen pass**：构建前 libclang 扫描标注头 → 生成 `*.gen.cpp`（每类型一个 `static TypeInfo` + 静态注册）。纳入构建系统（§10 build-system）依赖图。
- **运行时布局**：`TypeInfo` 表为只读静态数据；`ClassRegistry` = `Name→TypeInfo*` 哈希（启动期由静态注册填充，无运行时扫描）。
- **AttrSet**：紧凑位集 + 参数化属性（Range 等）侧表。

## 5. 核心算法

- **序列化**：`Archive::Serialize<T>` → 查 `TypeInfo` → 遍历 `fields`，按 `offset+type` 递归；带 `Serialize` 属性的字段才存。
- **网络复制**：Networking 取带 `Replicate` 属性字段子集，生成 delta（脏字段位掩码）。
- **编辑器 inspector**：遍历 `fields`，按 type+attrs 生成控件（`EditorHidden` 跳过、`EditorRange` 给滑条）。
- **spawn-by-name**：`Find(name)->construct(alloc)` —— Prefab/脚本实例化基础。

## 6. 线程与内存模型

- 元数据只读、进程级、线程安全无锁读。
- `Archive` 实例非线程安全（每序列化任务一个），可在 worker 并行跑不同对象。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 未标注类型请求反射 | 编译期：codegen 不生成 → 使用点编译错误（强制标注） |
| `Find` 未知名 | 返回 null + 调用方 Result 处理（非崩溃；Prefab 报缺类型） |
| 版本字段缺失/多余 | 缺→默认值，多→跳过（容错） |
| 属性参数非法 | codegen 期校验报错 |

**禁止**：手写 per-type 序列化（绕过反射）；运行时类型扫描（启动期一次性静态注册）。

## 8. 性能目标

- 运行时反射查询 O(1)（哈希/直查），零动态扫描。
- 序列化吞吐受字段遍历主导，可并行（每对象一 job）。
- codegen 增量：仅变更头重生成（构建缓存）。

## 9. 测试计划

- **单元**：字段遍历完整、is-a 链、spawn-by-name、属性解析。
- **往返**：序列化→反序列化逐字段相等（含嵌套/容器/版本增删）。
- **生产注册门禁**：枚举所有 `Component`/`Subsystem` 子类，断言**生产路径**全部可 `Find`（防"仅测试内注册"回归）。
- **版本迁移**：旧版数据读入新 schema 的容错。

## 10. 开放问题 / Spike

- **Spike**：libclang codegen 集成成本与增量构建速度（M0）。
- 标注宏 vs 纯 codegen 推断（少宏更干净但需更强扫描）。
- C++26 静态反射迁移路径（接口冻结点）。
- 模板/泛型类型的反射策略。

## 11. 依赖与被依赖

- **依赖**：Core（Name/TypeId/Result）、构建系统（codegen pass）。
- **被依赖**：数据层（组件/系统注册）、Asset（序列化/版本）、Editor（inspector）、Networking（复制）、Scripting（绑定）、World（Prefab）。**横切多层**，M0 必须稳定。


