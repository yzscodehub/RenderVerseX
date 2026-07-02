# RVX-NG · Cross-cutting/Platform Profiles XR Mobile Web 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 RHI、Input、Frame Timing、Shipping  
**里程碑**：M2-M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义平台 profile 体系，覆盖 Desktop、Console、Mobile、XR、Web/Cloud 的能力、预算、输入、帧时序、包体、权限、质量档和降级规则。XR/Mobile/Web 不一定首期完整实现，但必须有一致的架构接缝，避免核心系统假设单窗口、单显示、单输入、无限后台 IO。

**范围内**：platform profile、device class、quality preset、XR stereo 接缝、mobile thermal/battery、web sandbox、console cert readiness、cloud/streaming profile。  
**范围外**：具体平台认证文档、厂商 SDK 私有细节、完整 XR runtime 实现。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 能力诚实 | 每个平台 profile 声明 RHI/input/audio/storage/network capabilities |
| 预算先行 | CPU/GPU/VRAM/RAM/IO/battery/thermal/package size 有预算 |
| 可裁剪 | Build/Shipping 按 profile 裁剪模块、shader、asset、feature |
| XR 预留 | stereo view、late update、tracking input、low latency path 不破坏主线 |
| Mobile/Web 预留 | app lifecycle、suspend/resume、权限、沙箱文件系统、触摸输入 |

## 3. 公共接口面（设计契约）

```cpp
enum class PlatformClass : uint8 {
    Desktop,
    Console,
    Mobile,
    XR,
    Web,
    Cloud
};

struct PlatformProfile {
    PlatformClass platformClass;
    RhiCaps requiredRhiCaps;
    MemoryBudget memory;
    FrameTimingConfig timing;
    InputCaps input;
    PackagePolicy package;
    FeatureMask defaultFeatures;
};

class PlatformProfileRegistry {
public:
    const PlatformProfile& Resolve(PlatformId, DeviceClass, BuildProfile);
    bool Supports(FeatureId feature) const;
};
```

**不变量**：feature enable 必须通过 profile/caps；profile 是 Build/Cook/Runtime/Testing 共用输入；XR/Mobile/Web unsupported 时明确 false，不写半实现 stub。

## 4. 数据结构与内存布局

- **Platform Profile**：device class、required/optional caps、memory budgets、quality presets、package policy。
- **Device Class**：GPU tier、CPU cores、RAM/VRAM、storage speed、thermal class、display modes。
- **Input Profile**：keyboard/mouse/gamepad/touch/gyro/VR controllers/hand tracking。
- **Display Profile**：HDR、VRR、stereo、refresh rates、safe area、DPI。
- **Lifecycle Policy**：startup, suspend, resume, background IO, save-on-suspend。
- **Permission Manifest**：network, storage, microphone, camera, platform services。

## 5. 核心算法

- **Profile Resolve**：platform + device + build profile → feature/capability set。
- **Quality Bootstrap**：选择 initial scalability tier，运行时可因 thermal/budget 降级。
- **Cook Filter**：按 profile 裁剪 texture formats、shader permutations、asset chunks、platform metadata。
- **XR View Build**：stereo camera pair、late-latched transforms、foveated/VRS support、low-latency present。
- **Mobile Lifecycle**：suspend 前 flush save/profile，resume 后验证 RHI/swapchain/online state。
- **Web Sandbox**：异步文件/网络限制、package streaming、thread/SIMD capability gate。

## 6. 线程与内存模型

Platform profile 在启动早期解析并冻结为 RuntimeProfile。Mobile/Web 的 suspend/resume 事件进入 App lifecycle。XR tracking 可在平台线程采样，但发布到 Frame Timing 的 late update 接缝。Profile 切换不直接重建核心系统，需通过 feature/scalability 变更队列。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| required cap 缺失 | 启动失败或禁用 profile |
| optional cap 缺失 | 降级并记录 fallback |
| thermal/battery pressure | 降 quality、分辨率、simulation rate 或后台任务 |
| suspend 中断 | 保留 last good save/profile，resume 走 recovery |
| XR tracking lost | 显示安全 fallback view，不驱动 gameplay teleport |
| web permission denied | feature disabled，UI 显示不可用 |

**禁止**：profile 外启用 feature；移动端后台继续高负载 cook/stream；XR tracking 数据直接绕过 Frame Timing。

## 8. 性能目标

- profile resolve 和 feature gate 启动阶段完成，热路径只查 compact flags。
- Mobile thermal 降级在可观测阈值内触发，避免长时间过热。
- XR path 优先低延迟和稳定帧率，非核心视觉 feature 可自动降级。

## 9. 测试计划

- profile schema、required/optional caps、feature fallback。
- Desktop/Console/Mobile/Web/XR mock profile build/cook closure。
- suspend/resume、permission denied、thermal pressure fault injection。
- XR stereo ViewSet contract、late update timing、tracking lost fallback。
- Shipping package audit 按 profile 裁剪。

## 10. 开放问题 / Spike

- 首期是否只定义 XR 接缝，不实现具体 OpenXR provider。
- Web 是否进入目标平台，还是仅保留 cloud/streaming profile。
- Mobile renderer 是否需要独立 Forward+ profile。
- 平台 profile 与 capability-test-matrix 是否生成同一 truth table。

## 11. 依赖与被依赖

- **依赖**：RHI caps、Input、Frame Timing、Memory Budget、Build/Cook、Shipping、Online Services。
- **被依赖**：Platform-specific builds、Scalability、Testing matrix、Editor device preview、Release qualification。
