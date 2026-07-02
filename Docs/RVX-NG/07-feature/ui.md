# RVX-NG · Feature/UI 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §12 UI  
**里程碑**：M9  
**状态**：详细设计 · 待 S10 Spike

---

## 1. 目标与范围

**目标**：提供游戏 UI 与工具 UI 可共享的 retained-mode UI 框架：布局、样式、文本、输入、焦点、动画、无障碍、本地化，以及高效 RHI 渲染。

**范围内**：UI 树、布局、样式、文本 shaping、字体 atlas、输入路由、焦点/导航、动画、render batching、DPI/localization/accessibility。  
**范围外**：完整编辑器设计器 UI、Web runtime、平台原生 UI。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 文本正确 | CJK、RTL、emoji、fallback font、真实字形度量 |
| 输入统一 | 鼠标、键盘、手柄、触摸、IME 走统一路由 |
| 高效渲染 | batch draw rect/text/image/vector，atlas 与裁剪正确 |
| 可访问性 | 字幕、重绑定、色盲、读屏接缝、焦点导航 |
| 数据绑定 | UI 状态通过声明式绑定或显式 model 更新 |

## 3. 公共接口面（设计契约）

```cpp
class UiDocument {
    UiNodeId CreateNode(UiNodeDesc desc);
    void SetStyle(UiNodeId node, UiStyle style);
    void SetText(UiNodeId node, LocalizedText text);
    void DispatchInput(const InputEvent& event);
    void BuildDrawList(UiDrawList& outDrawList);
};

class UiRenderer {
    void Record(RgBuilder& graph, const UiDrawList& drawList, RgTexture target);
};
```

**不变量**：UI layout 与 render 分离；文本 shaping 结果按字体/语言/内容缓存；UI 输入事件不直接越权操作 gameplay；所有可见文本走 localization key 或明确的 debug 标记。

## 4. 数据结构与内存布局

- **UiNode**：父子关系、layout box、style、state、event handlers。
- **Style Cache**：resolved style，按继承和伪状态缓存。
- **Layout Tree**：flex/grid/absolute 结果，dirty 标记增量重算。
- **Text Run**：shaped glyph runs、line breaks、fallback font segments。
- **DrawList**：按 texture/material/clip 分组的绘制命令。

## 5. 核心算法

- **布局**：dirty subtree 增量 layout，支持 flex/grid/anchor。
- **文本**：HarfBuzz 风格 shaping、fallback font、bidi、line breaking、atlas packing。
- **输入**：hit test → capture/bubble → focus/navigation → command。
- **动画**：属性 tween/timeline，运行在 UI update 阶段，不阻塞 layout。
- **渲染**：draw list 合批，clip/scissor，SDF/MSDF 字体或 alpha atlas。

## 6. 线程与内存模型

UI 树更新在主/game 线程；文本 shaping 可后台 job；渲染 draw list 是帧快照，Render 只读。字体 atlas 更新通过 RenderGraph/RHI staging 受预算控制。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 字体缺字 | fallback font，仍缺则 tofu glyph 并记录 |
| localization key 缺失 | 显示 key/debug fallback |
| layout 约束冲突 | 按规则收缩/裁剪，记录 warning |
| atlas 满 | 新 atlas page 或回收低优先 glyph |

**禁止**：渲染线程修改 UI 树；用户可见硬编码文本绕过 localization；输入事件无焦点规则直接广播全局。

## 8. 性能目标

- 大型 HUD/菜单 dirty 增量更新，静态 UI layout 成本接近 0。
- draw call 数按 atlas/material 合批控制。
- 文本 shaping/atlas miss 不造成明显帧尖峰。

## 9. 测试计划

- layout golden、文本 shaping CJK/RTL/fallback、输入焦点导航。
- localization 缺失与字体缺字测试。
- render draw list golden。
- accessibility 焦点顺序与字幕/重绑定检查。

## 10. 开放问题 / Spike

- UI 描述格式：声明式 DSL/JSON/二进制 cook。
- 文本 shaping 库与字体 atlas 策略。
- 游戏 UI 与 Editor UI 是否共用同一 widget 系统。
- Vector/SVG 支持范围。

## 11. 依赖与被依赖

- **依赖**：Input、Localization、Asset/Fonts、RenderGraph/RHI、Scripting 可选。
- **被依赖**：Game HUD、Menus、Editor、Accessibility、Console/CVars UI。


