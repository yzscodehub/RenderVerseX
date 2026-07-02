# RVX-NG · Feature/Terrain/Water/Foliage 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §12 Terrain/Water/Foliage  
**里程碑**：M9  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供开放世界地形、水体、植被系统：分块流式、GPU LOD、材质分层、物理/导航接缝、植被实例化、风场、水面反射/折射/波浪。

**范围内**：heightfield/clipmap terrain、terrain material、foliage instancing、impostor、wind、water surface、shoreline、caustics 预留、streaming。  
**范围外**：完整地形编辑器 UI、离线地貌生成工具、水下玩法系统。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 开放世界 | terrain/foliage/water 按 World Partition cell 流式 |
| GPU LOD | 地形、植被、水面按屏幕覆盖动态 LOD |
| 材质 | 地形支持多层材质、splat/virtual texture |
| 预算 | 植被实例数、阴影、碰撞、动画按预算控制 |
| 集成 | RenderGraph、GpuScene、Physics、Navigation 使用统一数据来源 |

## 3. 公共接口面（设计契约）

```cpp
class TerrainSystem {
    void LoadCell(WorldCellId cell);
    void RecordTerrainPass(RgBuilder& graph, const View& view);
    HeightQueryResult QueryHeight(Vec3 worldPos) const;
};

class FoliageSystem {
    void BuildInstances(WorldCellId cell, FoliageLayer layer);
    void RecordCulling(RgBuilder& graph, const View& view);
};

class WaterSystem {
    void RecordWater(RgBuilder& graph, const View& view, RgTexture sceneDepth);
};
```

**不变量**：地形/植被运行时数据由 streaming/cook 产物驱动；渲染剔除和绘制走 GPU-driven；物理/导航使用同版本 cell 数据；水体 pass 通过 RenderGraph 声明所有依赖。

## 4. 数据结构与内存布局

- **Terrain Tile**：height、normal、material weight、bounds、LOD metadata。
- **Terrain Clipmap**：近场高精度，远场低精度 ring。
- **Foliage Cluster**：实例 transform、species/material、bounds、wind params。
- **Impostor Atlas**：远景植被/树木 billboard 或 mesh proxy。
- **Water Body**：surface mesh/curve、wave spectrum、flow map、material params。

## 5. 核心算法

- **Terrain LOD**：chunk/clipmap LOD，裙边或 crack mask 处理裂缝。
- **Terrain Material**：virtual texture 或分层材质采样，按平台降级。
- **Foliage Placement**：cook 阶段生成候选实例，运行时按密度/规则/预算裁剪。
- **Foliage Culling**：cluster + instance GPU culling，近景真实 mesh，远景 impostor。
- **Wind**：全局/局部 wind field 驱动 shader deformation。
- **Water**：FFT/gerstner/flow map 分级，SSR/RT/reflection probe 组合反射，深度驱动 shoreline/foam。

## 6. 线程与内存模型

cell 加载、实例生成、碰撞/导航代理生成走 JobSystem。地形/植被 GPU 资源持久驻留并受 streaming 预算控制。水体模拟可在 GPU compute 或 CPU 低频更新。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| cell 未驻留 | 使用低精度 proxy 或不可见 |
| 植被实例超预算 | 按距离/重要性裁剪密度 |
| 地形材质缺失 | fallback 材质 |
| 水反射能力不足 | 降级 probe/SSR/环境反射 |

**禁止**：每帧 CPU 全量生成植被；物理/导航与渲染使用不同版本地形数据；水体直接旁路 RenderGraph。

## 8. 性能目标

- 大规模植被实例由 GPU culling + indirect 绘制。
- terrain cell streaming 无帧尖峰。
- water 质量档可明显控制反射、波浪、透明成本。

## 9. 测试计划

- terrain LOD 裂缝、height query、材质层采样。
- foliage culling、impostor 切换、预算裁剪。
- water reflection/refraction fallback。
- cell 流式一致性：render/physics/nav 同版本。

## 10. 开放问题 / Spike

- Terrain virtual texture 与通用 VT 的共享方式。
- 植被 procedural placement 是否运行时可编辑。
- 水模拟算法首期选型。
- 大世界坐标精度与浮动原点策略。

## 11. 依赖与被依赖

- **依赖**：World Partition、Asset streaming、RenderGraph、GpuScene、Physics、Navigation。
- **被依赖**：Gameplay traversal、AI nav、Editor world tools、Render lighting/shadows。


