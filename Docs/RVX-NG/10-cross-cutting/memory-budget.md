# RVX-NG · Cross-cutting/内存与预算 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自契约 C3、决策 D10  
**里程碑**：M0-M11 贯穿  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：统一 CPU 内存、GPU 显存、IO/streaming、transient frame memory、asset cache 的预算模型，确保各平台可预测运行、OOM 可降级、内存峰值可追踪。

**范围内**：分类预算、allocator category、VRAM residency、streaming budget、frame arena、pool、cache eviction、OOM 策略、memory telemetry。  
**范围外**：具体 allocator 实现细节（Foundation/Memory）、RHI 原生堆 API 细节。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 分类 | CPU/GPU/Asset/Streaming/Render/Audio/Physics 等分类追踪 |
| 预算 | 每平台 profile 明确 hard/soft budget |
| OOM | 先降级/驱逐/缩质，最后失败，不允许静默崩溃 |
| 可观测 | peak、live、fragmentation、eviction 原因可追踪 |
| 热路径 | 热路径零堆分配或可证明池化 |

## 3. 公共接口面（设计契约）

```cpp
struct MemoryBudget {
    Bytes softLimit;
    Bytes hardLimit;
    MemoryPriority priority;
};

class BudgetManager {
    void RegisterCategory(Name category, MemoryBudget budget);
    bool TryReserve(Name category, Bytes bytes);
    void Release(Name category, Bytes bytes);
    void OnPressure(MemoryPressure pressure);
};
```

**不变量**：所有长期内存归入 category；超 soft limit 触发 pressure，超 hard limit 前必须尝试驱逐/降级；显存 residency 与 Asset streaming 共用预算状态。

## 4. 数据结构与内存布局

- **Budget Table**：category、soft/hard、current、peak、priority。
- **Allocation Tags**：allocator id、callsite/token、size、lifetime。
- **Eviction Candidates**：asset cache、VT pages、geometry clusters、PSO/temp cache。
- **Pressure State**：normal、warning、critical、emergency。

## 5. 核心算法

- **Reserve/Commit**：大分配先 reserve 预算，再实际分配，失败可提前降级。
- **Pressure Propagation**：BudgetManager 广播 pressure event，高内存模块注册 handler。
- **Eviction**：按 priority、last used、reload cost、quality impact 排序驱逐。
- **Transient Sizing**：frame arena 与 transient GPU heap 根据历史 peak 调整。
- **Telemetry**：帧末采样 live/peak/eviction/OOM 事件。

## 6. 线程与内存模型

预算计数用原子或分片计数，帧末合并。allocator hot path 只做轻量 tag/计数；昂贵 stack trace 仅 debug/profile 构建启用。eviction 在安全点执行。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| soft limit 超过 | 发 pressure，降低 streaming/quality |
| hard limit 接近 | 强制驱逐低优先缓存 |
| 分配失败 | 返回 Result/OOM，不允许裸崩 |
| 预算泄漏 | profiler 标记 category/callsite |

**禁止**：未分类长期分配；OOM 后继续假成功；Streaming 与 RHI residency 各自为政。

## 8. 性能目标

- budget tracking 在 hot path 开销极低。
- pressure 到可见降内存行为延迟不超过数帧。
- memory profiler 能定位大头 category 与趋势。

## 9. 测试计划

- category reserve/release、soft/hard pressure、eviction order。
- OOM 注入：asset、render transient、VRAM residency。
- leak/peak tracking。
- 平台 profile 预算一致性验证。

## 10. 开放问题 / Spike

- 预算配置粒度：平台、质量档、场景类型。
- 显存 residency 与 OS 报告预算差异。
- fragmentation 指标如何跨 allocator 统一。
- 自动质量调节与手动 scalability 的优先级。

## 11. 依赖与被依赖

- **依赖**：Foundation Memory、RHI residency、Asset streaming、Profiler。
- **被依赖**：Render、Asset、World streaming、Audio、Physics、Shipping profiles。


