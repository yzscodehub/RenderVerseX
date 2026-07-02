# RVX-NG · Tools/Profiler & Debug 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §13 Profiler/Debug  
**里程碑**：M10  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供全引擎可观测性：CPU/GPU 时间轴、内存/显存预算、FrameDebugger、RenderGraphViz、GpuScene/streaming/debug draw、CVars console、capture/export 与回归基线。

**范围内**：trace event、CPU profiler、GPU timestamp/query、memory profiler、FrameDebugger、RenderGraph 可视化、debug overlays、console/CVars、capture 文件格式。  
**范围外**：外部云遥测后台实现、平台专有 profiler 细节。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 低开销 | Development 默认可开，Shipping 可裁剪 |
| 统一时间轴 | CPU jobs、GPU queues、asset IO、streaming、audio 等对齐 |
| 可导出 | capture 可离线分析、比较、回归 |
| 诚实 | pass 跳过/降级/能力 fallback 必可见 |
| 可集成 | PIX/RenderDoc/Xcode/平台 profiler debug markers |

## 3. 公共接口面（设计契约）

```cpp
class TraceRecorder {
    TraceScope BeginScope(Name category, Name name);
    void Counter(Name name, double value);
    void Event(Name category, Name name, TracePayload payload);
};

class FrameDebugger {
    void CaptureFrame(FrameCaptureOptions options);
    const CapturedFrame& LastCapture() const;
};

class CVarRegistry {
    void Register(CVarDesc desc);
    bool Set(Name name, CVarValue value);
};
```

**不变量**：profiling marker 不改变运行语义；GPU timestamps 与 RenderGraph pass 对齐；capture 不包含未授权敏感数据；所有分级/降级原因能被 trace 或 stats 查询。

## 4. 数据结构与内存布局

- **Trace Buffer**：per-thread lock-free ring，帧末合并。
- **Gpu Timing Buffer**：timestamp query ring，延迟读回。
- **Memory Snapshot**：allocator category、pool、budget、peak、leak marker。
- **Frame Capture**：RenderGraph、resources、pass stats、barrier、pipeline、draw/dispatch。
- **CVar Table**：name、type、default、flags、callback、config layer。

## 5. 核心算法

- **Trace 合并**：按 timestamp 与 thread id 合并 CPU events；GPU query 延迟 N 帧解析。
- **FrameDebugger**：在目标帧启用资源/状态记录，导出 pass DAG 与资源生命周期。
- **RenderGraphViz**：DAG 节点、资源边、barrier、async queue、aliasing 区间可视化。
- **Memory Profiler**：allocator hook 上报 category，帧末生成预算状态。
- **CVars**：分层配置，运行时 callback，变更写 trace。

## 6. 线程与内存模型

trace 写入必须无锁或低锁，使用 per-thread ring。GPU timing readback 延迟处理，不阻塞提交。capture 大对象写入后台 job，避免卡帧；必要时 capture 模式允许单帧暂停。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| trace buffer 满 | 丢弃低优先级事件并计数 |
| GPU timestamp 不支持 | 标记 unavailable，不伪造数据 |
| capture 资源过大 | 按选项裁剪或失败 |
| CVar 非法值 | 拒绝并保留旧值 |

**禁止**：profiler 造成隐式同步；Shipping 泄露敏感 debug console；用静态字符串检查替代真实行为测试。

## 8. 性能目标

- trace scope 开销低到可在热点小心使用。
- GPU pass timing p95 稳定可读，无 CPU/GPU 同步尖峰。
- capture 文件可用于回归比较。

## 9. 测试计划

- trace nesting、多线程合并、GPU timestamp 延迟解析。
- RenderGraphViz DAG/alias/barrier 正确。
- memory category 预算与 leak marker。
- CVar 范围/权限/持久化。

## 10. 开放问题 / Spike

- Trace 文件格式：Chrome trace、Perfetto、自定义二进制。
- Frame capture 的资源快照粒度。
- 平台工具标记 API 抽象。
- 自动性能回归基线存储。

## 11. 依赖与被依赖

- **依赖**：Foundation Time/Log/Memory、RHI Query、RenderGraph、Config。
- **被依赖**：Editor、CI performance gates、Shipping telemetry、Debug workflows。


