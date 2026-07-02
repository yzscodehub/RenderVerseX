# RVX-NG · Render/Material System 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自总纲 §9 Render、ShaderCompiler、Asset Pipeline  
**里程碑**：M4-M7 / M10 Graph Editors  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义材质资产、材质实例、shader model、参数布局、permutation、graph IR、runtime GPU 表和 fallback 规则，使材质系统能支撑 PBR 主线、平台分级、编辑器图形化 authoring、离线 cook 和 Shipping PSO 预烘焙。

**范围内**：Material Type、Material Asset、Material Instance、参数布局、纹理绑定、shader permutation、材质图 IR、cook、runtime GPU material table、fallback/error material、质量档。  
**范围外**：具体 shader 代码实现、Editor graph UI 细节、第三方 DCC 材质完整兼容。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 数据驱动 | 材质资产经反射/schema 描述，不硬编码到 pass |
| GPU 友好 | runtime 材质参数进入 GpuScene/MaterialGpu 表，bindless 索引访问 |
| 可分级 | shading model、feature、texture set 按 platform/profile 裁剪 |
| 可预烘焙 | Shipping 所需 shader permutation/PSO 在 cook 阶段确定 |
| 可诊断 | 编译失败、缺纹理、未驻留、unsupported feature 有可见 fallback |
| 可扩展 | Material Type 可注册新 shading model，但不得破坏固定 PBR 主线 |

## 3. 公共接口面（设计契约）

```cpp
enum class ShadingModel : uint8 {
    Unlit,
    DefaultLit,
    Clearcoat,
    Subsurface,
    Hair,
    Terrain,
    Custom
};

struct MaterialKey {
    AssetId materialAsset;
    ShadingModel model;
    FeatureMask features;
    RenderPassMask passes;
    QualityTier tier;
};

class MaterialSystem {
public:
    Result<MaterialHandle> Load(AssetId asset);
    MaterialInstanceHandle CreateInstance(MaterialHandle base);
    void SetParameter(MaterialInstanceHandle, Name param, Variant value);
    BindlessIndex GetGpuMaterial(MaterialInstanceHandle);
    ShaderSetHandle ResolveShaders(MaterialKey key);
};

class MaterialCooker {
public:
    CookResult CookMaterialGraph(AssetId graphAsset, CookProfile profile);
    Span<const PsoKey> EnumerateRequiredPsos(MaterialKey key);
};
```

**不变量**：runtime 不解析编辑器 graph；graph 必须 cook 为稳定 IR/bytecode + material metadata；材质 public API 不暴露后端 shader 对象；缺失材质必须显示 error material 而不是静默黑屏。

## 4. 数据结构与内存布局

- **Material Type**：shading model、required textures、parameter schema、supported passes、feature flags、fallback model。
- **Material Asset**：graph IR 或 text/binary material source、default parameters、texture references、shader feature set、version。
- **Material Instance**：base material、override parameter block、runtime texture handles、quality override。
- **MaterialGpu**：packed scalar/vector parameters、bindless texture indices、sampler id、flags、shading model id。
- **Permutation Key**：material type、render pass、vertex factory、skin/morph flags、lighting mode、quality tier、backend caps。
- **Material Diagnostics**：missing texture、unsupported feature、compile error、fallback reason、PSO readiness。

## 5. 核心算法

- **Graph Compile**：Editor graph → typed IR → validation → shader snippets/includes → shader compiler input。
- **Permutation Prune**：根据 material usage、platform caps、quality tier、render pass mask 裁剪 permutation。
- **Parameter Packing**：按 Material Type schema 生成 CPU/GPU layout，保证 C++/shader 反射一致。
- **Runtime Resolve**：MaterialInstance dirty → update parameter block → update GpuScene material slot。
- **Fallback Resolve**：unsupported feature 降级到 lower tier；compile fail 使用 error material；missing texture 使用 typed fallback texture。
- **PSO Enumeration**：cook 根据 scene/material usage 产 PsoKey 列表，交给 ShaderCompiler/Build graph。

## 6. 线程与内存模型

材质加载和 cook 是后台 job；runtime 参数更新在 safe point 合并到 render snapshot。MaterialGpu 表由 GpuScene 管理，增量上传。Shader 编译在 Development 可异步，Shipping 只消费预烘焙 artifact。材质实例参数使用紧凑 POD block，避免每帧 map/string 查找。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| graph validation fail | cook 失败，保留旧 cooked artifact |
| shader compile fail | Development 显示 error material；Shipping 视为 cook/release gate 失败 |
| texture missing | typed fallback texture + WARN + diagnostics |
| feature unsupported | 降级到 fallback model/tier，FrameStats 记录 |
| parameter type mismatch | 拒绝写入，报告 schema/name/type |

**禁止**：Shipping runtime 编译新材质 shader；材质 graph 直接进入 runtime；材质失败静默渲染为黑色。

## 8. 性能目标

- 材质参数上传只处理 dirty instance。
- shader permutation 数量由 cook 报告并设上限，超过预算 fail 或要求裁剪。
- draw/dispatch 侧只通过 bindless index 和 packed parameter 访问材质。
- 材质实例创建和参数修改不阻塞 shader compile。

## 9. 测试计划

- Material schema 与 shader reflection layout 一致。
- graph compile、permutation prune、PSO enumeration。
- missing texture/error material/unsupported feature fallback。
- MaterialInstance 参数 override、dirty 上传、GpuScene material table golden。
- Shipping cook：无 runtime shader compile，所需 PSO 全部可解析。

## 10. 开放问题 / Spike

- Material Graph IR 是自定义 IR、SPIR-V 片段还是 DSL。
- Substrate/分层 BSDF 何时进入 Research overlay。
- Shader permutation 爆炸的预算上限和自动裁剪策略。
- DCC 材质导入与引擎 Material Type 的映射范围。

## 11. 依赖与被依赖

- **依赖**：ShaderCompiler、Asset Pipeline、GpuScene、RenderGraph、Reflection、Build/Cook、Editor Graph。
- **被依赖**：Geometry、Lighting、RayTracing、VFX、Terrain、UI、Editor、Shipping Cook。
