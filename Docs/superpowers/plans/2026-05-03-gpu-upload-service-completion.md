# GPU Upload Service Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将当前第一版 `GPUUploadService` 从“集中入口 + Upload 内存直写 + 纹理占位”推进为可验证、可回收、可调度的 GPU 上传系统：支持 buffer/texture 的 staged copy 路径、上传结果与失败原因、资源 ready/failure 状态、帧级 fence 回收，并让 `GPUResourceManager` 只负责资源缓存和驻留状态。

**Architecture:** `GPUResourceManager` 继续作为资源缓存/生命周期门面；`GPUUploadService` 负责上传策略选择、staging 资源、copy 命令提交、pending/completed 状态和统计。RHI 层暴露跨后端 copy/transition/fence 能力；若某后端能力未补齐，服务保留 `ImmediateMapped` fallback，但必须显式记录模式与限制。

**Tech Stack:** C++20, CMake, Render/RHI/Resource/Scene modules, standalone validation executables.

---

## 执行进度 - 2026-05-03

- [x] 已切换到工作分支 `codex/gpu-upload-service-completion`。
- [x] Phase 1 完成：新增 `GPUUploadMode`、`GPUUploadFailureReason`、详细 upload result、failure reason、immediate/staged stats；旧 `UploadBufferData`/`UploadTextureData` 调用面保留为 wrapper。
- [x] Phase 2 完成：texture 上传增加尺寸、mip/array、数据、格式校验；`Unknown`/深度/压缩格式明确返回 `Unsupported`。
- [x] Phase 3 完成：RHI 已存在 `CopyBuffer`、`CopyBufferToTexture`、barrier、command context、submit、fence/staging buffer 抽象，DX11/DX12/Vulkan/OpenGL 后端已有对应 command context 方法。
- [x] Phase 4 完成：buffer 在可用 staging/copy context 时走 `StagedCopy`，创建 Default + CopyDst 目标 buffer，通过 staging buffer 记录 `CopyBuffer`；不支持时回落 `ImmediateMapped`。
- [x] Phase 5 部分完成：基础 uncompressed 2D texture staged copy 已接入，写入 staging buffer 并记录 `CopyBufferToTexture`；高级 mip/array/复杂 footprint 仍保持 fallback，压缩/深度/Unknown 格式仍明确失败。
- [x] Phase 6 完成：staged upload 不再调用 `WaitIdle()`，改为创建 fence 并记录 pending upload；`ProcessCompletedUploads()` 在 fence 完成后释放 staging in-flight，并让 `GPUResourceManager` 从 `Uploading` 转为 `GPUReady`。

已验证：

```bash
cmake --build build\Debug --config Debug --target GPUResourceManagerValidation
build\Debug\Tests\Debug\GPUResourceManagerValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
build\Debug\Tests\Debug\RenderGraphValidation.exe
build\Debug\Tests\Debug\SystemIntegrationTest.exe
```

---

## 当前基线

- [x] `GPUUploadService` 已加入 `Render/Include/Render/GPUUploadService.h` 和 `Render/Private/GPUUploadService.cpp`。
- [x] `GPUResourceManager` 已改为通过 `GPUUploadService` 上传 mesh buffer 与 texture。
- [x] 已有 `GPUResourceManagerValidation` 覆盖 buffer 上传、非法 buffer 上传、重复队列、无 index mesh 失败、有效 mesh ready。
- [x] `RenderGraphValidation` 已扩展 validation stats。
- [x] `SceneRenderer` 已拆出 `RenderPassRegistry` 与 `RenderFrameResourceBinder`。
- [x] 最近通过的验证命令：
  - `cmake --build build\Debug --config Debug --target GPUResourceManagerValidation`
  - `build\Debug\Tests\Debug\GPUResourceManagerValidation.exe`
  - `cmake --build build\Debug --config Debug --target ModelViewer`
  - `build\Debug\Tests\Debug\RenderGraphValidation.exe`
  - `build\Debug\Tests\Debug\SystemIntegrationTest.exe`

---

## 工作原则

