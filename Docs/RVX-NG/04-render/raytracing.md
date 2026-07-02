# RVX-NG · Render/RayTracing 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §9 RayTracing、决策 D7
**里程碑**：M6（RT 阴影/反射）/ M12（ReSTIR GI）
**状态**：详细设计 · v1.0 可评审
**地位**：混合 RT 一等公民。**命中点真受光**。ReSTIR GI 为 M12 高端档。

---

## 1. 目标与范围

**目标**：加速结构（BLAS/TLAS）管理 + RT 阴影/反射 + 命中点正确二次光照 + 降噪；为 Lighting 的辐照缓存喂 RT 命中。

**范围内**：AS 场景（BLAS 缓存/复用、TLAS 重建、实例/材质/alpha 元数据、shader table）、RT 反射、命中着色（direct light + shadow ray + IBL/缓存）、miss 采样环境、降噪。RT 阴影主体在 Shadows。
**范围外**：clustered 延迟着色（Lighting）、AS 构建原语（RHI）、ReSTIR 降噪细节（M12）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| AS | BLAS 缓存复用（静态不重建）+ TLAS 按帧重建/refit |
| 命中着色 | 命中点 = direct light + shadow ray + IBL/辐照缓存（**真受光**，非裸 albedo） |
| miss | 未命中采样天空/环境（**非黑**） |
| any-hit | alpha 测试（材质 alpha 元数据） |
| 降噪 | 时空 + 边缘感知（深度/法线/置信度） |
| 分级 | RT 不可用 → SSR 兜底（C6） |

## 3. 公共接口面（设计契约）

```cpp
class RayTracingScene {               // caps.rayTracing 守门
    bool IsSupported() const;         // 同时查 rayTracing + rtPipeline
    void RecordBuildBLAS(RgBuilder&, BufferHandle dirtyMeshes);  // 缓存复用
    void RecordBuildTLAS(RgBuilder&, BufferHandle instances);    // 每帧实例 + 元数据
    ASHandle Tlas() const;
    BufferHandle MaterialMeta(), GeometryMeta();  // 命中着色用 bindless 元数据
};

class RtReflections {
    void Record(RgBuilder&, ASHandle tlas, RgTexture gbuffer[], RgTexture& reflection);
};

class RtDenoise {
    void Record(RgBuilder&, RgTexture noisy, RgTexture depth, RgTexture normal, RgTexture& clean);
};
```

**不变量**：`IsSupported` 查全（rayTracing **且** rtPipeline）；命中着色含 direct light + shadow ray + IBL（真受光）；miss 采样环境（非零）；不支持 → SSR 兜底，不黑屏。

## 4. 数据结构与内存布局

- **BLAS 缓存**：每静态 mesh 一 BLAS，存活复用；skinned/动态按需 refit。
- **TLAS 实例**：每帧从 GpuScene 实例上传 native instance record（transform + instanceID→元数据索引）。
- **材质元数据表**（bindless）：base/MR/normal/emissive 索引 + UV 变换 + alpha cutoff（any-hit 用）。
- **几何元数据**：index/UV/normal/tangent buffer 的 bindless 索引（命中点属性插值）。
- **Shader table**：raygen/miss/hit group（对齐 RHI shader table 布局）。
- **reservoir**（M12 ReSTIR）：时空蓄水池 buffer。

## 5. 核心算法

- **BLAS**：脏 mesh（新增/形变）构建/refit；静态复用（不每帧建，省时间）。
- **TLAS**：每帧用 GpuScene 实例重建（或 refit）；instanceID 指向材质/几何元数据。
- **RT 反射**：raygen 从 G-Buffer 反射方向投 ray → closest-hit：插值属性 + 采样 bindless 材质 → **命中点真着色**（采样最近光 + 投 shadow ray + 采辐照缓存/IBL）→ 写 reflection（带置信度）；**miss → 采样天空/环境**。any-hit 做 alpha 测试。
- **降噪**：时空重投影 + 边缘感知（深度/法线/置信度加权）spatial。
- **分级**：caps 不足 → RtReflections 用 SSR；GI 高端 ReSTIR 走 reservoir（M12）。

## 6. 线程与内存模型

- AS 构建录制于 render 阶段（GPU）；TLAS 在几何之后、光照之前。
- BLAS 缓存持久驻留；TLAS/scratch 瞬态。
- RT dispatch + 降噪 compute，可与其他 async 重叠。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| RT 不支持 | SSR 兜底；GI 走辐照缓存(无 RT)/探针（C6，不黑） |
| AS 构建失败 | 该实例不入 TLAS（不崩）；WARN |
| 命中元数据缺失 | 用 fallback 材质（不裸 albedo） |
| 反射噪声 | 降噪 + 时域累积；置信度门控未命中（不黑 smear） |

**禁止**：命中点用裸 albedo；miss 返回黑；只查一半能力位。

## 8. 性能目标

- BLAS 缓存命中率高（静态场景近零重建）。
- TLAS 重建 < 0.5 ms（典型实例数）。
- RT 反射 + 降噪在帧预算内（按 spp/分辨率分级）。
- DX12/Vulkan RT 对等（Vulkan RT 必须真实现）。

## 9. 测试计划

- **正确性**：命中着色 = 参考路径追踪近似（真受光，非裸 albedo）；miss = 环境。
- **跨后端**：DX12/Vulkan RT 反射/阴影逐像素近似一致（容差内）。
- **能力门禁**：`IsSupported` 查全；Vulkan 谎报回归防护。
- **降级**：RT off → SSR/探针，无黑屏。
- **降噪**：disocclusion/firefly 处理 golden。

## 10. 开放问题 / Spike

- **ReSTIR GI（M12）**：reservoir 时空重采样 + 降噪管线。
- 命中着色的光采样策略（最近 N 光 vs 重要性采样）。
- BLAS refit vs rebuild 阈值（skinned）。
- 主机 RT 模型差异（与 RHI 主机接缝协调）。

## 11. 依赖与被依赖

- **依赖**：RHI（AS/DispatchRays/shader table）、GpuScene（实例/材质元数据）、Shadows、RenderGraph、ShaderCompiler。
- **被依赖**：Lighting（GI/反射）、Shadows（RT 阴影）、FrameRenderer。


