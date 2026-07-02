# RVX-NG · Cross-cutting/帧时序与同步 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自决策 D12 与契约 C9  
**里程碑**：M0-M11 贯穿  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义全引擎帧时序、CPU/GPU 同步、固定步模拟、渲染流水线深度、低延迟模式、present 策略与 fence 语义，使吞吐、延迟、确定性和平台要求可配置且可测。

**范围内**：main loop、fixed sim、variable render、Extract 同步点、triple buffering、GPU timeline、present、低延迟模式、帧 pacing。  
**范围外**：具体渲染 pass 内容、平台 swapchain 细节实现。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 固定模拟 | sim/physics 使用固定 dt，可多步追赶，有上限 |
| 流水线可配 | 默认吞吐 2-3 帧，低延迟模式减少重叠 |
| 无隐式等待 | CPU/GPU sync 必须经 timeline/fence 显式表达 |
| 可观测 | 每帧 sim/render/GPU/present 延迟可追踪 |
| 平台适配 | present 线程亲和和 frame pacing 可按平台策略配置 |

## 3. 公共接口面（设计契约）

```cpp
struct FrameTimingConfig {
    float fixedDt;
    u32 maxSimStepsPerFrame;
    u32 pipelineDepth;
    bool lowLatencyMode;
    PresentMode presentMode;
};

class FrameScheduler {
    void Tick(float realDt);
    FrameIndex BeginFrame();
    void SubmitRender(FrameIndex frame);
    void Present(FrameIndex frame);
};
```

**不变量**：Render 消费完成的 Extract 快照；GPU 不访问仍在 CPU 写入的 per-frame data；低延迟模式可牺牲吞吐但不破坏模拟固定步；任何等待都进入 profiler trace。

## 4. 数据结构与内存布局

- **Frame Ring**：per-frame allocator、upload ring、descriptor transient、query set。
- **Timeline State**：graphics/compute/copy queue fence values。
- **Snapshot Ring**：sim/write、render/read、GPU/in-flight 索引。
- **Pacing State**：目标帧率、VRR/vsync、present history、input sample time。

## 5. 核心算法

- **主循环**：poll input → 累加 fixed dt → run sim steps → Extract → render previous snapshot → submit → present。
- **流水线深度**：frame resources 只有在 GPU fence 完成后复用；超过 depth 时 CPU throttle。
- **低延迟模式**：更晚采样输入、减少 CPU 预提交帧数、可禁用部分 async overlap。
- **Frame pacing**：根据 present mode 与历史 frame time 调节 sleep/spin。
- **Fence 统一**：RHI timeline 与 JobSystem completion 通过明确 bridge 传递，不混用隐式 blocking。

## 6. 线程与内存模型

sim/render 是 task graph 逻辑阶段，不绑定固定 OS 线程。OS 消息泵与部分 present 需要主线程亲和。GPU submit 可在 render 阶段末聚合，资源释放延迟到对应 fence 完成。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| sim 追赶过多 | clamp steps，进入 slow-frame 状态 |
| GPU 落后超过 depth | throttle CPU，记录 GPU-bound |
| fence 超时/device lost | 进入 RHI device removed 流程 |
| present 模式不支持 | 降级到平台默认并记录 |

**禁止**：CPU 写入 in-flight frame resource；不可见的 blocking wait；变步喂 physics。

## 8. 性能目标

- frame latency 可拆分为 input→sim→render→GPU→present。
- 低延迟模式下 queued frames 明显减少。
- GPU-bound/CPU-bound/present-bound 可自动归因。

## 9. 测试计划

- fixed dt 多步/clamp、pipeline depth resource reuse、fence 完成释放。
- 低延迟模式 queued frame 计数。
- present mode fallback。
- frame pacing jitter 统计。

## 10. 开放问题 / Spike

- Reflex/Anti-Lag/平台低延迟 API 抽象。
- VRR/vsync/offline capture 三套 pacing 策略。
- GPU fence 转 JobSystem Counter 的 API 形状。
- 多 viewport/editor 下 frame scheduler 共享方式。

## 11. 依赖与被依赖

- **依赖**：JobSystem、RHI timeline、Input、Time、Profiler。
- **被依赖**：Engine main loop、Render、Physics、Networking prediction、Editor viewports。


