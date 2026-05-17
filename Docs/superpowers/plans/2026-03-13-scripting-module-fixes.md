# Scripting 模块优化实施计划

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Scripting 模块的关键 bug：Time 和 Input 系统的静态变量从未更新

**Architecture:** 在 ScriptEngine::Tick 中添加 UpdateTime 和 UpdateInput 同步函数调用

**Tech Stack:** C++20, Lua (sol2), RenderVerseX Engine

---

## 问题分析

当前 Scripting 模块的问题：
1. **Time 系统 bug**: `s_deltaTime` 和 `s_totalTime` 从未在 ScriptEngine::Tick 中更新
2. **Input 系统 bug**: `s_inputCache` 静态缓存从未从 InputSubsystem 同步状态
3. **ScriptEngine 集成**: Tick 方法仅处理热重载，未更新 Time/Input 绑定

---

## Chunk 1: 修复 Time 系统

### 任务 1.1: 添加 UpdateTime 函数到 CoreBindings

**Files:**
- Modify: `Scripting/Private/Bindings/CoreBindings.cpp:81-86`

- [x] **Step 1: 添加 UpdateTime 函数**

```cpp
// 在 CoreBindings.cpp 的匿名 namespace 中 (第 81-86 行后添加)

void UpdateTime(float deltaTime, float totalTime)
{
    s_deltaTime = deltaTime;
    s_totalTime = totalTime;
}
```

- [x] **Step 2: 注册 UpdateTime 到 Lua**

在 `RegisterCoreBindings` 函数中添加 (第 118 行后):

```cpp
// Time table
sol::table time = state.create_table();
time["GetDeltaTime"] = &GetDeltaTime;
time["GetTotalTime"] = &GetTotalTime;
time["UpdateTime"] = &UpdateTime;  // 添加这行
rvx["Time"] = time;
```

- [ ] **Step 3: 提交**

```bash
git add Scripting/Private/Bindings/CoreBindings.cpp
git commit -m "feat: add UpdateTime function to CoreBindings

- Add UpdateTime(deltaTime, totalTime) function
- Register UpdateTime in Time namespace for Lua scripts

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Chunk 2: 修复 Input 系统

### 任务 2.1: 添加 UpdateInputCache 函数到 InputBindings

**Files:**
- Modify: `Scripting/Private/Bindings/InputBindings.cpp`

- [x] **Step 1: 添加 InputBindings 头文件引用**

在文件顶部添加 forward declaration (第 7-10 行后添加):

```cpp
// Forward declare InputSubsystem
namespace RVX
{
    class InputSubsystem;
}
```

- [x] **Step 2: 添加 SyncInputCache 函数**

在 `InputStateCache` 定义后 (第 39 行后添加):

```cpp
void SyncInputCache(const InputSubsystem* input)
{
    if (!input) return;

    // Sync keyboard state
    for (int i = 0; i < 512; ++i)
    {
        s_inputCache.keys[i] = input->IsKeyDown(i);
        s_inputCache.keysPressed[i] = input->IsKeyPressed(i);
        s_inputCache.keysReleased[i] = input->IsKeyReleased(i);
    }

    // Sync mouse position
    input->GetMousePosition(s_inputCache.mouseX, s_inputCache.mouseY);
    input->GetMouseDelta(s_inputCache.mouseDeltaX, s_inputCache.mouseDeltaY);
    input->GetScrollDelta(s_inputCache.scrollX, s_inputCache.scrollY);

    // Sync mouse buttons
    for (int i = 0; i < 8; ++i)
    {
        s_inputCache.mouseButtons[i] = input->IsMouseButtonDown(i);
        s_inputCache.mouseButtonsPressed[i] = input->IsMouseButtonPressed(i);
        s_inputCache.mouseButtonsReleased[i] = input->IsMouseButtonReleased(i);
    }
}
```

- [x] **Step 3: 注册 SyncInputCache 到 Lua**

在 `RegisterInputBindings` 函数末尾 (第 369 行前添加):

```cpp
// Sync function (for internal use)
rvx["_SyncInputCache"] = &SyncInputCache;
```

- [ ] **Step 4: 提交**

```bash
git add Scripting/Private/Bindings/InputBindings.cpp
git commit -m "feat: add SyncInputCache to InputBindings

