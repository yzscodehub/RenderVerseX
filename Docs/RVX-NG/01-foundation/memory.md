# RVX-NG · Foundation/Memory 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §5 Memory、契约 C3、决策 D10
**里程碑**：M0
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供分层分配器，使热路径**零通用 malloc**、内存**按类别预算可控**、超额可干净处理（D10/C3）。

**范围内**：帧竞技场、类型对象池、通用堆（TLSF）、线程本地分配、分类预算与追踪、OOM 策略、分配器注入。
**范围外**：GPU 显存（RHI residency §7/03-rhi）、虚拟内存映射资产（VFS/Asset）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 竞技场分配 | bump 指针，O(1)，无锁（线程本地）；每帧 reset O(1) |
| 池分配 | O(1) freelist；同类型连续 slab，cache 友好 |
| 通用堆 | TLSF：O(1) alloc/free，最坏碎片有界 |
| 对齐 | 全分配器支持自定义对齐（SIMD/cache line/GPU 上传） |
| 预算 | 每类别（纹理/网格/音频/脚本/…）硬上限 + 实时用量 |
| OOM | 超预算/超物理 → 可恢复策略（驱逐/降级/拒绝），非崩溃 |
| 可注入 | 容器/子系统接受 allocator 参数，便于测试与分域 |

## 3. 公共接口面（设计契约）

```cpp
// 所有分配器实现统一接口；可注入容器/子系统
struct IAllocator {
    void* Alloc(usize size, usize align, MemCategory cat);
    void  Free(void* p);                 // 竞技场可为 no-op
    usize Capacity() const;
};

// 帧竞技场：bump + 整体 reset；线程本地实例
class FrameArena : IAllocator {
    void  Reset();                       // 每帧边界调用，O(1)
    Marker Mark(); void Rewind(Marker);  // 作用域内嵌回退
};

// 类型对象池：固定 T 大小，slab + freelist
template<class T> class PoolAllocator {
    T*   Acquire();  void Release(T*);    // O(1)
    // 池满 → 链新 slab（可配上限）
};

// 通用堆：TLSF；进程级，线程安全
class TlsfHeap : IAllocator { /* alloc/free/coalesce */ };

// 预算与追踪（贯穿所有分配器，按 MemCategory）
namespace MemBudget {
    void  SetLimit(MemCategory, usize bytes);
    usize Used(MemCategory);  usize Limit(MemCategory);
    bool  WouldExceed(MemCategory, usize bytes);
    void  OnOverBudget(MemCategory, OverBudgetPolicy); // Evict/Degrade/Reject
}
```

**不变量**：竞技场分配的指针在 `Reset`/`Rewind` 后失效（调用方不得跨帧持有）；池 `Acquire/Release` 必须配对；任何 `Alloc` 计入对应 `MemCategory`，超预算触发 `OnOverBudget` 而非静默。

## 4. 数据结构与内存布局

- **FrameArena**：大块 backing（VirtualAlloc 提交按需），`head` bump 指针；`Marker` = 偏移快照。线程本地，无锁。
- **PoolAllocator<T>**：slab 链表，每 slab N×sizeof(T) 连续；空闲槽侵入式 freelist（复用对象内存存 next）。
- **TlsfHeap**：两级 segregated free list（size class 位图 O(1) 定位）；块头含 size/prev-phys/free 标志，free 时与物理相邻块 coalesce。
- **预算计数**：每 `MemCategory` 一个原子 `used`；分配器 alloc/free 时原子增减。

## 5. 核心算法

- **Arena alloc**：`p = align(head); head = p + size;` 越界 → 提交下一段或转 OOM 策略。`Reset`：`head = base`。
- **Pool acquire**：`if freelist: pop; else: new slab`。`Release`：push freelist。
- **TLSF alloc**：按 size class 查位图找首个可用块 → split → 标记 used。**free**：标记 free → coalesce 相邻 → 归并入 free list。
- **OnOverBudget**：`Evict`（通知该类别 LRU 驱逐，如纹理流式）→ 重试；失败则 `Degrade`（降分辨率/质量档）→ 重试；再失败 `Reject`（返回 null + WARN，调用方处理）。

## 6. 线程与内存模型

- **竞技场**：每 worker/线程一个实例（线程本地），无同步。
- **池**：默认线程本地；跨线程池用无锁 freelist（CAS）或分片。
- **TLSF 堆**：进程级，细粒度锁或分片堆（按 size class/线程）降争用。
- **大页/提交策略**：reserve 大虚拟区间、按需 commit，降物理占用与 TLB 压力。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 竞技场越界 | 提交下一段；段上限到 → OOM 策略 |
| 池满且达上限 | 转 TLSF 兜底或返回 null（按配置） |
| 超类别预算 | `OnOverBudget`：Evict→Degrade→Reject 链 |
| 物理 OOM | 触发全局内存压力事件 → 各子系统主动释放 → 安全模式 |
| 泄漏 | Debug 构建：分配点追踪 + 关闭时未释放报告（按 category） |
| 跨帧持有竞技场指针 | Debug 毒化（reset 填 0xDD）+ 断言 |

## 8. 性能目标

- 竞技场 alloc < 5 ns；reset O(1)。
- 池 acquire/release < 15 ns。
- TLSF alloc/free < 100 ns（典型）；碎片率有界（TLSF 最坏 ~12.5%）。
- 预算计数原子增减不成为热路径瓶颈（每帧分配数 × 原子 < 帧时预算的 0.1%）。

## 9. 测试计划

- **单元**：对齐正确性、Mark/Rewind 嵌套、池 slab 增长、TLSF split/coalesce 正确性、预算计数准确。
- **压力**：随机 alloc/free 序列碎片率、池高频 churn、多线程争用（TSan）。
- **OOM 路径**：注入预算耗尽，验证 Evict/Degrade/Reject 链与无崩溃。
- **泄漏门禁**：关闭时 category 用量归零（CI 断言）。
- ASan/poison 验证悬垂检测。

## 10. 开放问题 / Spike

- 跨线程池：无锁 freelist vs 分片，实测争用下选型。
- TLSF vs mimalloc/rpmalloc 作为通用堆后端（自研 vs 集成）——M0 spike。
- 预算类别粒度（多细才有用而不繁）。
- 与 RHI 显存预算的统一抽象（CPU/GPU 预算同一 API？）——跨 RHI 协调。

## 11. 依赖与被依赖

- **依赖**：Foundation/Platform（虚拟内存 reserve/commit、大页）、Core（断言/类型）。
- **被依赖**：JobSystem（per-worker 池）、几乎所有子系统（竞技场/池/容器分配器）。M0 与 JobSystem 并列地基。