- [ ] 每个阶段先加最小失败测试，再实现，再跑对应目标。
- [ ] 不修改无关模块，不清理当前脏工作区里的其他改动。
- [ ] 任何 RHI 接口改动必须同步至少一个 fake-device validation test；触及真实后端时再跑对应 backend validation。
- [ ] `ImmediateMapped` 只能作为 fallback，不能伪装成完整异步上传。
- [ ] texture 上传不再只创建空纹理；即使分阶段落地，也必须在结果与 stats 中标出实际 upload mode。

---

## Phase 1 - 固化上传结果模型

### 目标

让所有上传调用返回可诊断结果，后续 staged copy、fallback、失败原因都能被测试观察。

### 需要修改的文件

- `Render/Include/Render/GPUUploadService.h`
- `Render/Private/GPUUploadService.cpp`
- `Render/Private/GPUResourceManager.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`

### 任务

- [ ] 在 `GPUUploadService.h` 增加：
  - `enum class GPUUploadMode : uint8 { None = 0, ImmediateMapped, StagedCopy };`
  - `enum class GPUUploadFailureReason : uint8 { None = 0, InvalidDevice, InvalidData, InvalidDescription, CreateResourceFailed, MapFailed, CopyFailed, Unsupported };`
  - `template <typename T> struct GPUUploadResult` 或分别为 buffer/texture 定义结果结构，包含 `resource`, `succeeded`, `mode`, `failureReason`, `bytesUploaded`。
- [ ] 保留当前 `UploadBufferData`/`UploadTextureData` 的调用面，或提供过渡 wrapper，避免一次性扩大调用面。
- [ ] `GPUResourceManager` 根据 result 设置 `GPUResourceState::Ready` 或 `GPUResourceState::Failed`。
- [ ] 扩展 stats：
  - `immediateUploadCount`
  - `stagedUploadCount`
  - `failedUploadCount`
  - `bytesUploaded`
- [ ] 在 `GPUResourceManagerValidation` 增加失败原因断言，例如 invalid data 返回 `InvalidData`，create resource failure 返回 `CreateResourceFailed`。

### 验证

```bash
cmake --build build\Debug --config Debug --target GPUResourceManagerValidation
build\Debug\Tests\Debug\GPUResourceManagerValidation.exe
```

预期：新增测试先失败；实现后所有 `GPUResourceManagerValidation` 测试通过。

---

## Phase 2 - 补齐 texture 上传验证

### 目标

让 texture 路径具备明确输入校验，并通过测试覆盖常见错误，避免“创建了纹理但没上传数据”的状态不透明。

### 需要修改的文件

- `Render/Include/Render/GPUUploadService.h`
- `Render/Private/GPUUploadService.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`

### 任务

- [ ] 扩展 `GPUUploadTextureDesc`，确保包含 width、height、depth、format、mipLevels、arrayLayers、data、dataSize、debugName。
- [ ] 增加 texture 校验：
  - width/height/depth 不能为 0。
  - mipLevels 至少为 1。
  - data 非空时 dataSize 必须大于 0。
  - 当前无法准确计算格式 footprint 的格式，返回 `Unsupported` 或走显式 fallback。
- [ ] 增加 texture stats：上传请求数、成功数、失败数、上传字节。
- [ ] 测试：
  - valid texture upload 返回 succeeded。
  - null data + nonzero dimensions 的行为明确：如果允许创建空纹理，mode 为 `None` 或 bytes 为 0；如果上传要求数据，返回 `InvalidData`。
  - zero width/height 返回 `InvalidDescription`。
  - unsupported format footprint 返回 `Unsupported`。

### 验证

```bash
cmake --build build\Debug --config Debug --target GPUResourceManagerValidation
build\Debug\Tests\Debug\GPUResourceManagerValidation.exe
```

---

## Phase 3 - 审计并补齐 RHI copy 能力

### 目标

确认 RHI 是否已有跨后端 copy buffer、copy buffer-to-texture、resource transition、fence/submit 接口。缺口必须先以最小接口补齐，再接入 `GPUUploadService`。

