# RVX-NG · Render/PostProcess & Atmosphere 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §9 PostProcess/AtmosphereVolumetrics/Decals/Transparency、决策 D0(MSAA)
**里程碑**：M5（天空/体积/贴花）/ M7（合成/后处理）
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：场景色之上的合成与后处理链——天空/体积、延迟贴花、Forward+ 透明、TAA + 上采样、tonemap + HDR 显示输出、bloom/DOF/motion blur。

**范围内**：Sky-Atmosphere、体积雾/光、延迟贴花、Forward+ 透明（接入阴影）、TAA、上采样抽象(DLSS/FSR/XeSS)、tonemap、HDR 输出/色彩管理、bloom/DOF/motion blur、debug 可视化模式。
**范围外**：延迟着色（Lighting）、RT（RayTracing）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| AA | **TAA-only（无 MSAA，D0）** + 上采样 |
| 上采样 | DLSS/FSR/XeSS 统一抽象，运行时可选 |
| 透明 | Forward+（复用 clustered 光照），**接入阴影** |
| 顺序 | 贴花(延迟前)→透明(着色后)→后处理(线性)→HDR 输出 |
| HDR | tonemap + HDR10/scRGB 显示输出 + 色域映射 |
| 体积 | Sky-Atmosphere + 体积雾/光，纳入 RenderGraph |

## 3. 公共接口面（设计契约）

```cpp
class AtmosphereVolumetrics {
    void RecordSky(RgBuilder&, View, RgTexture& sceneColor);      // 天空模型
    void RecordVolumetrics(RgBuilder&, RgTexture depth, BufferHandle lights, RgTexture& sceneColor);
};
class Decals { void Record(RgBuilder&, RgTexture gbuffer[], BufferHandle decals); }; // 延迟前
class TransparentPass {                                            // Forward+，接入阴影
    void Record(RgBuilder&, const DrawList& transp, BufferHandle lightLists,
                RgTexture shadowMask, RgTexture& sceneColor);
};
class PostProcessStack {
    void Record(RgBuilder&, RgTexture sceneColor, const PpSettings&, RgTexture& ldrOutput);
    // 内部链：TAA → 上采样 → bloom → DOF → motion blur → tonemap → 色彩分级 → HDR 输出
};
class Upscaler { enum Kind { TAAU, DLSS, FSR, XeSS }; /* 统一抽象 */ };
```

**不变量**：透明走 clustered 光照且接入阴影掩码；后处理在线性 HDR 空间，最后 tonemap + HDR 显示输出；无 MSAA 路径（TAA-only）。

## 4. 数据结构与内存布局

- **历史缓冲**：TAA/上采样需要前帧 color + depth + motion（持久，jitter 序列）。
- **体积**：froxel（视锥体素）散射/透射缓冲。
- **贴花**：延迟贴花在 G-Buffer 写入阶段后、着色前混合。
- **后处理瞬态**：ping-pong 链（RenderGraph 别名）。

## 5. 核心算法

- **贴花**：延迟贴花投影到 G-Buffer（着色前混合 albedo/normal）。
- **透明**：Forward+ 主 pass，复用 cluster 光列表 + 阴影掩码；排序（OIT 可选高端）。
- **TAA + 上采样**：jitter 抖动 + 历史重投影 + neighborhood clamp；上采样器（DLSS/FSR/XeSS/内置 TAAU）按设置选。
- **tonemap + HDR 输出**：线性 → tonemap 算子 → 色彩分级 → SDR/HDR10/scRGB 输出（OETF + 色域）。
- **体积**：froxel 散射积分（参与介质）。

## 6. 线程与内存模型

- 全 GPU compute/fullscreen；后处理链可部分 async。
- 历史缓冲持久（TAA/上采样/motion blur）。
- ping-pong 瞬态由 RenderGraph 别名。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 上采样器不可用（无 DLSS 硬件） | 降到 FSR/内置 TAAU（C6） |
| HDR 显示不支持 | 降到 SDR tonemap |
| TAA ghosting/disocclusion | neighborhood clamp + motion 拒绝 |
| 透明排序错误 | 质心排序（大物/相交仍有限，OIT 高端档） |

**禁止**：MSAA 路径（D0）；透明无阴影；后处理在非线性空间做物理运算。

## 8. 性能目标

- 上采样：从低内分辨率重建到目标分辨率，帧时显著下降（DLSS/FSR 性能档）。
- 后处理链在帧预算内；ping-pong 别名省显存。
- 透明 Forward+ 复用光列表，无重复剔除。

## 9. 测试计划

- **golden**：天空/体积/贴花/透明/tonemap 各 golden；HDR 输出色彩正确。
- **TAA**：静止收敛、运动无 ghosting（disocclusion 测试）。
- **透明阴影门禁**：透明面接收阴影（防回归）。
- **上采样**：各上采样器路径 + 降级。

## 10. 开放问题 / Spike

- 上采样器集成（DLSS SDK / FSR / XeSS）与统一抽象边界。
- OIT 是否进高端档（加权混合 vs per-pixel linked list）。
- 体积分辨率/froxel 密度 vs 成本。
- HDR 显示校准/色调映射算子选型。

## 11. 依赖与被依赖

- **依赖**：Lighting（scene color/光列表）、Shadows（掩码）、Geometry（透明 draw list/motion）、RenderGraph、RHI（HDR swapchain）。
- **被依赖**：FrameRenderer（编排）、UI（叠加于 LDR 输出）。



