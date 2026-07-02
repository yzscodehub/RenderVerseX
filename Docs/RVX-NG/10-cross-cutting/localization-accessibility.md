# RVX-NG · Cross-cutting/本地化与无障碍 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §15  
**里程碑**：M9-M11  
**状态**：详细设计 · 待 S10 Spike

---

## 1. 目标与范围

**目标**：将本地化和无障碍作为引擎级能力，覆盖文本、字体、语音/字幕、输入重绑定、色彩辅助、屏幕阅读接缝、UI 焦点导航和平台可访问性要求。

**范围内**：localized text、string table、plural/gender rules、font fallback、RTL/CJK、subtitle/caption、input remapping、color-blind filters、screen reader hooks、accessibility settings。  
**范围外**：具体翻译服务、语音合成内容生产、平台认证流程本身。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 无硬编码文本 | 用户可见文本必须经过 localization key 或明确 debug 标记 |
| 字体完整 | 支持 CJK、RTL、fallback font、字形缺失诊断 |
| 输入可重绑 | 所有 gameplay action 支持重绑定、冲突检测、设备切换 |
| 字幕可用 | 对话、重要音效、过场可输出字幕/说明 |
| 平台接缝 | 可对接平台无障碍 API 与系统设置 |

## 3. 公共接口面（设计契约）

```cpp
struct LocalizedText {
    TextKey key;
    Locale locale;
    Span<const TextArg> args;
};

class LocalizationSystem {
    String Resolve(LocalizedText text) const;
    void SetLocale(Locale locale);
    void ReloadTables();
};

class AccessibilitySystem {
    void ApplySettings(AccessibilitySettings settings);
    void Announce(AccessibleEvent event);
};
```

**不变量**：UI 渲染前必须 resolve locale；字体 fallback 不允许静默丢字；输入 action 名称也走 localization；无障碍设置可在运行时切换并持久化。

## 4. 数据结构与内存布局

- **String Table**：namespace/key、locale、format string、metadata、source reference。
- **Font Set**：primary font、fallback chain、coverage map、atlas profile。
- **Subtitle Track**：time range、speaker、localized text、priority。
- **Input Binding Profile**：action、device、binding、conflict group、display glyph。
- **Accessibility Settings**：视觉、听觉、输入、认知辅助分类。

## 5. 核心算法

- **Text resolve**：locale fallback → plural/gender/select rules → argument formatting。
- **Font fallback**：按 codepoint coverage 切分 text runs，交给 UI text shaping。
- **Subtitle routing**：Audio/Sequencer/Game events 生成 subtitle cue，UI 统一展示。
- **Input remap**：冲突检测、设备 glyph 解析、profile 保存。
- **Color assistance**：后处理或 UI theme 层应用色盲/高对比策略。
- **Screen reader**：UI focus/announce event 映射到平台 API。

## 6. 线程与内存模型

String table 和 font metadata 可后台加载，locale 切换在帧安全点原子替换。Text shaping 可走 JobSystem；UI 使用 resolved snapshot，避免渲染线程访问可变 localization 数据。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| key 缺失 | 显示 key/debug fallback，记录缺失 |
| 字形缺失 | fallback font，仍缺则 tofu glyph 并计数 |
| 绑定冲突 | 阻止保存或提示替换 |
| 平台无障碍 API 不可用 | 内部 UI 辅助仍保持可用 |

**禁止**：Shipping 用户可见硬编码文本；字体缺字无诊断；输入 action 无重绑定路径。

## 8. 性能目标

- Text resolve 缓存命中后开销低。
- Locale 切换不造成长时间主线程停顿。
- 字体 atlas miss 可控并可观测。

## 9. 测试计划

- string table missing key、plural、RTL/CJK、font fallback。
- input remap conflict、device switch、glyph display。
- subtitle timing、screen reader announce。
- color-blind/high-contrast visual checks。

## 10. 开放问题 / Spike

- ICU/HarfBuzz/FreeType 等库组合与平台授权。
- 运行时 locale 热切换对资产和 UI cache 的影响。
- 屏幕阅读器抽象的跨平台能力矩阵。
- 字幕与 audio event metadata 的格式。

## 11. 依赖与被依赖

- **依赖**：UI、Input、Audio、Asset、Platform accessibility APIs。
- **被依赖**：Editor、Shipping certification readiness、Game UI、Sequencer。