### 需要检查的文件

- `RHI/Include/RHI/RHICommandContext.h`
- `RHI/Include/RHI/RHICommandList.h`
- `RHI/Include/RHI/RHIDevice.h`
- `RHI_DX11/Private/*Command*`
- `RHI_DX12/Private/*Command*`
- `RHI_Vulkan/Private/*Command*`
- `RHI_OpenGL/Private/*Command*`

### 任务

- [ ] 用 `rg` 查找现有能力：
  - `CopyBuffer`
  - `CopyTexture`
  - `BufferToTexture`
  - `Transition`
  - `Fence`
  - `Submit`
- [ ] 如果接口已存在，记录服务应该调用的最小集合。
- [ ] 如果接口缺失，新增最小 RHI 上传接口：
  - `CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst, const RHIBufferCopyRegion& region)`
  - `CopyBufferToTexture(IRHIBuffer* src, IRHITexture* dst, const RHITextureCopyRegion& region)`
  - `TransitionResource(...)` 或复用现有 RenderGraph barrier。
- [ ] fake device/fake command context 先实现可计数的 mock，不要求真实 GPU 行为。
- [ ] 真实后端分批补齐；未补齐后端必须返回 unsupported 或使用 fallback。

### 验证

```bash
cmake --build build\Debug --config Debug --target GPUResourceManagerValidation
cmake --build build\Debug --config Debug --target ModelViewer
```

如果修改真实后端，再追加：

```bash
cmake --build build\Debug --config Debug --target DX11Validation
cmake --build build\Debug --config Debug --target DX12Validation
cmake --build build\Debug --config Debug --target VulkanValidation
```

实际执行时只跑当前平台和已启用 target；不存在的 target 不视为失败。

---

## Phase 4 - 实现 buffer staged copy 路径

### 目标

mesh vertex/index buffer 默认创建为 GPU-local/default 资源，通过 staging upload buffer copy 到目标 buffer；当前 Upload 内存直写只作为 fallback。

### 需要修改的文件

- `Render/Include/Render/GPUUploadService.h`
- `Render/Private/GPUUploadService.cpp`
- `Render/Private/GPUResourceManager.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`
- 必要时修改 RHI copy 接口与后端实现。

### 任务

- [ ] 增加 `GPUUploadService::UploadBufferData` 的策略选择：
  - 如果 device/command queue 支持 staged copy：创建 `Default` dst buffer + `Upload` staging buffer。
  - map staging buffer，复制数据，unmap。
  - 记录 copy 命令。
  - 返回 `GPUUploadMode::StagedCopy`。
  - 如果不支持 staged copy 且目标可 map：使用 `ImmediateMapped` fallback。
- [ ] `GPUResourceManager::UploadMeshToGPU` 继续通过服务上传，但不直接知道 staging 细节。
- [ ] pending upload 初版可以同步完成；必须保留 result/status 字段，供 Phase 6 引入 fence。
- [ ] 测试：
  - fake device 支持 staged copy 时，上传 mode 为 `StagedCopy`。
  - copy 命令被记录一次。
  - dst buffer memory type 是 `Default`，staging buffer 是 `Upload`。
  - fallback path 仍可通过。

### 验证

```bash
cmake --build build\Debug --config Debug --target GPUResourceManagerValidation
build\Debug\Tests\Debug\GPUResourceManagerValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

---

## Phase 5 - 实现 texture staged copy 路径

### 目标

texture 上传具备实际 staging copy，支持基础 2D texture，后续再扩展 mip chain、array/cubemap、compressed formats。

### 需要修改的文件

- `Render/Include/Render/GPUUploadService.h`
- `Render/Private/GPUUploadService.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`
- 必要时修改 RHI texture copy region 定义。

### 任务

- [ ] 增加格式 footprint helper，至少覆盖当前项目常用格式：
  - RGBA8/RGBA8_UNorm
  - BGRA8 如项目已有
  - R32/RG32/RGBA32 float 如项目已有
- [ ] 计算 row pitch、slice pitch、总 staging size。
- [ ] 创建 dst texture 为 GPU-local/default。
- [ ] 创建 staging buffer 并按 footprint 写入。
- [ ] 记录 buffer-to-texture copy。
- [ ] 返回 `GPUUploadMode::StagedCopy` 和准确 bytes。
- [ ] 暂不支持的格式明确返回 `Unsupported`，不静默创建空纹理。
- [ ] 测试：
  - 2D RGBA8 texture copy 成功。
  - row pitch 不匹配时按服务计算的 footprint 写入 staging。
  - unsupported format 返回失败原因。

### 验证

```bash
cmake --build build\Debug --config Debug --target GPUResourceManagerValidation
build\Debug\Tests\Debug\GPUResourceManagerValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