- Add SyncInputCache function to sync with InputSubsystem
- Sync keyboard, mouse position, delta, and scroll states

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Chunk 3: 集成到 ScriptEngine::Tick

### 任务 3.1: 修改 ScriptEngine::Tick 更新 Time 和 Input

**Files:**
- Modify: `Scripting/Private/ScriptEngine.cpp:64-77`

- [x] **Step 1: 添加必要的头文件**

在文件顶部 (第 1-10 行后添加):

```cpp
#include "Runtime/Input/InputSubsystem.h"
```

- [x] **Step 2: 修改 Tick 方法**

替换 `ScriptingSubsystem::Tick` 方法 (第 64-77 行):

```cpp
void ScriptingSubsystem::Tick(float deltaTime)
{
    // Get total time from TimeSubsystem if available
    float totalTime = 0.0f;
    auto* timeSys = Services::GetTimeSubsystem();
    if (timeSys)
    {
        totalTime = timeSys->GetTotalTime();
    }

    // Update time bindings for Lua scripts
    Bindings::UpdateTime(deltaTime, totalTime);

    // Sync input state from InputSubsystem
    auto* inputSys = Services::GetInputSubsystem();
    if (inputSys)
    {
        Bindings::SyncInputCache(inputSys);
    }

    // Update registered script components
    for (ScriptComponent* comp : m_components)
    {
        if (comp)
        {
            comp->OnTick(deltaTime);
        }
    }

    // Check for hot reload
    if (m_config.enableHotReload)
    {
        m_timeSinceLastCheck += deltaTime;
        if (m_timeSinceLastCheck >= m_config.hotReloadInterval)
        {
            m_timeSinceLastCheck = 0.0f;
            CheckForHotReload();
        }
    }
}
```

- [ ] **Step 3: 提交**

```bash
git add Scripting/Private/ScriptEngine.cpp
git commit -m "feat: integrate Time/Input sync in ScriptEngine::Tick

- Update time bindings each frame from TimeSubsystem
- Sync input cache from InputSubsystem each frame
- Call OnTick on registered ScriptComponents

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Chunk 4: 验证修复

### 任务 4.1: 编译验证

**Files:**
- Test: 编译整个项目

- [ ] **Step 1: 编译项目**

```bash
cmake --build --preset debug 2>&1 | head -50
```

预期: 无编译错误

- [ ] **Step 2: 运行测试 (如果有)**

```bash
./build/Tests/Debug/SystemIntegrationTest.exe
```

- [ ] **Step 3: 提交**

```bash
git commit -m "chore: verify Scripting module fixes compile

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## 预期产出

1. `RVX.Time.GetDeltaTime()` 返回正确的帧间隔
2. `RVX.Time.GetTotalTime()` 返回正确的总时间
3. `RVX.Input.IsKeyDown()`, `IsKeyPressed()`, `IsKeyReleased()` 正确响应
4. 鼠标位置和滚动状态正确同步

---

## 关键文件路径

| 文件 | 绝对路径 |
|------|----------|
| CoreBindings.cpp | `E:\WorkSpace\RenderVerseX\Scripting\Private\Bindings\CoreBindings.cpp` |
| InputBindings.cpp | `E:\WorkSpace\RenderVerseX\Scripting\Private\Bindings\InputBindings.cpp` |
| ScriptEngine.cpp | `E:\WorkSpace\RenderVerseX\Scripting\Private\ScriptEngine.cpp` |
| InputSubsystem.h | `E:\WorkSpace\RenderVerseX\Runtime\Include\Runtime\Input\InputSubsystem.h` |

---

*Plan created: 2026-03-13*
