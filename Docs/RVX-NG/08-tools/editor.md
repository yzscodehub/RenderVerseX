# RVX-NG · Tools/Editor 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §13 Editor  
**里程碑**：M10  
**状态**：详细设计 · 待 S11 Spike

---

## 1. 目标与范围

**目标**：提供与运行时同源的引擎编辑器：真实 World 编辑、PIE、Inspector、Gizmo、Asset Browser、Material/Shader Graph、VFX Graph、Sequencer、Profiler/FrameDebugger 集成与源码/版本控制接缝。

**范围内**：编辑器应用壳、窗口/布局、编辑世界、selection、undo/redo、property inspector、viewport、asset browser、PIE、tool mode、graph editors、sequencer。  
**范围外**：多人协作编辑、云资产管理、平台商店发布后台。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 同一运行时 | Editor 视口使用引擎 Render/RHI，不使用假预览 |
| 数据真实 | Inspector 经 Reflection 编辑真实对象/组件/资产 |
| 可撤销 | 所有编辑操作走 transaction/command |
| PIE 隔离 | 编辑世界与游戏运行世界状态隔离，可复制/回滚 |
| 可扩展 | Tools 通过插件/模块注册面板、菜单、命令 |

## 3. 公共接口面（设计契约）

```cpp
class EditorApp {
    void Initialize(EditorConfig config);
    void Tick();
    void Shutdown();
};

class TransactionManager {
    void Begin(Name label);
    void Record(ChangeOp op);
    void Commit();
    void Undo();
    void Redo();
};

class EditorViewport {
    void SetWorld(WorldHandle world);
    void Render(RgBuilder& graph);
    PickResult Pick(ScreenPos pos);
};
```

**不变量**：编辑操作必须可序列化为 transaction；PIE 不直接污染编辑世界；Editor UI 不绕过 runtime 资产/反射/World API；所有工具命令有权限和上下文检查。

## 4. 数据结构与内存布局

- **Editor Context**：当前 project、world、selection、active tool、PIE state。
- **Transaction Log**：变更 op、before/after、asset/world target、merge policy。
- **Selection Set**：Entity/Asset/Subobject 句柄集合。
- **Viewport State**：camera、gizmo mode、show flags、debug overlays。
- **Tool Registry**：panel、command、menu、shortcut、asset editor factory。

## 5. 核心算法

- **Inspector**：Reflection 遍历属性，生成 editor widgets，编辑产生 ChangeOp。
- **Undo/Redo**：事务提交后写入 ring/log；支持合并拖拽连续变更。
- **PIE**：复制或 fork world，切换输入/时间/模块模式，结束后销毁运行世界。
- **Asset Browser**：Registry 查询、缩略图队列、拖放、重导入、依赖查看。
- **Sequencer**：轨道驱动 property/entity/camera/audio，时间轴可预览和烘焙。
- **Graph Editors**：Material/VFX/Shader Graph 生成中间 IR，交给 cook/compiler。

## 6. 线程与内存模型

Editor 主线程负责 UI 与 OS 事件；资产扫描、缩略图、cook、索引走 JobSystem。视口渲染生成 RenderSnapshot，不直接读写编辑中的可变结构。编辑命令在帧安全点应用。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 事务应用失败 | 回滚本事务并报告 |
| PIE 崩溃 | 隔离运行世界，保留编辑世界 |
| 资产重导入失败 | 保留旧 cooked 版本 |
| 工具插件错误 | 卸载/禁用插件，Editor 进入安全模式 |

**禁止**：不可撤销地修改世界/资产；Editor 视口使用脱离 runtime 的渲染路径；PIE 与编辑世界共享可变运行时对象。

## 8. 性能目标

- 大型场景 selection/inspector 响应在交互预算内。
- 缩略图/cook/index 后台化，不阻塞视口。
- Editor 视口性能可通过同一 Profiler 归因。

## 9. 测试计划

- transaction undo/redo、PIE 隔离、asset reimport、selection/picking。
- inspector 反射属性编辑往返。
- graph editor cook/compile golden。
- Editor 安全模式与插件失败注入。

## 10. 开放问题 / Spike

- Editor UI 技术栈：自研 UI vs ImGui/Qt 混合。
- World fork/PIE 的内存复制策略。
- Graph IR 与 ShaderCompiler/VFXSystem 的边界。
- 多窗口/多视口 docking 与跨平台输入。

## 11. 依赖与被依赖

- **依赖**：Reflection、World、Asset、Render、Input、UI、Profiler、Build/Cook。
- **被依赖**：内容生产、调试、自动化测试、团队工作流。