---

## Phase 6 - 引入 pending/completed 与 fence 回收

### 目标

上传服务不再假设提交后立刻完成。它应能跟踪 pending uploads，在帧推进时查询 fence，释放 staging 资源，并更新资源 ready 状态。

### 需要修改的文件

- `Render/Include/Render/GPUUploadService.h`
- `Render/Private/GPUUploadService.cpp`
- `Render/Include/Render/GPUResourceManager.h`
- `Render/Private/GPUResourceManager.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp` 或已有 frame tick 入口。
- `Tests/GPUResourceManagerValidation/main.cpp`

### 任务

- [ ] 增加 `GPUUploadHandle` 或内部 upload id。
- [ ] `GPUUploadService` 维护 pending list：
  - resource id/handle
  - staging resources
  - submitted fence value
  - bytes
  - debugName
- [ ] 增加 `GPUUploadService::ProcessCompletedUploads()`。
- [ ] `GPUResourceManager` 在每帧或显式 update 中调用 upload service，并将 completed resource 标为 `Ready`。
- [ ] 失败上传立即标为 `Failed`，pending 上传标为 `Loading`。
- [ ] 增加 reclaim stats：
  - `pendingUploadCount`
  - `completedUploadCount`
  - `stagingBytesInFlight`
  - `peakStagingBytesInFlight`
- [ ] 测试：
  - fake fence 未完成时 resource 仍为 `Loading`。
  - fake fence 完成后 resource 变为 `Ready`。
  - staging resource 在完成后释放。

### 验证

```bash
cmake --build build\Debug --config Debug --target GPUResourceManagerValidation
build\Debug\Tests\Debug\GPUResourceManagerValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
build\Debug\Tests\Debug\SystemIntegrationTest.exe
```

---

## Phase 7 - 预算、批处理与调度控制

### 目标

避免资源系统一次性制造过多 staging 内存或提交过多 copy 命令，为后续异步资源加载接入留出稳定边界。

### 需要修改的文件

- `Render/Include/Render/GPUUploadService.h`
- `Render/Private/GPUUploadService.cpp`
- `Render/Private/GPUResourceManager.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`

### 任务

- [ ] 增加 `GPUUploadServiceDesc`：
  - `maxStagingBytesInFlight`
  - `maxUploadsPerFrame`
  - `enableImmediateFallback`
  - `enableStagedCopy`
- [ ] 上传请求超过预算时进入 queued 状态，不直接失败。
- [ ] 增加 `PumpUploads()`：按预算启动 queued uploads。
- [ ] `GPUResourceManager` 暴露 `UpdateUploads()` 或并入已有 update。
- [ ] 测试：
  - 超过预算时第二个上传保持 queued/loading。
  - 调用 `ProcessCompletedUploads` 后预算释放，queued 上传启动。
  - 关闭 fallback 且 staged copy unsupported 时返回 `Unsupported`。

### 验证

