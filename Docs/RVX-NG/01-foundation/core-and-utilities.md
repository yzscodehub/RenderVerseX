# RVX-NG · Foundation/Core 与基础工具 详细设计（Tier-1）

**日期**：2026-06-20  
**层级**：Tier-1 · 派生自总纲 §5 Core/Utilities  
**里程碑**：M0  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义 RVX-NG 的基础词汇表与跨模块小工具契约：固定宽类型、`Result`、代际 `Handle`、`Name`、容器、数学、日志、配置/CVar、事件、时间、RNG、Hash、压缩和基础序列化接缝。

**范围内**：Core 类型、错误模型、句柄池、字符串驻留、容器规范、数学手系、确定性数学子集、结构化日志、配置层、事件总线、时钟、随机数、内容 Hash、压缩抽象。  
**范围外**：反射 codegen、完整资产序列化格式、平台线程/RHI/Render 细节。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 基础稳定 | M0 后跨模块公共类型尽量冻结，避免上层接口反复变形 |
| 无异常边界 | 跨模块 API 使用 `Result<T>`/错误码，不让异常穿越模块边界 |
| 句柄安全 | 资源引用使用代际句柄，悬垂访问可检测 |
| 数据导向 | 热路径容器连续、可注入 allocator、避免节点分配 |
| 确定性 | RNG、Hash、DetMath 在 deterministic domain 可重复 |
| 可裁剪 | Log/CVar/assert/debug helpers 可按 build profile 裁剪 |

## 3. 公共接口面（设计契约）

```cpp
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using usize = std::size_t;

struct Error {
    ErrorCode code;
    Name detail;
};

template<class T, class E = Error>
class Result;

template<class Tag>
struct Handle {
    uint32 index = RVX_INVALID_INDEX;
    uint32 generation = 0;
    bool Valid() const;
};

template<class T>
class Span {
public:
    T* data = nullptr;
    usize size = 0;
};

Name MakeName(StringView text);
Hash128 ContentHash(Span<const byte> data);

namespace DetMath {
    Fixed32 FromFloat(float value);
    int64 StableRound(float value);
}
```

**不变量**：public API 不暴露 owning raw pointer；跨模块失败显式返回；`Name` 比较为 O(1) id 比较；`Handle` resolve 必须校验 generation；deterministic domain 不使用全局 RNG 或 unordered iteration 影响结果。

## 4. 数据结构与内存布局

- **Result/Error**：小对象内联，错误码 + `Name` detail，避免异常 unwind 成本。
- **HandlePool**：slot array + generation + freelist，释放后 generation 增加，旧句柄 resolve 为 null。
- **Name Table**：分片 hash table + 字符串 arena，写少读多，debug build 可反查字符串。
- **Containers**：`Array`、`SmallVector`、`PagedArray`、`SparseSet`、`FlatHashMap`、`HandleMap`、`RingBuffer`，均支持 allocator 注入。
- **Math Types**：Vec/Quat/Mat/AABB/Sphere/Frustum，手系、存储顺序、乘法约定显式记录。
- **Log Ring**：per-thread ring + 后台 sink，Fatal 接 crash pipeline。
- **Config Stack**：default/platform/project/user/command-line 分层覆盖。
- **Event Queue**：类型化事件队列，frame safe point drain。

## 5. 核心算法

- **Handle Acquire/Release**：从 freelist 取 slot，写 generation；release 清空对象并 bump generation。
- **Name Interning**：hash 字符串，分片锁查找/插入，返回稳定 id。
- **Flat Hash**：开放寻址 + robin-hood/linear probing，避免节点分配。
- **DetMath**：固定 rounding、禁 fast-math 入口、必要时定点表达 gameplay-critical 数值。
- **Config Resolve**：按层级覆盖并记录来源，CVar 变更触发 callback 和 trace event。
- **Event Dispatch**：emit 写入队列，frame safe point 按稳定顺序派发，listener token 控制生命周期。
- **Content Hash**：对 cook/source/runtime 输入使用稳定字节序，作为 DDC/cache key。

## 6. 线程与内存模型

Core 类型本身无全局可变状态；Name/Log/Config/Event 是少数共享服务。Name table 使用分片锁或 RCU 快照；Log 热路径写 per-thread ring；Config 修改在安全点发布快照；EventBus 不跨线程直接调用 listener，跨线程通过队列转入目标阶段。

容器默认不内建同步，由调用方通过 JobSystem 阶段、command buffer 或外部锁保证并发安全。所有长期容器分配归入 Memory category。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| Result 未检查 | Debug/static analysis 警告；关键边界使用 nodiscard |
| Handle generation mismatch | Resolve 返回 null，Debug 可断言 |
| Name table OOM | 返回 reserved invalid name 并报告 OOM |
| CVar 类型错误 | 拒绝变更，保留旧值 |
| Event listener 失效 | token 自动解绑或 Debug 断言 |
| Hash/serialization endian 不一致 | meta-test 失败 |

**禁止**：跨模块 API 用异常表达常规失败；热路径使用 `std::map`/`std::list`；deterministic domain 使用隐式全局随机或平台相关未固定 rounding。

## 8. 性能目标

- `Name` 比较为整数比较；常用 `MakeName` 命中路径低开销。
- `Handle` resolve O(1)，Sparse/Flat containers cache 友好。
- Log trace 在 disabled/min-level 裁剪时接近零成本。
- Config/Event 不成为帧热路径瓶颈；昂贵诊断仅 Development/Test profile 开启。

## 9. 测试计划

- Result 分支、nodiscard、错误码序列化。
- Handle acquire/release/generation mismatch、池复用。
- Name interning 唯一性、并发插入、debug reverse lookup。
- 容器增删查、迭代稳定性、allocator 注入、fuzz 边界。
- Math 手系、矩阵乘法、Quat、Frustum、DetMath 跨编译器一致性。
- RNG 同种子逐位一致；ContentHash 跨平台稳定；压缩往返。
- Config layer 覆盖、CVar callback、Event 顺序与解绑。

## 10. 开放问题 / Spike

- Math 手系、矩阵存储、shader 端约定是否需要额外可视化 fixture 固化。
- 通用 `FlatHashMap` 哈希函数与 DoS/安全输入边界。
- `Name` 是否支持运行时卸载字符串，还是进程生命周期驻留。
- `DetMath` 定点覆盖范围与 Cross-cutting/Determinism 对齐。

## 11. 依赖与被依赖

- **依赖**：Memory allocator、Platform time/source、Build profiles。
- **被依赖**：全引擎所有模块。Core 与基础工具是公共语言层，M0 之后应保持最大程度向后稳定。
