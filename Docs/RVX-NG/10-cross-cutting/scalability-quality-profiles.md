# RVX-NG · Cross-cutting/Scalability & Quality Profiles 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 Capability Tiering、Platform Profiles、Memory Budget、Frame Timing  
**里程碑**：M5-M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义跨模块统一质量档和可伸缩策略。Scalability 不只是渲染设置，而是 CPU/GPU/内存/IO/网络/音频/UI/脚本等预算压力下的系统化降级机制。每个 quality tier 必须可配置、可观测、可测试，并与 Platform Profile、Capability Tiering 和 FrameStats 对齐。

**范围内**：quality preset、runtime scalability group、dynamic resolution、feature degrade order、budget pressure、profile override、user settings、benchmark auto-detect。  
**范围外**：具体美术调参、项目自定义画质 UI 文案。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 全局一致 | 各模块使用同一 QualityProfile/ScalabilityGroup，不各自发明档位 |
| 可解释 | 每次自动降级必须记录原因、输入指标和目标预算 |
| 可逆 | 压力解除后可按 hysteresis 逐步恢复，避免抖动 |
| 用户优先 | 用户显式设置、平台限制、运行时自动调节优先级明确 |
| 可测试 | 每个档位有 fixture，fallback 不允许黑屏/静音/崩溃 |

## 3. 公共接口面（设计契约）

```cpp
enum class QualityTier : uint8 {
    Low,
    Medium,
    High,
    Ultra,
    Cinematic,
    Custom
};

enum class ScalabilityGroup : uint16 {
    Resolution,
    ViewDistance,
    Shadows,
    GI,
    Reflections,
    Textures,
    Geometry,
    VFX,
    Animation,
    Audio,
    UI,
    Scripts,
    Networking
};

class ScalabilityManager {
public:
    void ApplyProfile(QualityProfileId profile);
    void SetGroupTier(ScalabilityGroup group, QualityTier tier);
    void ReportPressure(BudgetPressure pressure);
    QualityState Snapshot() const;
};
```

**不变量**：quality 变更通过 ScalabilityManager 发布；模块不得私自读取用户配置后绕过 capability/fallback；Shipping profile 必须知道每个 target 的默认 profile；Research/Advanced 优先降级，Proven 只降质量参数。

## 4. 数据结构与内存布局

- **Quality Profile**：platform、device class、default tier、group overrides、startup-only flags。
- **Scalability Group State**：current tier、target tier、last change frame、hysteresis、reason。
- **Budget Pressure**：CPU frame、GPU frame、VRAM、RAM、IO、thermal、battery、network、audio underrun。
- **Degrade Rule**：group、order、threshold、cooldown、min/max tier、required caps。
- **User Settings Binding**：user choice、auto mode、locked groups、dirty/apply policy。

## 5. 核心算法

- **Profile Bootstrap**：Platform Profile + benchmark + user settings → initial QualityState。
- **Pressure Resolve**：收集 FrameStats/Memory/Streaming/Thermal → 计算 pressure → 按 degrade order 调整 target tier。
- **Hysteresis**：降级快速，升级慢速，避免帧间反复切换。
- **Apply Queue**：runtime-safe group 立即应用；startup-only group 标记 pending restart。
- **Cross-module Dispatch**：Render、Asset、Audio、Animation、VFX、UI、Scripting 订阅 QualityState 快照。
- **Validation**：profile 中启用的 feature 必须被 capability matrix 支持，否则自动降级并记录。

## 6. 线程与内存模型

ScalabilityManager 在帧边界发布 immutable QualityState。模块在安全点读取快照并更新本地参数。压力采样来自 FrameStats/Telemetry ring，避免热路径锁。用户设置写盘走 Save/Profile。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| profile 引用 unsupported feature | 降级并记录 profile validation error |
| 用户设置非法 | clamp 到 platform allowed range |
| runtime 改 startup-only group | 标记 pending restart，不立即破坏系统 |
| 自动降级无法满足预算 | 继续降非核心组；仍失败则报告 severe pressure |
| module 未响应 quality event | Test profile fail，输出 missing subscriber |

**禁止**：模块私自定义不可观测质量档；自动降级关闭 Proven 必需功能；压力解除后无 hysteresis 地频繁切换。

## 8. 性能目标

- QualityState 快照读取为 O(1) 或紧凑数组索引。
- 每帧 pressure resolve 成本低于常规 FrameStats 汇总成本。
- dynamic resolution 和高频质量调节不触发 shader/PSO 同步编译。

## 9. 测试计划

- profile schema、unsupported feature validation、startup-only pending。
- CPU/GPU/VRAM/thermal pressure injection。
- degrade order、hysteresis、user locked group。
- Render/Asset/Audio/VFX/UI/Scripting subscriber coverage。
- low/high/ultra/cinematic profile golden/smoke。

## 10. 开放问题 / Spike

- 自动 benchmark 是否进入首期，或只使用用户/平台 preset。
- Dynamic resolution 与 upscaler/Frame Generation 的优先级。
- Thermal/battery pressure 数据在各平台的可用性。
- 项目是否可注册自定义 ScalabilityGroup。

## 11. 依赖与被依赖

- **依赖**：Capability Tiering、Platform Profile、FrameStats、Memory Budget、Save/Profile、Render、Asset、Audio、VFX。
- **被依赖**：Shipping defaults、Performance tuning、Accessibility comfort settings、Editor device preview、Automated perf tests。