```bash
cmake --build build\Debug --config Debug --target GPUResourceManagerValidation
build\Debug\Tests\Debug\GPUResourceManagerValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

---

## Phase 8 - 与资源异步加载衔接

### 目标

CPU 侧资源异步加载完成后，不在任意线程直接触碰 GPU device；统一投递到主渲染线程/上传服务 pump。

### 需要修改的文件

- `Resource/Include/Resource/ResourceManager.h`
- `Resource/Private/ResourceManager.cpp`
- `Render/Private/GPUResourceManager.cpp`
- 可能新增或复用 Core job/main-thread dispatch 入口。

### 任务

- [ ] 梳理 `ResourceManager::LoadAsync` 完成 callback 当前线程语义。
- [ ] 定义 CPU resource loaded -> GPU upload request 的线程边界：
  - worker thread 只解析 asset 和生成 CPU 数据。
  - render/main thread 调用 `GPUResourceManager` 和 `GPUUploadService`。
- [ ] 如果已有 main-thread task queue，接入它；如果没有，只在计划范围内新增最小 dispatch 接口。
- [ ] 增加生命周期测试：资源管理器析构后，不执行悬空 callback；上传服务 shutdown 后拒绝新上传。

### 验证

```bash
cmake --build build\Debug --config Debug --target SystemIntegrationTest
build\Debug\Tests\Debug\SystemIntegrationTest.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

---

## Phase 9 - 文档与诊断

### 目标

让未来维护者能看懂上传系统的边界、fallback 和后端支持状态。

### 需要修改的文件

- `Docs/` 下新增或更新 GPU upload 说明文档。
- `Render/Include/Render/GPUUploadService.h`
- `Render/Private/GPUUploadService.cpp`

### 任务

- [ ] 为 public structs/enums 添加简洁 Doxygen。
- [ ] 记录每个后端的 staged copy 支持状态。
- [ ] 增加日志：
  - staged copy unsupported fallback。
  - upload failure reason。
  - staging budget exceeded。
- [ ] 保持日志频率受控，避免每帧刷屏。

### 验证

```bash
cmake --build build\Debug --config Debug --target ModelViewer
```

---

## 最终验证矩阵

在全部阶段完成后运行：

```bash
cmake --build build\Debug --config Debug --target GPUResourceManagerValidation
build\Debug\Tests\Debug\GPUResourceManagerValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
build\Debug\Tests\Debug\RenderGraphValidation.exe
build\Debug\Tests\Debug\SystemIntegrationTest.exe
```

如果本机启用了后端 validation target，再运行：

```bash
cmake --build build\Debug --config Debug --target DX11Validation
cmake --build build\Debug --config Debug --target DX12Validation
cmake --build build\Debug --config Debug --target VulkanValidation
```

---

## Definition of Done

- [ ] Buffer 上传默认走 staged copy，fallback 行为可配置、可观察。
- [ ] Texture 2D 基础上传实际复制像素数据，不再只是创建空纹理。
- [ ] 上传失败有明确 `GPUUploadFailureReason`。
- [ ] 资源状态能区分 `Loading`、`Ready`、`Failed`。
- [ ] Pending upload 通过 fence 完成后释放 staging 资源。
- [ ] 有预算控制，避免无限 staging 内存增长。
- [ ] `GPUResourceManagerValidation` 覆盖成功、失败、fallback、pending、budget。
- [ ] `ModelViewer` 编译通过。
- [ ] `RenderGraphValidation` 和 `SystemIntegrationTest` 通过。
- [ ] Public API 有简洁注释，后端支持状态有文档。

---

## 推荐执行方式

### 方式 A - 分阶段执行

按 Phase 1 到 Phase 9 顺序执行。每个 Phase 完成后立即运行该阶段验证命令，确认通过再进入下一阶段。这是当前项目最稳妥的方式，因为 RHI copy 能力可能在不同后端上不一致。

### 方式 B - 小步合并执行

Phase 1 和 Phase 2 可以合并，先把 result model 与 texture validation 一次性落地。Phase 3 必须独立执行；Phase 4 到 Phase 6 依赖 RHI copy/fence 的审计结果，不能跳过。

### 当前建议

先执行 Phase 1 + Phase 2。它们只会强化 `GPUUploadService` 的 API、stats 和 validation tests，风险低，而且会为后续 staged copy 提供清晰的测试观察点。随后执行 Phase 3，确认 RHI 能力缺口，再决定 Phase 4/5 是直接接真实 RHI，还是先用 fake command context 保护行为。
