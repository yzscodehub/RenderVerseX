# RVX-NG · Feature/Audio 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §12 Audio  
**里程碑**：M9  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供可流式、可混音、可空间化、可调试的游戏音频系统：事件驱动播放、3D 空间化、遮挡/阻尼、DSP、bus/mixer、streaming、voice budget、平台后端抽象。

**范围内**：音频事件、voice 管理、解码与流式、混音图、bus、DSP、3D 空间化、物理遮挡、快照/状态切换、字幕/无障碍接缝。  
**范围外**：具体作曲/内容创作工具、第三方音频中间件编辑器。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 实时性 | audio render thread 不分配、不阻塞、不等待磁盘 |
| 流式 | 长音频分块解码，短音效常驻或按包加载 |
| 预算 | voice 数、解码、DSP、内存按平台预算 |
| 空间化 | 支持 HRTF/声像、距离衰减、遮挡、reverb send |
| 可替换 | 自研后端和 Wwise/FMOD 等中间件接缝不污染 gameplay 接口 |

## 3. 公共接口面（设计契约）

```cpp
struct AudioEventHandle { u32 id; u32 gen; };
struct VoiceHandle { u32 id; u32 gen; };

class AudioSystem {
    VoiceHandle PostEvent(AudioEventHandle event, Entity emitter);
    void SetParameter(Entity emitter, Name param, float value);
    void Stop(VoiceHandle voice, FadeDesc fade);
    void SetListener(const AudioListener& listener);
};

class AudioMixer {
    void Render(AudioBuffer& output, u32 frameCount);
};
```

**不变量**：gameplay 只发 audio event，不直接操作 mixer voice；audio render thread 不访问 ECS 可变状态；空间化输入来自上一帧稳定快照；所有磁盘 IO 与解码在非实时线程。

## 4. 数据结构与内存布局

- **AudioEvent**：clip/随机容器/参数曲线/bus routing/fade/priority。
- **Voice**：播放游标、gain、pitch、spatial state、DSP chain、priority。
- **Mixer Graph**：bus 层级、send、effect 插槽、side-chain。
- **Streaming Ring**：每 voice 解码块环形缓冲，预读窗口。
- **Acoustic Probe/Zone**：reverb/occlusion/portal 数据。

## 5. 核心算法

- **Voice 分配**：按 priority、audibility、距离、重要性抢占。
- **解码**：后台 job 解码到 ring，audio thread 只消费已准备块。
- **混音**：按 bus graph 拓扑顺序处理，SIMD 混音，DSP 插件预算化。
- **空间化**：HRTF 或声像近似；距离衰减 + 多普勒 + spread。
- **遮挡/混响**：Physics/World 查询低频更新，结果平滑后写入 audio snapshot。
- **状态快照**：listener/emitter transform 每帧生成只读快照供 audio thread 使用。

## 6. 线程与内存模型

audio render thread 是实时线程；不得调用通用 allocator、锁、日志落盘或资产加载。解码、streaming、遮挡查询走 JobSystem。跨线程通信使用无锁 ring/双缓冲快照。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| clip 未就绪 | 播放 fallback/静音并记录一次 WARN |
| streaming underrun | 插值静音/短淡出，计数进入 telemetry |
| voice 超预算 | 按 priority 抢占低价值 voice |
| 后端设备丢失 | 尝试重建设备，失败进入静音安全模式 |

**禁止**：audio thread 阻塞 IO；gameplay 持有后端原生 voice；无限 voice 创建。

## 8. 性能目标

- audio callback p99 小于 buffer deadline 的 50%。
- stream underrun 在正常 IO 预算下为 0。
- voice/bus/DSP 成本可在 profiler 中逐项归因。

## 9. 测试计划

- voice 生命周期、抢占、fade、参数曲线。
- streaming 压力、underrun 注入、设备丢失恢复。
- 3D 空间化与遮挡 golden/数值测试。
- audio thread no-allocation/no-lock 检测。

## 10. 开放问题 / Spike

- 自研 mixer vs Wwise/FMOD 集成的产品边界。
- HRTF 库选择与平台授权。
- 声学 portal/zone 模型复杂度。
- 网络语音与游戏音频 mixer 的集成边界。

## 11. 依赖与被依赖

- **依赖**：Asset streaming、JobSystem、World/Physics 查询、Platform audio backend。
- **被依赖**：Gameplay、Editor audition、Sequencer、Accessibility/subtitles。


