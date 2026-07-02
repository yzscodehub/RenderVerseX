# RVX NG · Render/Lighting & GI 详细设计（Tier 1）

**日期**：2026 06 20
**层级**：Tier 1 · 派生自总纲 §9 Lighting、决策 D6/D7
**里程碑**：M5（光照）/ M6（GI 目标能力）/ M12（ReSTIR）
**状态**：详细设计 · 待 S6 Spike
**地位**：Clustered 延迟着色 + **GI 三档分级**（探针/烘焙→辐照缓存混合 Lumen 式→ReSTIR）。

   

## 1. 目标与范围

**目标**：海量动态光的 clustered 光剔除 + 延迟着色；GI 走分级路线，主线 = 辐照缓存混合（屏幕探针 + 世界探针 + RT），低档 = 探针/烘焙兜底，高端 = ReSTIR（M12）。

**范围内**：cluster 构建与光剔除、延迟着色（消费 G Buffer）、面光源、GI 三档、AO。
**范围外**：阴影（Shadows）、RT 原语（RayTracing）、几何/G Buffer 生成（Geometry）。

## 2. 需求与约束

| 约束 | 要求 |
|   |   |
| 多光源 | 数千动态光，clustered 剔除（非逐像素遍历全光） |
| 着色 | 延迟（G Buffer→compute/fullscreen shade），Cook Torrance PBR |
| GI 分级 | 探针(低)→辐照缓存混合(主)→ReSTIR(高)，可干净降级（C6） |
| 面光源 | LTC 或近似 |
| 兜底 | RT 不可用时 GI 走探针/SSGI（不黑屏） |

## 3. 公共接口面（设计契约）

```cpp
class ClusteredLighting {
    void RecordBuildClusters(RgBuilder&, View, BufferHandle lights, BufferHandle& clusterGrid);
    void RecordCullLights(RgBuilder&, BufferHandle clusterGrid, BufferHandle& lightIndexList);
};

class DeferredShading {
    // G Buffer + 光列表 + 阴影 + GI → scene color
    void Record(RgBuilder&, RgTexture gbuffer[], BufferHandle lightLists,
                RgTexture shadowMask, RgTexture giResult, RgTexture& sceneColor);
};

class GlobalIllumination {            // 分级：tier 决定走哪条
    enum Tier { Probe, RadianceCache, ReSTIR };
    void Record(RgBuilder&, Tier, const GiInputs&, RgTexture& giResult);
};
```

**不变量**：着色用 clustered 光列表（非全光遍历）；GI tier 不支持上档 → 自动降到下档（探针永远可用兜底）；延迟管线不走 MSAA（TAA only，D0 范围决策）。

## 4. 数据结构与内存布局

  **Cluster 网格**：视锥按屏幕 tile × 深度切片（如 16×9×24）；每 cluster 存光索引偏移+数。
  **光索引列表**：紧凑全局 light index buffer（cluster→其影响光）。
  **G Buffer**：albedo/normal/roughness metallic/motion/emissive（紧凑编码，octahedral normal）。
  **GI 数据**：
    Probe 档：irradiance volume / 反射探针 cubemap 数组。
    辐照缓存档：屏幕空间探针 + 世界空间 radiance cache（hash grid / clipmap）。
    ReSTIR 档：reservoir buffer（时空蓄水池）。

## 5. 核心算法

  **Cluster 构建**：compute 算各 cluster 的视锥/AABB。
  **光剔除**：compute 把每光分配到相交 cluster → 紧凑 light index list。
  **延迟着色**：compute（tiled）逐像素读 G Buffer → 遍历本 cluster 光列表 → Cook Torrance + 阴影掩码 + GI → scene color。
  **GI 分级**：
    **Probe**：采样最近 irradiance probe + 反射探针（低成本兜底）。
    **辐照缓存混合（主线）**：屏幕空间探针追踪（SDF/depth）+ 世界空间 radiance cache 填补屏幕外 + RT 命中喂缓存（硬件 RT 可用时）。
    **ReSTIR（M12）**：时空 reservoir 重采样 + 强降噪。
  **面光源**：LTC（线性变换余弦）。

## 6. 线程与内存模型

  全 GPU compute；cluster/light cull 可走 async compute（RenderGraph）。
  GI 世界缓存持久（跨帧），residency 管理。
  G Buffer/光列表瞬态（RenderGraph 别名）。

## 7. 错误与失败语义

| 情形 | 处理 |
|   |   |
| 光数超 cluster 容量 | 上限裁剪 + 监控；超额光降优先级 |
| RT 不可用 | GI 降到辐照缓存(无 RT)/探针档（不黑屏） |
| GI 噪声（ReSTIR） | 强降噪 + 时域累积；disocclusion 处理 |
| 探针未烘焙 | 运行时动态探针或纯环境光兜底 |

**禁止**：逐像素遍历全光源；GI 单档无兜底。

## 8. 性能目标

  光剔除：数千光 < 0.5 ms（GPU）。
  延迟着色：1080p/4K 在帧预算内；clustered 使代价 O(像素 × cluster 光数)，非 O(像素 × 全光)。
  GI 分级：Probe/烘焙/SSGI 兜底可出货；Radiance Cache 作为 M6 目标能力，S6 关闭后才可升格主线。

## 9. 测试计划

  **正确性**：cluster 光分配 vs 参考、PBR 着色 golden、面光源。
  **分级**：每 GI tier 输出 golden + tier 降级路径（RT off → Probe/SSGI fallback；Radiance Cache off）无黑屏。
  **多光压力**：数千光帧时基线。
  **golden**：延迟着色跨后端逐像素一致。

## 10. 开放问题 / Spike

  **辐照缓存策略 spike（M6 前）**：世界缓存用 hash grid vs clipmap；屏幕探针密度/追踪步进。
  ReSTIR 降噪管线（M12）。
  透明的 clustered 复用（Forward+，见 PostProcess/Transparency）。
  面光源阴影。

## 11. 依赖与被依赖

  **依赖**：Geometry（G Buffer）、GpuScene（光表）、Shadows（阴影掩码）、RayTracing（GI/反射的 RT 命中）、RenderGraph、RHI。
  **被依赖**：PostProcess（消费 scene color）、FrameRenderer。





