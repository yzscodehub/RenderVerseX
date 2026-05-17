# RHI_DX11 后端实现计划

本文档详细描述 DirectX 11 后端实现的设计框架，基于现有 RHI 抽象层和 DX12/OpenGL/Vulkan 后端的最佳实践。

---

## 0. 实施计划总览

### 0.1 阶段划分

| 阶段 | 名称 | 目标 | 预计任务量 |
|------|------|------|------------|
| **Phase 1** | 基础框架 | 设备创建、编译通过 | 8个文件 |
| **Phase 2** | 核心资源 | Buffer/Texture/Shader | 4个功能模块 |
| **Phase 3** | 渲染管线 | Pipeline/DescriptorSet | 3个功能模块 |
| **Phase 4** | 命令执行 | CommandContext/RenderPass | 2个功能模块 |
| **Phase 5** | 呈现系统 | SwapChain/Frame管理 | 2个功能模块 |
| **Phase 6** | 验证测试 | Triangle Sample运行 | 集成测试 |
| **Phase 7** | 完善优化 | StateCache/Debug/性能 | 优化迭代 |

### 0.2 文件结构（最终目标）

```
RHI_DX11/
├── CMakeLists.txt                 ✅ 已存在
├── Include/
│   └── DX11/
│       └── DX11Device.h           ✅ 已存在（需扩展）
└── Private/
    ├── DX11Common.h               🆕 Phase 1
    ├── DX11Conversions.h          🆕 Phase 1
    ├── DX11Debug.h                🆕 Phase 1
    ├── DX11Debug.cpp              🆕 Phase 1
    ├── DX11Device.h               🆕 Phase 1
    ├── DX11Device.cpp             ✅ 已存在（需重写）
    ├── DX11Resources.h            🆕 Phase 2
    ├── DX11Resources.cpp          ✅ 已存在（需重写）
    ├── DX11Pipeline.h             🆕 Phase 3
    ├── DX11Pipeline.cpp           ✅ 已存在（需重写）
    ├── DX11BindingRemapper.h      🆕 Phase 3
    ├── DX11BindingRemapper.cpp    ✅ 已存在（需重写）
    ├── DX11StateCache.h           🆕 Phase 3
    ├── DX11StateCache.cpp         🆕 Phase 3
    ├── DX11CommandContext.h       🆕 Phase 4
    ├── DX11CommandContext.cpp     ✅ 已存在（需重写）
    ├── DX11SwapChain.h            🆕 Phase 5
    └── DX11SwapChain.cpp          ✅ 已存在（需重写）
```

---

## Phase 1: 基础框架（可编译通过）

### 1.1 目标
- 创建所有基础头文件
- DX11Device 能够初始化并查询 Capabilities
- 编译通过，链接成功

### 1.2 任务清单

#### Task 1.1: DX11Common.h
```cpp
// 创建文件: Private/DX11Common.h
// 内容: Windows/D3D11 头文件、ComPtr、日志宏、常量定义
```

**验证**: 能被其他文件 include 无错误

#### Task 1.2: DX11Conversions.h
```cpp
// 创建文件: Private/DX11Conversions.h
// 内容: RHIFormat -> DXGI_FORMAT 转换函数
//       RHI 状态枚举 -> D3D11 状态枚举
```

**验证**: 编译通过，格式转换正确

#### Task 1.3: DX11Debug.h/cpp
```cpp
// 创建文件: Private/DX11Debug.h, Private/DX11Debug.cpp
// 内容: DX11Debug 类（简化版，仅错误处理和日志）
```

**验证**: 能初始化调试系统

#### Task 1.4: DX11Device.h (Private)
```cpp
// 创建文件: Private/DX11Device.h
// 内容: DX11Device 类声明，包含所有 IRHIDevice 接口
```

**验证**: 编译通过

#### Task 1.5: DX11Device.cpp - 初始化
```cpp
// 重写文件: Private/DX11Device.cpp
// 实现:
//   - CreateDX11Device() 工厂函数
//   - Initialize(): DXGI Factory、Adapter 枚举、Device 创建
//   - Shutdown(): 清理
//   - QueryCapabilities(): 填充 RHICapabilities
//   - BeginFrame()/EndFrame(): 空实现
//   - WaitIdle(): glFinish 等效
```

**验证**: 
```cpp
auto device = CreateRHIDevice(RHIBackendType::DX11, desc);
assert(device != nullptr);
assert(device->GetBackendType() == RHIBackendType::DX11);
printf("GPU: %s\n", device->GetCapabilities().adapterName.c_str());
```

#### Task 1.6: 存根实现
```cpp
// 所有其他接口方法返回 nullptr 或空实现
// CreateBuffer() -> nullptr
// CreateTexture() -> nullptr
// CreateCommandContext() -> nullptr
// 等等...
```

**验证**: 编译链接通过，Sample 程序能启动（虽然不能渲染）

### 1.3 Phase 1 完成标准
- [ ] `cmake --build build/Debug --target RVX_RHI_DX11` 成功
- [ ] DX11 设备能创建，GPU 名称能打印
- [ ] Debug Layer 能启用（如果可用）

---

## Phase 2: 核心资源

### 2.1 目标
- 实现 Buffer 创建和 Map/Unmap
- 实现 Texture 创建和视图
- 实现 Sampler
- 实现 Shader 创建

### 2.2 任务清单

#### Task 2.1: DX11Resources.h
```cpp
// 创建文件: Private/DX11Resources.h
// 类声明:
//   - DX11Buffer
//   - DX11Texture  
//   - DX11TextureView
//   - DX11Sampler
//   - DX11Shader
//   - DX11Fence
```

#### Task 2.2: DX11Buffer 实现
```cpp
// 功能:
//   - CreateBuffer(): 创建 ID3D11Buffer
//   - 根据 Usage 创建 SRV/UAV
//   - Map()/Unmap() 支持 Upload/Readback 缓冲
```

**验证**:
```cpp
auto buffer = device->CreateBuffer({
    .size = 1024,
    .usage = RHIBufferUsage::Vertex,
    .memoryType = RHIMemoryType::Upload
});
void* mapped = buffer->Map();
memcpy(mapped, data, 1024);
buffer->Unmap();
```

#### Task 2.3: DX11Texture 实现
```cpp
// 功能:
//   - 支持 Texture2D（最常用）
//   - 根据 Usage 创建 SRV/RTV/DSV/UAV
//   - 暂不实现 Texture1D/3D/Cube
```

**验证**:
```cpp
auto texture = device->CreateTexture({
    .width = 800, .height = 600,
    .format = RHIFormat::RGBA8_UNORM,
    .usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource
});
assert(texture->GetWidth() == 800);
```

#### Task 2.4: DX11TextureView 实现
```cpp
// 功能: 为指定子资源范围创建视图
```

#### Task 2.5: DX11Sampler 实现
```cpp
// 功能: 创建 ID3D11SamplerState
```

#### Task 2.6: DX11Shader 实现
```cpp
// 功能:
//   - 从 DXBC bytecode 创建着色器
//   - 支持 VS/PS（最小集）
//   - 保存 bytecode 供 InputLayout 使用
```

**验证**:
```cpp
auto vs = device->CreateShader({
    .stage = RHIShaderStage::Vertex,
    .bytecode = compiledVS.data(),
    .bytecodeSize = compiledVS.size()
});
```

#### Task 2.7: DX11Fence 实现
```cpp
// 功能:
//   - 尝试 ID3D11Fence (Win10+)
//   - 降级到 Query-based (旧系统)
```

### 2.3 Phase 2 完成标准
- [ ] 能创建各类资源对象
- [ ] Buffer Map/Unmap 工作正常
- [ ] 资源能正确设置调试名称

---

## Phase 3: 渲染管线

### 3.1 目标
- 实现 Pipeline 状态管理
- 实现 DescriptorSet 绑定
- 实现状态缓存

### 3.2 任务清单

#### Task 3.1: DX11StateCache.h/cpp
```cpp
// 内容:
//   - DX11StateObjectCache: 缓存 RasterizerState/BlendState/DepthStencilState/InputLayout
//   - DX11BindingStateCache: 跟踪当前绑定状态
```

#### Task 3.2: DX11BindingRemapper.h/cpp
```cpp
// 内容:
//   - (set, binding) -> DX11 slot 映射逻辑
//   - 槽位分配策略
```

#### Task 3.3: DX11Pipeline.h
```cpp
// 类声明:
//   - DX11DescriptorSetLayout
//   - DX11PipelineLayout
//   - DX11DescriptorSet
//   - DX11GraphicsPipeline
//   - DX11ComputePipeline
```

#### Task 3.4: DX11DescriptorSetLayout 实现
```cpp
// 功能: 存储布局信息，计算槽位映射
```

#### Task 3.5: DX11PipelineLayout 实现
```cpp
// 功能: 
//   - 存储多个 SetLayout 引用
//   - 为 Push Constants 创建 CB
```

#### Task 3.6: DX11DescriptorSet 实现
```cpp
// 功能:
//   - Update(): 缓存资源绑定
//   - Apply(): 绑定到 Context
```

#### Task 3.7: DX11GraphicsPipeline 实现
```cpp
// 功能:
//   - 存储着色器引用
//   - 从 StateCache 获取状态对象
//   - Apply(): 设置所有 Pipeline 状态
```

### 3.3 Phase 3 完成标准
- [ ] 能创建 Pipeline 对象
- [ ] 能创建和更新 DescriptorSet
- [ ] 状态缓存减少冗余设置

---

## Phase 4: 命令执行

### 4.1 目标
- 实现 CommandContext
- 实现 RenderPass
- 实现 Draw/Dispatch 命令

### 4.2 任务清单

#### Task 4.1: DX11CommandContext.h
```cpp
// 内容: DX11CommandContext 类声明
```

#### Task 4.2: 生命周期实现
```cpp
// Begin()/End()/Reset()
// 使用 Immediate Context
```

#### Task 4.3: RenderPass 实现
```cpp
// BeginRenderPass():
//   - 绑定 RTV/DSV
//   - 执行 Clear（根据 LoadOp）
// EndRenderPass():
//   - 解绑 RTV/DSV（可选）
```

#### Task 4.4: 绑定命令实现
```cpp
// SetPipeline()
// SetVertexBuffer()/SetIndexBuffer()
// SetDescriptorSet()
// SetViewport()/SetScissor()
```

#### Task 4.5: Draw 命令实现
```cpp
// Draw()
// DrawIndexed()
```

#### Task 4.6: Barrier 实现（空操作）
```cpp
// BufferBarrier() -> no-op
// TextureBarrier() -> no-op
```

### 4.3 Phase 4 完成标准
- [ ] 能录制完整的渲染命令
- [ ] SetPipeline 正确应用状态

---

## Phase 5: 呈现系统

### 5.1 目标
- 实现 SwapChain
- 实现完整的帧循环

### 5.2 任务清单

#### Task 5.1: DX11SwapChain.h/cpp
```cpp
// 内容:
//   - 创建 IDXGISwapChain
//   - 管理 BackBuffer 纹理/视图
//   - Present()
//   - Resize()
```

#### Task 5.2: Frame 管理完善
```cpp
// BeginFrame(): 重置状态
// EndFrame(): 可选 Flush
// SubmitCommandContext(): 处理 Fence
```

### 5.3 Phase 5 完成标准
- [ ] SwapChain 能创建
- [ ] Present 工作正常
- [ ] Resize 不崩溃

---

## Phase 6: 验证测试

### 6.1 目标
- Triangle Sample 能运行
- 输出正确的三角形

### 6.2 任务清单

#### Task 6.1: 运行 Triangle Sample
```bash
./build/Debug/Samples/Triangle/Triangle.exe --backend=dx11
```

**预期结果**: 窗口显示彩色三角形

#### Task 6.2: 调试问题
- 使用 DX11 Debug Layer 检查错误
- 使用 PIX/RenderDoc 捕获帧

#### Task 6.3: 运行其他 Samples
- TexturedQuad
- Cube3D

### 6.3 Phase 6 完成标准
- [ ] Triangle 渲染正确
- [ ] TexturedQuad 渲染正确
- [ ] Cube3D 渲染正确
- [ ] 无 Debug Layer 错误

---

## Phase 7: 完善优化

### 7.1 目标
- 完整的调试支持
- 性能优化
- 边缘情况处理

### 7.2 任务清单

#### Task 7.1: 完善调试系统
- 资源跟踪
- 操作日志
- Device Removed 诊断

#### Task 7.2: Deferred Context 支持（可选）
- 多线程命令录制

#### Task 7.3: ComputePipeline 实现
- Dispatch 命令
- UAV 绑定

#### Task 7.4: Copy 命令实现
- CopyBuffer
- CopyTexture
- CopyBufferToTexture

#### Task 7.5: 性能优化
- 减少状态切换
- 优化资源创建

### 7.3 Phase 7 完成标准
- [ ] ComputeDemo 运行正确
- [ ] 所有测试通过
- [ ] 无内存泄漏

---

## 0.3 快速开始命令

```bash
# 1. 配置项目
cmake --preset debug

# 2. 仅编译 DX11 模块（快速验证）
cmake --build build/Debug --target RVX_RHI_DX11

# 3. 编译 Triangle Sample
cmake --build build/Debug --target Triangle

# 4. 运行测试
./build/Debug/Samples/Triangle/Triangle.exe --backend=dx11
```

---

## 1. 架构概览

### 1.1 设计原则

1. **统一抽象**: 完全遵循 `IRHIDevice` 接口规范，与其他后端保持一致的使用方式
2. **性能优先**: 利用 DX11 的立即模式上下文特性，减少不必要的抽象开销
3. **状态缓存**: 实现高效的状态缓存机制，避免重复设置相同状态
4. **资源池化**: 对常用资源（如 Constant Buffer）实现池化管理
5. **优雅降级**: 对于 DX11 不支持的特性（如 Heap/Placed Resources），提供合理的降级实现

### 1.2 DX11 vs 现代 API 差异

| 特性 | DX12/Vulkan | DX11 | DX11 实现策略 |
|------|-------------|------|---------------|
| 资源状态管理 | 显式 Barrier | 自动（驱动管理） | Barrier 操作为空实现 |
| Command Buffer | 显式录制/提交 | 立即模式 + Deferred Context | 支持两种模式 |
| Descriptor Set | Descriptor Heap/Pool | 直接绑定槽位 | BindingRemapper 映射 |
| Pipeline State | PSO | 分离状态对象 | 状态缓存 + 惰性设置 |
| Fence | GPU Timeline Fence | ID3D11Fence (Win10+) / Query | 版本检测 + 降级 |
| Heap/Placed Resources | 支持 | 不支持 | 返回 nullptr / 创建独立资源 |
| Async Compute | 专用队列 | 单队列 | 串行执行 |

### 1.3 文件结构

```
RHI_DX11/
├── CMakeLists.txt
├── Include/
│   └── DX11/
│       └── DX11Device.h           # 公共头文件
└── Private/
    ├── DX11Common.h               # 公共定义、宏、类型
    ├── DX11Conversions.h          # RHI -> D3D11 格式转换
    ├── DX11Device.h               # 设备实现声明
    ├── DX11Device.cpp             # 设备实现
    ├── DX11Resources.h            # Buffer/Texture/Sampler/Shader
    ├── DX11Resources.cpp
    ├── DX11Pipeline.h             # Pipeline/DescriptorSet/Layout
    ├── DX11Pipeline.cpp
    ├── DX11CommandContext.h       # CommandContext 实现
    ├── DX11CommandContext.cpp
    ├── DX11SwapChain.h
    ├── DX11SwapChain.cpp
    ├── DX11StateCache.h           # 状态缓存系统
    ├── DX11StateCache.cpp
    └── DX11BindingRemapper.h/cpp  # Binding -> Slot 映射
```

---

## 2. 调试与错误诊断系统

DX11 确实有调试层（D3D11 Debug Layer），虽然不如 DX12/Vulkan 的验证层详细，但结合自定义错误跟踪可以实现高效的问题定位。

### 2.1 DX11 调试层支持

```cpp
// DX11Debug.h
#pragma once

#include "DX11Common.h"
#include <dxgidebug.h>
#include <source_location>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>

namespace RVX
{
    // =============================================================================
    // HRESULT 错误处理 - 核心宏
    // =============================================================================
    
    // 将 HRESULT 转换为可读字符串
    inline std::string HRESULTToString(HRESULT hr)
    {
        switch (hr)
        {
            case S_OK:                              return "S_OK";
            case S_FALSE:                           return "S_FALSE";
            case E_FAIL:                            return "E_FAIL";
            case E_INVALIDARG:                      return "E_INVALIDARG";
            case E_OUTOFMEMORY:                     return "E_OUTOFMEMORY";
            case E_NOTIMPL:                         return "E_NOTIMPL";
            case E_NOINTERFACE:                     return "E_NOINTERFACE";
            case E_POINTER:                         return "E_POINTER";
            case E_ABORT:                           return "E_ABORT";
            case E_ACCESSDENIED:                    return "E_ACCESSDENIED";
            
            // DXGI errors
            case DXGI_ERROR_DEVICE_HUNG:            return "DXGI_ERROR_DEVICE_HUNG";
            case DXGI_ERROR_DEVICE_REMOVED:         return "DXGI_ERROR_DEVICE_REMOVED";
            case DXGI_ERROR_DEVICE_RESET:           return "DXGI_ERROR_DEVICE_RESET";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR:  return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
            case DXGI_ERROR_FRAME_STATISTICS_DISJOINT: return "DXGI_ERROR_FRAME_STATISTICS_DISJOINT";
            case DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE: return "DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE";
            case DXGI_ERROR_INVALID_CALL:           return "DXGI_ERROR_INVALID_CALL";
            case DXGI_ERROR_MORE_DATA:              return "DXGI_ERROR_MORE_DATA";
            case DXGI_ERROR_NONEXCLUSIVE:           return "DXGI_ERROR_NONEXCLUSIVE";
            case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
            case DXGI_ERROR_NOT_FOUND:              return "DXGI_ERROR_NOT_FOUND";
            case DXGI_ERROR_UNSUPPORTED:            return "DXGI_ERROR_UNSUPPORTED";
            case DXGI_ERROR_ACCESS_LOST:            return "DXGI_ERROR_ACCESS_LOST";
            case DXGI_ERROR_WAIT_TIMEOUT:           return "DXGI_ERROR_WAIT_TIMEOUT";
            case DXGI_ERROR_SESSION_DISCONNECTED:   return "DXGI_ERROR_SESSION_DISCONNECTED";
            case DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE: return "DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE";
            case DXGI_ERROR_CANNOT_PROTECT_CONTENT: return "DXGI_ERROR_CANNOT_PROTECT_CONTENT";
            case DXGI_ERROR_ACCESS_DENIED:          return "DXGI_ERROR_ACCESS_DENIED";
            case DXGI_ERROR_NAME_ALREADY_EXISTS:    return "DXGI_ERROR_NAME_ALREADY_EXISTS";
            
            // D3D11 specific errors
            case D3D11_ERROR_FILE_NOT_FOUND:        return "D3D11_ERROR_FILE_NOT_FOUND";
            case D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS: return "D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS";
            case D3D11_ERROR_TOO_MANY_UNIQUE_VIEW_OBJECTS:  return "D3D11_ERROR_TOO_MANY_UNIQUE_VIEW_OBJECTS";
            case D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD: 
                return "D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD";
            
            default:
            {
                // Format message from system
                char buffer[256];
                if (FormatMessageA(
                    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    nullptr, hr, MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
                    buffer, sizeof(buffer), nullptr) > 0)
                {
                    return buffer;
                }
                return "Unknown HRESULT: 0x" + std::format("{:08X}", static_cast<uint32>(hr));
            }
        }
    }

    // =============================================================================
    // 带位置信息的错误检查宏
    // =============================================================================
    
    // 详细错误信息结构
    struct DX11ErrorContext
    {
        HRESULT hr;
        const char* operation;
        const char* file;
        int line;
        const char* function;
        std::string additionalInfo;
    };

    // 错误处理回调类型
    using DX11ErrorCallback = std::function<void(const DX11ErrorContext&)>;

    // 全局错误处理器
    class DX11ErrorHandler
    {
    public:
        static DX11ErrorHandler& Get()
        {
            static DX11ErrorHandler instance;
            return instance;
        }

        void SetCallback(DX11ErrorCallback callback) { m_callback = callback; }
        
        void HandleError(const DX11ErrorContext& ctx)
        {
            // 记录到日志
            RVX_RHI_ERROR("DX11 Error in '{}': {} (0x{:08X})",
                ctx.operation, HRESULTToString(ctx.hr), static_cast<uint32>(ctx.hr));
            RVX_RHI_ERROR("  Location: {}:{} in {}",
                ctx.file, ctx.line, ctx.function);
            
            if (!ctx.additionalInfo.empty())
            {
                RVX_RHI_ERROR("  Info: {}", ctx.additionalInfo);
            }

            // 调用自定义回调
            if (m_callback)
            {
                m_callback(ctx);
            }

            // 触发断点（调试模式）
            #ifdef RVX_DEBUG
            if (IsDebuggerPresent())
            {
                __debugbreak();
            }
            #endif
        }

    private:
        DX11ErrorCallback m_callback;
    };

    // 增强的错误检查宏
    #define DX11_CHECK(hr) \
        do { \
            HRESULT _hr = (hr); \
            if (FAILED(_hr)) { \
                DX11ErrorContext ctx; \
                ctx.hr = _hr; \
                ctx.operation = #hr; \
                ctx.file = __FILE__; \
                ctx.line = __LINE__; \
                ctx.function = __FUNCTION__; \
                DX11ErrorHandler::Get().HandleError(ctx); \
            } \
        } while(0)

    // 带额外信息的错误检查
    #define DX11_CHECK_INFO(hr, info) \
        do { \
            HRESULT _hr = (hr); \
            if (FAILED(_hr)) { \
                DX11ErrorContext ctx; \
                ctx.hr = _hr; \
                ctx.operation = #hr; \
                ctx.file = __FILE__; \
                ctx.line = __LINE__; \
                ctx.function = __FUNCTION__; \
                ctx.additionalInfo = info; \
                DX11ErrorHandler::Get().HandleError(ctx); \
            } \
        } while(0)

    // 带返回值的错误检查
    #define DX11_CHECK_RETURN(hr, retval) \
        do { \
            HRESULT _hr = (hr); \
            if (FAILED(_hr)) { \
                DX11ErrorContext ctx; \
                ctx.hr = _hr; \
                ctx.operation = #hr; \
                ctx.file = __FILE__; \
                ctx.line = __LINE__; \
                ctx.function = __FUNCTION__; \
                DX11ErrorHandler::Get().HandleError(ctx); \
                return retval; \
            } \
        } while(0)

    // 带 nullptr 返回的错误检查（常用于资源创建）
    #define DX11_CHECK_NULLPTR(hr) DX11_CHECK_RETURN(hr, nullptr)

    // =============================================================================
    // 资源类型（用于跟踪）
    // =============================================================================
    enum class DX11ResourceType : uint8
    {
        Buffer,
        Texture1D,
        Texture2D,
        Texture3D,
        ShaderResourceView,
        UnorderedAccessView,
        RenderTargetView,
        DepthStencilView,
        VertexShader,
        PixelShader,
        GeometryShader,
        HullShader,
        DomainShader,
        ComputeShader,
        InputLayout,
        BlendState,
        RasterizerState,
        DepthStencilState,
        SamplerState,
        Query,
        Predicate,
        CommandList,
        DeferredContext,
        Unknown
    };

    inline const char* ToString(DX11ResourceType type)
    {
        switch (type)
        {
            case DX11ResourceType::Buffer:             return "Buffer";
            case DX11ResourceType::Texture1D:          return "Texture1D";
            case DX11ResourceType::Texture2D:          return "Texture2D";
            case DX11ResourceType::Texture3D:          return "Texture3D";
            case DX11ResourceType::ShaderResourceView: return "SRV";
            case DX11ResourceType::UnorderedAccessView: return "UAV";
            case DX11ResourceType::RenderTargetView:   return "RTV";
            case DX11ResourceType::DepthStencilView:   return "DSV";
            case DX11ResourceType::VertexShader:       return "VertexShader";
            case DX11ResourceType::PixelShader:        return "PixelShader";
            case DX11ResourceType::GeometryShader:     return "GeometryShader";
            case DX11ResourceType::HullShader:         return "HullShader";
            case DX11ResourceType::DomainShader:       return "DomainShader";
            case DX11ResourceType::ComputeShader:      return "ComputeShader";
            case DX11ResourceType::InputLayout:        return "InputLayout";
            case DX11ResourceType::BlendState:         return "BlendState";
            case DX11ResourceType::RasterizerState:    return "RasterizerState";
            case DX11ResourceType::DepthStencilState:  return "DepthStencilState";
            case DX11ResourceType::SamplerState:       return "SamplerState";
            case DX11ResourceType::Query:              return "Query";
            case DX11ResourceType::Predicate:          return "Predicate";
            case DX11ResourceType::CommandList:        return "CommandList";
            case DX11ResourceType::DeferredContext:    return "DeferredContext";
            default:                                   return "Unknown";
        }
    }

    // =============================================================================
    // 资源跟踪信息
    // =============================================================================
    struct DX11ResourceInfo
    {
        void* handle = nullptr;
        DX11ResourceType type = DX11ResourceType::Unknown;
        std::string debugName;
        std::string creationFile;
        int creationLine = 0;
        std::string creationFunction;
        uint64 creationFrame = 0;
        uint64 size = 0;
        std::string description;  // 额外描述信息
    };

    // =============================================================================
    // 调试统计
    // =============================================================================
    struct DX11DebugStats
    {
        // 每帧计数器
        std::atomic<uint32> drawCalls{0};
        std::atomic<uint32> drawIndexedCalls{0};
        std::atomic<uint32> dispatchCalls{0};
        std::atomic<uint32> copyBufferCalls{0};
        std::atomic<uint32> copyTextureCalls{0};
        std::atomic<uint32> updateSubresourceCalls{0};
        std::atomic<uint32> mapCalls{0};
        std::atomic<uint32> unmapCalls{0};
        
        // 状态变更计数
        std::atomic<uint32> pipelineStateChanges{0};
        std::atomic<uint32> vertexBufferBinds{0};
        std::atomic<uint32> indexBufferBinds{0};
        std::atomic<uint32> constantBufferBinds{0};
        std::atomic<uint32> srvBinds{0};
        std::atomic<uint32> uavBinds{0};
        std::atomic<uint32> samplerBinds{0};
        std::atomic<uint32> rtvBinds{0};
        std::atomic<uint32> dsvBinds{0};
        
        // 累积计数器
        std::atomic<uint32> buffersCreated{0};
        std::atomic<uint32> buffersDestroyed{0};
        std::atomic<uint32> texturesCreated{0};
        std::atomic<uint32> texturesDestroyed{0};
        std::atomic<uint32> shadersCreated{0};
        std::atomic<uint32> shadersDestroyed{0};
        std::atomic<uint64> totalBufferMemory{0};
        std::atomic<uint64> totalTextureMemory{0};
        
        // 错误计数
        std::atomic<uint32> errorCount{0};
        std::atomic<uint32> warningCount{0};

        void ResetFrameCounters()
        {
            drawCalls = 0;
            drawIndexedCalls = 0;
            dispatchCalls = 0;
            copyBufferCalls = 0;
            copyTextureCalls = 0;
            updateSubresourceCalls = 0;
            mapCalls = 0;
            unmapCalls = 0;
            pipelineStateChanges = 0;
            vertexBufferBinds = 0;
            indexBufferBinds = 0;
            constantBufferBinds = 0;
            srvBinds = 0;
            uavBinds = 0;
            samplerBinds = 0;
            rtvBinds = 0;
            dsvBinds = 0;
        }
    };

    // =============================================================================
    // DX11 调试系统
    // =============================================================================
    class DX11Debug
    {
    public:
        static DX11Debug& Get()
        {
            static DX11Debug instance;
            return instance;
        }

        // =========================================================================
        // 初始化
        // =========================================================================
        bool Initialize(ID3D11Device* device, bool enableDebugLayer);
        void Shutdown();

        // =========================================================================
        // 帧管理
        // =========================================================================
        void BeginFrame(uint64 frameIndex);
        void EndFrame();

        // =========================================================================
        // InfoQueue 消息处理
        // =========================================================================
        void ProcessInfoQueueMessages();
        void SetBreakOnError(bool enable);
        void SetBreakOnWarning(bool enable);
        
        // 过滤特定消息
        void AddMessageFilter(D3D11_MESSAGE_ID messageId);
        void RemoveMessageFilter(D3D11_MESSAGE_ID messageId);

        // =========================================================================
        // 资源跟踪
        // =========================================================================
        void TrackResource(void* handle, DX11ResourceType type, const char* debugName,
                          const char* file, int line, const char* function);
        void UntrackResource(void* handle, DX11ResourceType type);
        void SetResourceSize(void* handle, DX11ResourceType type, uint64 size);
        void SetResourceDescription(void* handle, DX11ResourceType type, const std::string& desc);
        const DX11ResourceInfo* GetResourceInfo(void* handle) const;

        // =========================================================================
        // 调试名称（PIX/RenderDoc 可见）
        // =========================================================================
        void SetDebugName(ID3D11DeviceChild* resource, const char* name);
        void SetBufferName(ID3D11Buffer* buffer, const char* name);
        void SetTextureName(ID3D11Resource* texture, const char* name);
        void SetViewName(ID3D11View* view, const char* name);
        void SetShaderName(ID3D11DeviceChild* shader, const char* name);
        void SetStateName(ID3D11DeviceChild* state, const char* name);

        // =========================================================================
        // 调试标记（PIX/RenderDoc 可见）
        // =========================================================================
        void BeginEvent(ID3D11DeviceContext* context, const wchar_t* name, uint32 color = 0);
        void EndEvent(ID3D11DeviceContext* context);
        void SetMarker(ID3D11DeviceContext* context, const wchar_t* name, uint32 color = 0);

        // =========================================================================
        // 验证助手
        // =========================================================================
        bool ValidateBuffer(ID3D11Buffer* buffer, const char* operation);
        bool ValidateTexture(ID3D11Resource* texture, const char* operation);
        bool ValidateShader(ID3D11DeviceChild* shader, const char* operation);
        bool ValidateRenderTarget(ID3D11RenderTargetView* rtv, const char* operation);
        bool ValidateDepthStencil(ID3D11DepthStencilView* dsv, const char* operation);

        // =========================================================================
        // Device Removed 诊断
        // =========================================================================
        void DiagnoseDeviceRemoved(ID3D11Device* device);
        std::string GetDeviceRemovedReason(ID3D11Device* device);

        // =========================================================================
        // 统计
        // =========================================================================
        DX11DebugStats& GetStats() { return m_stats; }
        const DX11DebugStats& GetStats() const { return m_stats; }
        void LogFrameStats();
        
        // =========================================================================
        // 资源泄漏检测
        // =========================================================================
        void DumpTrackedResources();
        void ReportLiveObjects();  // 使用 ID3D11Debug::ReportLiveDeviceObjects

        // =========================================================================
        // 状态
        // =========================================================================
        bool IsDebugEnabled() const { return m_debugEnabled; }
        uint64 GetCurrentFrame() const { return m_currentFrame; }

    private:
        DX11Debug() = default;
        ~DX11Debug() = default;

        bool m_debugEnabled = false;
        uint64 m_currentFrame = 0;

        // D3D11 Debug interfaces
        ComPtr<ID3D11Debug> m_d3dDebug;
        ComPtr<ID3D11InfoQueue> m_infoQueue;
        ComPtr<IDXGIInfoQueue> m_dxgiInfoQueue;

        // 用于 PIX/RenderDoc 的用户定义标注
        typedef HRESULT(WINAPI* PFN_D3DPERF_BeginEvent)(DWORD, LPCWSTR);
        typedef HRESULT(WINAPI* PFN_D3DPERF_EndEvent)(void);
        typedef HRESULT(WINAPI* PFN_D3DPERF_SetMarker)(DWORD, LPCWSTR);
        PFN_D3DPERF_BeginEvent m_pfnBeginEvent = nullptr;
        PFN_D3DPERF_EndEvent m_pfnEndEvent = nullptr;
        PFN_D3DPERF_SetMarker m_pfnSetMarker = nullptr;

        // 资源跟踪
        mutable std::mutex m_resourceMutex;
        std::unordered_map<void*, DX11ResourceInfo> m_trackedResources;

        DX11DebugStats m_stats;
    };

    // =============================================================================
    // 调试宏
    // =============================================================================

#ifdef RVX_DX11_DEBUG
    // 资源跟踪
    #define DX11_DEBUG_TRACK(handle, type, name) \
        DX11Debug::Get().TrackResource(handle, type, name, __FILE__, __LINE__, __FUNCTION__)
    
    #define DX11_DEBUG_UNTRACK(handle, type) \
        DX11Debug::Get().UntrackResource(handle, type)

    // 设置调试名称
    #define DX11_DEBUG_NAME(resource, name) \
        DX11Debug::Get().SetDebugName(resource, name)

    // 验证资源
    #define DX11_DEBUG_VALIDATE_BUFFER(buffer, op) \
        if (!DX11Debug::Get().ValidateBuffer(buffer, op)) return

    #define DX11_DEBUG_VALIDATE_TEXTURE(texture, op) \
        if (!DX11Debug::Get().ValidateTexture(texture, op)) return

    // 统计
    #define DX11_DEBUG_STAT_INC(stat) \
        ++DX11Debug::Get().GetStats().stat

    // 调试标记
    #define DX11_DEBUG_EVENT_BEGIN(ctx, name) \
        DX11Debug::Get().BeginEvent(ctx, L##name)
    
    #define DX11_DEBUG_EVENT_END(ctx) \
        DX11Debug::Get().EndEvent(ctx)
    
    #define DX11_DEBUG_MARKER(ctx, name) \
        DX11Debug::Get().SetMarker(ctx, L##name)

    // RAII 作用域标记
    class DX11DebugScope
    {
    public:
        DX11DebugScope(ID3D11DeviceContext* ctx, const wchar_t* name) : m_ctx(ctx)
        {
            DX11Debug::Get().BeginEvent(ctx, name);
        }
        ~DX11DebugScope() { DX11Debug::Get().EndEvent(m_ctx); }
    private:
        ID3D11DeviceContext* m_ctx;
    };
    #define DX11_DEBUG_SCOPE(ctx, name) \
        DX11DebugScope _dx11DebugScope##__LINE__(ctx, L##name)

#else
    // Release 模式：空操作
    #define DX11_DEBUG_TRACK(handle, type, name) ((void)0)
    #define DX11_DEBUG_UNTRACK(handle, type) ((void)0)
    #define DX11_DEBUG_NAME(resource, name) ((void)0)
    #define DX11_DEBUG_VALIDATE_BUFFER(buffer, op) ((void)0)
    #define DX11_DEBUG_VALIDATE_TEXTURE(texture, op) ((void)0)
    #define DX11_DEBUG_STAT_INC(stat) ((void)0)
    #define DX11_DEBUG_EVENT_BEGIN(ctx, name) ((void)0)
    #define DX11_DEBUG_EVENT_END(ctx) ((void)0)
    #define DX11_DEBUG_MARKER(ctx, name) ((void)0)
    #define DX11_DEBUG_SCOPE(ctx, name) ((void)0)
#endif

} // namespace RVX
```

### 2.2 DX11Debug 实现

```cpp
// DX11Debug.cpp
#include "DX11Debug.h"
#include <d3d9.h>  // For D3DPERF_* functions

namespace RVX
{
    bool DX11Debug::Initialize(ID3D11Device* device, bool enableDebugLayer)
    {
        m_debugEnabled = enableDebugLayer;
        
        if (!enableDebugLayer)
        {
            RVX_RHI_INFO("DX11 Debug System: disabled");
            return true;
        }

        // 获取 ID3D11Debug 接口
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&m_d3dDebug));
        if (FAILED(hr))
        {
            RVX_RHI_WARN("Failed to get ID3D11Debug interface");
        }

        // 获取 ID3D11InfoQueue 接口
        hr = device->QueryInterface(IID_PPV_ARGS(&m_infoQueue));
        if (SUCCEEDED(hr))
        {
            // 启用错误断点
            m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
            
            // 可选：过滤某些消息
            D3D11_MESSAGE_ID hide[] =
            {
                D3D11_MESSAGE_ID_SETPRIVATEDATA_CHANGINGPARAMS,
                // 根据需要添加更多要过滤的消息
            };
            
            D3D11_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumIDs = _countof(hide);
            filter.DenyList.pIDList = hide;
            m_infoQueue->AddStorageFilterEntries(&filter);
            
            RVX_RHI_INFO("DX11 InfoQueue initialized");
        }
        else
        {
            RVX_RHI_WARN("Failed to get ID3D11InfoQueue interface");
        }

        // 加载 PIX/D3D9 性能标记函数
        HMODULE d3d9Module = GetModuleHandleW(L"d3d9.dll");
        if (!d3d9Module)
        {
            d3d9Module = LoadLibraryW(L"d3d9.dll");
        }
        
        if (d3d9Module)
        {
            m_pfnBeginEvent = (PFN_D3DPERF_BeginEvent)GetProcAddress(d3d9Module, "D3DPERF_BeginEvent");
            m_pfnEndEvent = (PFN_D3DPERF_EndEvent)GetProcAddress(d3d9Module, "D3DPERF_EndEvent");
            m_pfnSetMarker = (PFN_D3DPERF_SetMarker)GetProcAddress(d3d9Module, "D3DPERF_SetMarker");
            
            if (m_pfnBeginEvent && m_pfnEndEvent)
            {
                RVX_RHI_INFO("PIX/RenderDoc event markers available");
            }
        }

        RVX_RHI_INFO("DX11 Debug System initialized");
        return true;
    }

    void DX11Debug::Shutdown()
    {
        // 报告泄漏
        if (m_debugEnabled && !m_trackedResources.empty())
        {
            RVX_RHI_WARN("DX11 Debug: {} resources still tracked at shutdown (potential leaks):",
                        m_trackedResources.size());
            DumpTrackedResources();
        }

        // 使用 D3D11 Debug 报告活动对象
        if (m_d3dDebug)
        {
            RVX_RHI_INFO("DX11 Debug: Reporting live device objects...");
            m_d3dDebug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
        }

        m_trackedResources.clear();
        m_infoQueue.Reset();
        m_d3dDebug.Reset();
    }

    void DX11Debug::BeginFrame(uint64 frameIndex)
    {
        m_currentFrame = frameIndex;
        m_stats.ResetFrameCounters();
    }

    void DX11Debug::EndFrame()
    {
        if (m_debugEnabled)
        {
            ProcessInfoQueueMessages();
        }
    }

    void DX11Debug::ProcessInfoQueueMessages()
    {
        if (!m_infoQueue) return;

        UINT64 messageCount = m_infoQueue->GetNumStoredMessages();
        
        for (UINT64 i = 0; i < messageCount; ++i)
        {
            SIZE_T messageLength = 0;
            m_infoQueue->GetMessage(i, nullptr, &messageLength);
            
            if (messageLength == 0) continue;

            std::vector<char> messageData(messageLength);
            D3D11_MESSAGE* message = reinterpret_cast<D3D11_MESSAGE*>(messageData.data());
            
            if (SUCCEEDED(m_infoQueue->GetMessage(i, message, &messageLength)))
            {
                // 根据严重程度记录
                switch (message->Severity)
                {
                    case D3D11_MESSAGE_SEVERITY_CORRUPTION:
                        RVX_RHI_ERROR("[DX11 CORRUPTION] {}", message->pDescription);
                        ++m_stats.errorCount;
                        break;
                    case D3D11_MESSAGE_SEVERITY_ERROR:
                        RVX_RHI_ERROR("[DX11 ERROR] {}", message->pDescription);
                        ++m_stats.errorCount;
                        break;
                    case D3D11_MESSAGE_SEVERITY_WARNING:
                        RVX_RHI_WARN("[DX11 WARNING] {}", message->pDescription);
                        ++m_stats.warningCount;
                        break;
                    case D3D11_MESSAGE_SEVERITY_INFO:
                        RVX_RHI_DEBUG("[DX11 INFO] {}", message->pDescription);
                        break;
                    case D3D11_MESSAGE_SEVERITY_MESSAGE:
                        RVX_RHI_DEBUG("[DX11 MSG] {}", message->pDescription);
                        break;
                }
            }
        }

        m_infoQueue->ClearStoredMessages();
    }

    void DX11Debug::DiagnoseDeviceRemoved(ID3D11Device* device)
    {
        HRESULT reason = device->GetDeviceRemovedReason();
        
        RVX_RHI_ERROR("=== DX11 DEVICE REMOVED DIAGNOSTIC ===");
        RVX_RHI_ERROR("Reason: {} (0x{:08X})", GetDeviceRemovedReason(device), static_cast<uint32>(reason));
        
        // 详细诊断信息
        switch (reason)
        {
            case DXGI_ERROR_DEVICE_HUNG:
                RVX_RHI_ERROR("  GPU hung - possible infinite loop in shader or excessive workload");
                RVX_RHI_ERROR("  Check: Long-running shaders, excessive draw calls, compute dispatch");
                break;
            case DXGI_ERROR_DEVICE_REMOVED:
                RVX_RHI_ERROR("  GPU physically removed or disabled");
                RVX_RHI_ERROR("  Check: GPU driver update, multi-GPU system, power management");
                break;
            case DXGI_ERROR_DEVICE_RESET:
                RVX_RHI_ERROR("  GPU reset by driver/OS");
                RVX_RHI_ERROR("  Check: TDR (Timeout Detection and Recovery) triggered");
                break;
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
                RVX_RHI_ERROR("  Driver internal error");
                RVX_RHI_ERROR("  Check: Update GPU drivers, check for known issues");
                break;
            case DXGI_ERROR_INVALID_CALL:
                RVX_RHI_ERROR("  Invalid API call");
                RVX_RHI_ERROR("  Check: Resource state, invalid parameters");
                break;
            default:
                RVX_RHI_ERROR("  Unknown device removed reason");
                break;
        }

        // 输出最后的操作统计
        RVX_RHI_ERROR("Last frame stats before crash:");
        RVX_RHI_ERROR("  Draw calls: {} + {} indexed", 
            m_stats.drawCalls.load(), m_stats.drawIndexedCalls.load());
        RVX_RHI_ERROR("  Dispatch calls: {}", m_stats.dispatchCalls.load());
        RVX_RHI_ERROR("  Total errors/warnings: {}/{}", 
            m_stats.errorCount.load(), m_stats.warningCount.load());

        RVX_RHI_ERROR("=== END DIAGNOSTIC ===");
    }

    std::string DX11Debug::GetDeviceRemovedReason(ID3D11Device* device)
    {
        HRESULT reason = device->GetDeviceRemovedReason();
        return HRESULTToString(reason);
    }

    void DX11Debug::SetDebugName(ID3D11DeviceChild* resource, const char* name)
    {
        if (!resource || !name) return;
        
        resource->SetPrivateData(WKPDID_D3DDebugObjectName,
            static_cast<UINT>(strlen(name)), name);
    }

    void DX11Debug::BeginEvent(ID3D11DeviceContext* context, const wchar_t* name, uint32 color)
    {
        (void)context;  // DX11 没有 context 级别的标记，使用全局 D3DPERF
        if (m_pfnBeginEvent)
        {
            m_pfnBeginEvent(color, name);
        }
    }

    void DX11Debug::EndEvent(ID3D11DeviceContext* context)
    {
        (void)context;
        if (m_pfnEndEvent)
        {
            m_pfnEndEvent();
        }
    }

    void DX11Debug::SetMarker(ID3D11DeviceContext* context, const wchar_t* name, uint32 color)
    {
        (void)context;
        if (m_pfnSetMarker)
        {
            m_pfnSetMarker(color, name);
        }
    }

    void DX11Debug::TrackResource(void* handle, DX11ResourceType type, const char* debugName,
                                  const char* file, int line, const char* function)
    {
        if (!m_debugEnabled || !handle) return;

        std::lock_guard<std::mutex> lock(m_resourceMutex);
        
        DX11ResourceInfo info;
        info.handle = handle;
        info.type = type;
        info.debugName = debugName ? debugName : "";
        info.creationFile = file;
        info.creationLine = line;
        info.creationFunction = function;
        info.creationFrame = m_currentFrame;
        
        m_trackedResources[handle] = info;

        RVX_RHI_DEBUG("DX11 Resource Created: {} '{}' at {}:{} (frame {})",
                     ToString(type), info.debugName, file, line, m_currentFrame);
    }

    void DX11Debug::UntrackResource(void* handle, DX11ResourceType type)
    {
        if (!m_debugEnabled || !handle) return;

        std::lock_guard<std::mutex> lock(m_resourceMutex);
        
        auto it = m_trackedResources.find(handle);
        if (it != m_trackedResources.end())
        {
            RVX_RHI_DEBUG("DX11 Resource Destroyed: {} '{}' (lived {} frames)",
                         ToString(type), it->second.debugName,
                         m_currentFrame - it->second.creationFrame);
            m_trackedResources.erase(it);
        }
        else
        {
            RVX_RHI_WARN("DX11 Resource Destroyed but not tracked: {} @ {:p}",
                        ToString(type), handle);
        }
    }

    void DX11Debug::DumpTrackedResources()
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        
        for (const auto& [handle, info] : m_trackedResources)
        {
            RVX_RHI_WARN("  - {} '{}' @ {:p}",
                        ToString(info.type), info.debugName, handle);
            RVX_RHI_WARN("    Created at {}:{} ({}) frame {}",
                        info.creationFile, info.creationLine,
                        info.creationFunction, info.creationFrame);
            if (info.size > 0)
            {
                RVX_RHI_WARN("    Size: {} bytes", info.size);
            }
            if (!info.description.empty())
            {
                RVX_RHI_WARN("    Description: {}", info.description);
            }
        }
    }

    void DX11Debug::ReportLiveObjects()
    {
        if (m_d3dDebug)
        {
            m_d3dDebug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
        }
    }

    void DX11Debug::LogFrameStats()
    {
        RVX_RHI_INFO("=== DX11 Frame {} Stats ===", m_currentFrame);
        RVX_RHI_INFO("  Draw: {} + {} indexed", 
            m_stats.drawCalls.load(), m_stats.drawIndexedCalls.load());
        RVX_RHI_INFO("  Dispatch: {}", m_stats.dispatchCalls.load());
        RVX_RHI_INFO("  State Changes: {}", m_stats.pipelineStateChanges.load());
        RVX_RHI_INFO("  Binds: VB={} IB={} CB={} SRV={} UAV={} Sampler={}",
            m_stats.vertexBufferBinds.load(),
            m_stats.indexBufferBinds.load(),
            m_stats.constantBufferBinds.load(),
            m_stats.srvBinds.load(),
            m_stats.uavBinds.load(),
            m_stats.samplerBinds.load());
        RVX_RHI_INFO("  Errors: {} Warnings: {}",
            m_stats.errorCount.load(), m_stats.warningCount.load());
    }

} // namespace RVX
```

### 2.3 使用示例

```cpp
// 资源创建时的跟踪
RHIBufferRef DX11Device::CreateBuffer(const RHIBufferDesc& desc)
{
    ComPtr<ID3D11Buffer> buffer;
    
    D3D11_BUFFER_DESC d3dDesc = {};
    // ... 填充描述符 ...
    
    DX11_CHECK_NULLPTR(m_device->CreateBuffer(&d3dDesc, nullptr, &buffer));
    
    // 设置调试名称
    DX11_DEBUG_NAME(buffer.Get(), desc.debugName);
    
    // 跟踪资源
    DX11_DEBUG_TRACK(buffer.Get(), DX11ResourceType::Buffer, desc.debugName);
    DX11Debug::Get().SetResourceSize(buffer.Get(), DX11ResourceType::Buffer, desc.size);
    
    // 统计
    DX11_DEBUG_STAT_INC(buffersCreated);
    
    return Ref<DX11Buffer>(new DX11Buffer(this, buffer, desc));
}

// 渲染时的标记和统计
void DX11CommandContext::Draw(uint32 vertexCount, uint32 instanceCount,
                              uint32 firstVertex, uint32 firstInstance)
{
    DX11_DEBUG_SCOPE(m_context.Get(), "Draw");
    DX11_DEBUG_STAT_INC(drawCalls);
    
    m_context->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

// Device Removed 处理
void DX11Device::HandleDeviceRemoved()
{
    RVX_RHI_ERROR("Device Removed detected!");
    DX11Debug::Get().DiagnoseDeviceRemoved(m_device.Get());
    
    // 可以尝试重建设备或通知应用程序
}
```

### 2.4 启用调试层的方法

```cpp
bool DX11Device::CreateDevice(bool enableDebugLayer)
{
    UINT createDeviceFlags = 0;
    
    if (enableDebugLayer)
    {
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
        RVX_RHI_INFO("DX11 Debug Layer enabled");
    }

    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT hr = D3D11CreateDevice(
        m_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        createDeviceFlags,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        &m_device,
        &m_featureLevel,
        &m_immediateContext
    );

    if (FAILED(hr) && (createDeviceFlags & D3D11_CREATE_DEVICE_DEBUG))
    {
        // 调试层可能不可用，尝试不带调试层创建
        RVX_RHI_WARN("Failed to create device with debug layer, retrying without...");
        createDeviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        
        hr = D3D11CreateDevice(/* ... same params but without debug flag ... */);
    }

    DX11_CHECK_RETURN(hr, false);

    // 初始化调试系统
    DX11Debug::Get().Initialize(m_device.Get(), enableDebugLayer);

    return true;
}
```

---

## 3. 核心组件设计

### 2.1 DX11Common.h - 公共定义

```cpp
#pragma once

// Windows headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// DirectX 11 headers
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "Core/Types.h"
#include "Core/Log.h"

namespace RVX
{
    using Microsoft::WRL::ComPtr;

    // Debug helpers
    #define DX11_CHECK(hr) \
        do { \
            HRESULT _hr = (hr); \
            if (FAILED(_hr)) { \
                RVX_RHI_ERROR("DX11 Error: 0x{:08X} at {}:{}", \
                    static_cast<uint32>(_hr), __FILE__, __LINE__); \
            } \
        } while(0)

    #ifdef RVX_DEBUG
        #define DX11_SET_DEBUG_NAME(obj, name) \
            if (obj && name) { obj->SetPrivateData(WKPDID_D3DDebugObjectName, \
                static_cast<UINT>(strlen(name)), name); }
    #else
        #define DX11_SET_DEBUG_NAME(obj, name)
    #endif

    // Feature level constants
    constexpr D3D_FEATURE_LEVEL MIN_FEATURE_LEVEL = D3D_FEATURE_LEVEL_11_0;
    constexpr UINT MAX_CONSTANT_BUFFER_SIZE = 65536;  // 64KB (DX11 limit)
    
    // DX11 binding limits
    constexpr uint32 DX11_MAX_VS_CBUFFERS = 14;
    constexpr uint32 DX11_MAX_PS_CBUFFERS = 14;
    constexpr uint32 DX11_MAX_CS_CBUFFERS = 14;
    constexpr uint32 DX11_MAX_TEXTURE_SLOTS = 128;
    constexpr uint32 DX11_MAX_SAMPLER_SLOTS = 16;
    constexpr uint32 DX11_MAX_UAV_SLOTS = 8;  // PS UAV limit (11.0)
    constexpr uint32 DX11_MAX_UAV_SLOTS_11_1 = 64;  // 11.1 feature level

} // namespace RVX
```

### 2.2 DX11Device - 设备实现

```cpp
// DX11Device.h
#pragma once

#include "DX11Common.h"
#include "DX11StateCache.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHICapabilities.h"

namespace RVX
{
    class DX11Device : public IRHIDevice
    {
    public:
        DX11Device();
        ~DX11Device() override;

        bool Initialize(const RHIDeviceDesc& desc);
        void Shutdown();

        // =========================================================================
        // IRHIDevice Implementation
        // =========================================================================
        
        // Resource Creation
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override;
        RHITextureRef CreateTexture(const RHITextureDesc& desc) override;
        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc) override;
        RHISamplerRef CreateSampler(const RHISamplerDesc& desc) override;
        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override;

        // Heap Management (DX11: 模拟实现)
        RHIHeapRef CreateHeap(const RHIHeapDesc& desc) override;
        RHITextureRef CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc) override;
        RHIBufferRef CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc) override;
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc& desc) override;
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) override;

        // Pipeline Creation
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) override;
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override;
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override;
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) override;

        // Descriptor Set
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override;

        // Command Context
        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override;
        void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override;
        void SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence) override;

        // SwapChain
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) override;

        // Synchronization
        RHIFenceRef CreateFence(uint64 initialValue) override;
        void WaitForFence(RHIFence* fence, uint64 value) override;
        void WaitIdle() override;

        // Frame Management
        void BeginFrame() override;
        void EndFrame() override;
        uint32 GetCurrentFrameIndex() const override { return m_frameIndex; }

        // Capabilities
        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::DX11; }

        // =========================================================================
        // DX11 Specific Accessors
        // =========================================================================
        ID3D11Device* GetD3DDevice() const { return m_device.Get(); }
        ID3D11Device5* GetD3DDevice5() const { return m_device5.Get(); }
        ID3D11DeviceContext* GetImmediateContext() const { return m_immediateContext.Get(); }
        ID3D11DeviceContext4* GetImmediateContext4() const { return m_immediateContext4.Get(); }
        IDXGIFactory2* GetDXGIFactory() const { return m_factory.Get(); }
        
        DX11StateCache& GetStateCache() { return m_stateCache; }
        D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_featureLevel; }

        // Threading mode support
        bool SupportsDeferredContext() const { return m_capabilities.dx11.supportsDeferredContext; }
        DX11ThreadingMode GetThreadingMode() const { return m_threadingMode; }
        void SetThreadingMode(DX11ThreadingMode mode) { m_threadingMode = mode; }

        // Create deferred context for multi-threaded command recording
        ComPtr<ID3D11DeviceContext> CreateDeferredContext();

    private:
        bool CreateFactory();
        bool SelectAdapter(uint32 preferredIndex);
        bool CreateDevice(bool enableDebugLayer);
        void QueryCapabilities();
        void EnableDebugLayer();

        // DXGI/D3D11 Core Objects
        ComPtr<IDXGIFactory2> m_factory;
        ComPtr<IDXGIAdapter1> m_adapter;
        ComPtr<ID3D11Device> m_device;
        ComPtr<ID3D11Device5> m_device5;  // Optional: for ID3D11Fence support
        ComPtr<ID3D11DeviceContext> m_immediateContext;
        ComPtr<ID3D11DeviceContext4> m_immediateContext4;  // Optional: for advanced features

        D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
        DX11ThreadingMode m_threadingMode = DX11ThreadingMode::SingleThreaded;

        // State cache
        DX11StateCache m_stateCache;

        // Frame management
        uint32 m_frameIndex = 0;

        // Capabilities
        RHICapabilities m_capabilities;
        bool m_debugLayerEnabled = false;
    };

} // namespace RVX
```

### 2.3 DX11StateCache - 状态缓存系统

DX11 不同于现代 API，需要通过状态缓存避免冗余状态切换：

```cpp
// DX11StateCache.h
#pragma once

#include "DX11Common.h"
#include "RHI/RHIPipeline.h"
#include <unordered_map>
#include <array>

namespace RVX
{
    // =============================================================================
    // State Object Cache - 缓存已创建的状态对象
    // =============================================================================
    
    // Hash for RasterizerState
    struct RasterizerStateHash
    {
        size_t operator()(const RHIRasterizerState& state) const;
    };
    
    // Hash for DepthStencilState
    struct DepthStencilStateHash
    {
        size_t operator()(const RHIDepthStencilState& state) const;
    };
    
    // Hash for BlendState
    struct BlendStateHash
    {
        size_t operator()(const RHIBlendState& state) const;
    };

    // =============================================================================
    // DX11 State Object Cache
    // =============================================================================
    class DX11StateObjectCache
    {
    public:
        void Initialize(ID3D11Device* device);
        void Shutdown();

        ID3D11RasterizerState* GetRasterizerState(const RHIRasterizerState& desc);
        ID3D11DepthStencilState* GetDepthStencilState(const RHIDepthStencilState& desc);
        ID3D11BlendState* GetBlendState(const RHIBlendState& desc);
        ID3D11InputLayout* GetInputLayout(const RHIInputLayoutDesc& desc, 
                                          const void* vsBlob, size_t vsBlobSize);

    private:
        ID3D11Device* m_device = nullptr;

        std::unordered_map<size_t, ComPtr<ID3D11RasterizerState>> m_rasterizerStates;
        std::unordered_map<size_t, ComPtr<ID3D11DepthStencilState>> m_depthStencilStates;
        std::unordered_map<size_t, ComPtr<ID3D11BlendState>> m_blendStates;
        std::unordered_map<size_t, ComPtr<ID3D11InputLayout>> m_inputLayouts;
    };

    // =============================================================================
    // DX11 Binding State Cache - 跟踪当前绑定状态
    // =============================================================================
    class DX11BindingStateCache
    {
    public:
        void Reset();

        // Shader stage binding tracking
        struct StageBindings
        {
            std::array<ID3D11Buffer*, DX11_MAX_VS_CBUFFERS> constantBuffers = {};
            std::array<ID3D11ShaderResourceView*, DX11_MAX_TEXTURE_SLOTS> srvs = {};
            std::array<ID3D11SamplerState*, DX11_MAX_SAMPLER_SLOTS> samplers = {};
        };

        // Check if binding changed (returns true if changed)
        bool SetConstantBuffer(RHIShaderStage stage, uint32 slot, ID3D11Buffer* buffer);
        bool SetSRV(RHIShaderStage stage, uint32 slot, ID3D11ShaderResourceView* srv);
        bool SetSampler(RHIShaderStage stage, uint32 slot, ID3D11SamplerState* sampler);
        bool SetUAV(uint32 slot, ID3D11UnorderedAccessView* uav);

        // Pipeline state tracking
        ID3D11VertexShader* currentVS = nullptr;
        ID3D11PixelShader* currentPS = nullptr;
        ID3D11GeometryShader* currentGS = nullptr;
        ID3D11HullShader* currentHS = nullptr;
        ID3D11DomainShader* currentDS = nullptr;
        ID3D11ComputeShader* currentCS = nullptr;

        ID3D11RasterizerState* currentRasterizerState = nullptr;
        ID3D11DepthStencilState* currentDepthStencilState = nullptr;
        ID3D11BlendState* currentBlendState = nullptr;
        ID3D11InputLayout* currentInputLayout = nullptr;

        D3D11_PRIMITIVE_TOPOLOGY currentTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        uint32 currentStencilRef = 0;
        float currentBlendFactor[4] = {1, 1, 1, 1};

        // Render target tracking
        std::array<ID3D11RenderTargetView*, 8> currentRTVs = {};
        ID3D11DepthStencilView* currentDSV = nullptr;
        uint32 currentRTVCount = 0;

        // Viewport/Scissor tracking
        D3D11_VIEWPORT currentViewport = {};
        D3D11_RECT currentScissor = {};

        // Vertex/Index buffer tracking
        ID3D11Buffer* currentVertexBuffers[16] = {};
        UINT currentVertexStrides[16] = {};
        UINT currentVertexOffsets[16] = {};
        ID3D11Buffer* currentIndexBuffer = nullptr;
        DXGI_FORMAT currentIndexFormat = DXGI_FORMAT_UNKNOWN;
        UINT currentIndexOffset = 0;

    private:
        std::array<StageBindings, 6> m_stageBindings;  // VS, HS, DS, GS, PS, CS
        std::array<ID3D11UnorderedAccessView*, DX11_MAX_UAV_SLOTS_11_1> m_uavs = {};
    };

    // =============================================================================
    // DX11 State Cache - 主状态缓存类
    // =============================================================================
    class DX11StateCache
    {
    public:
        void Initialize(ID3D11Device* device);
        void Shutdown();

        DX11StateObjectCache& GetObjectCache() { return m_objectCache; }
        DX11BindingStateCache& GetBindingCache() { return m_bindingCache; }

    private:
        DX11StateObjectCache m_objectCache;
        DX11BindingStateCache m_bindingCache;
    };

} // namespace RVX
```

### 2.4 DX11Resources - 资源实现

```cpp
// DX11Resources.h
#pragma once

#include "DX11Common.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"
#include "RHI/RHISampler.h"
#include "RHI/RHIShader.h"
#include "RHI/RHISynchronization.h"

namespace RVX
{
    class DX11Device;

    // =============================================================================
    // DX11 Buffer
    // =============================================================================
    class DX11Buffer : public RHIBuffer
    {
    public:
        DX11Buffer(DX11Device* device, const RHIBufferDesc& desc);
        ~DX11Buffer() override;

        // RHIBuffer interface
        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override;
        void Unmap() override;

        // DX11 Specific
        ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }
        ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
        ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }

    private:
        void CreateViews();

        DX11Device* m_device = nullptr;
        RHIBufferDesc m_desc;

        ComPtr<ID3D11Buffer> m_buffer;
        ComPtr<ID3D11ShaderResourceView> m_srv;
        ComPtr<ID3D11UnorderedAccessView> m_uav;
    };

    // =============================================================================
    // DX11 Texture
    // =============================================================================
    class DX11Texture : public RHITexture
    {
    public:
        DX11Texture(DX11Device* device, const RHITextureDesc& desc);
        DX11Texture(DX11Device* device, ComPtr<ID3D11Texture2D> texture, const RHITextureDesc& desc); // SwapChain
        ~DX11Texture() override;

        // RHITexture interface
        uint32 GetWidth() const override { return m_desc.width; }
        uint32 GetHeight() const override { return m_desc.height; }
        uint32 GetDepth() const override { return m_desc.depth; }
        uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        uint32 GetArraySize() const override { return m_desc.arraySize; }
        RHIFormat GetFormat() const override { return m_desc.format; }
        RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

        // DX11 Specific
        ID3D11Resource* GetResource() const { return m_resource.Get(); }
        ID3D11Texture1D* GetTexture1D() const;
        ID3D11Texture2D* GetTexture2D() const;
        ID3D11Texture3D* GetTexture3D() const;
        DXGI_FORMAT GetDXGIFormat() const { return m_dxgiFormat; }

        ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
        ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }
        ID3D11RenderTargetView* GetRTV(uint32 index = 0) const;
        ID3D11DepthStencilView* GetDSV() const { return m_dsv.Get(); }

    private:
        void CreateViews();

        DX11Device* m_device = nullptr;
        RHITextureDesc m_desc;
        DXGI_FORMAT m_dxgiFormat = DXGI_FORMAT_UNKNOWN;
        bool m_ownsResource = true;

        ComPtr<ID3D11Resource> m_resource;
        ComPtr<ID3D11ShaderResourceView> m_srv;
        ComPtr<ID3D11UnorderedAccessView> m_uav;
        std::vector<ComPtr<ID3D11RenderTargetView>> m_rtvs;
        ComPtr<ID3D11DepthStencilView> m_dsv;
    };

    // =============================================================================
    // DX11 Texture View
    // =============================================================================
    class DX11TextureView : public RHITextureView
    {
    public:
        DX11TextureView(DX11Device* device, RHITexture* texture, const RHITextureViewDesc& desc);
        ~DX11TextureView() override;

        // RHITextureView interface
        RHITexture* GetTexture() const override { return m_texture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_subresourceRange; }

        // DX11 Specific
        ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
        ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }
        ID3D11RenderTargetView* GetRTV() const { return m_rtv.Get(); }
        ID3D11DepthStencilView* GetDSV() const { return m_dsv.Get(); }

    private:
        DX11Device* m_device = nullptr;
        RHITexture* m_texture = nullptr;
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_subresourceRange;

        ComPtr<ID3D11ShaderResourceView> m_srv;
        ComPtr<ID3D11UnorderedAccessView> m_uav;
        ComPtr<ID3D11RenderTargetView> m_rtv;
        ComPtr<ID3D11DepthStencilView> m_dsv;
    };

    // =============================================================================
    // DX11 Sampler
    // =============================================================================
    class DX11Sampler : public RHISampler
    {
    public:
        DX11Sampler(DX11Device* device, const RHISamplerDesc& desc);
        ~DX11Sampler() override;

        ID3D11SamplerState* GetSampler() const { return m_sampler.Get(); }

    private:
        ComPtr<ID3D11SamplerState> m_sampler;
    };

    // =============================================================================
    // DX11 Shader
    // =============================================================================
    class DX11Shader : public RHIShader
    {
    public:
        DX11Shader(DX11Device* device, const RHIShaderDesc& desc);
        ~DX11Shader() override;

        // RHIShader interface
        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }

        // DX11 Specific
        ID3D11VertexShader* GetVertexShader() const { return m_vertexShader.Get(); }
        ID3D11PixelShader* GetPixelShader() const { return m_pixelShader.Get(); }
        ID3D11GeometryShader* GetGeometryShader() const { return m_geometryShader.Get(); }
        ID3D11HullShader* GetHullShader() const { return m_hullShader.Get(); }
        ID3D11DomainShader* GetDomainShader() const { return m_domainShader.Get(); }
        ID3D11ComputeShader* GetComputeShader() const { return m_computeShader.Get(); }

    private:
        RHIShaderStage m_stage = RHIShaderStage::None;
        std::vector<uint8> m_bytecode;

        ComPtr<ID3D11VertexShader> m_vertexShader;
        ComPtr<ID3D11PixelShader> m_pixelShader;
        ComPtr<ID3D11GeometryShader> m_geometryShader;
        ComPtr<ID3D11HullShader> m_hullShader;
        ComPtr<ID3D11DomainShader> m_domainShader;
        ComPtr<ID3D11ComputeShader> m_computeShader;
    };

    // =============================================================================
    // DX11 Fence (模拟实现)
    // =============================================================================
    class DX11Fence : public RHIFence
    {
    public:
        DX11Fence(DX11Device* device, uint64 initialValue);
        ~DX11Fence() override;

        // RHIFence interface
        uint64 GetCompletedValue() const override;
        void Signal(uint64 value) override;
        void Wait(uint64 value, uint64 timeoutNs) override;

        // DX11 Specific (Win10+ only)
        ID3D11Fence* GetFence() const { return m_fence.Get(); }
        bool HasNativeFence() const { return m_fence != nullptr; }

    private:
        DX11Device* m_device = nullptr;
        uint64 m_value = 0;
        
        // Win10+ native fence (optional)
        ComPtr<ID3D11Fence> m_fence;
        HANDLE m_event = nullptr;

        // Legacy fallback (Win7/8) using queries
        ComPtr<ID3D11Query> m_query;
    };

} // namespace RVX
```

### 2.5 DX11Pipeline - Pipeline 和 Descriptor 实现

```cpp
// DX11Pipeline.h
#pragma once

#include "DX11Common.h"
#include "DX11Resources.h"
#include "DX11BindingRemapper.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIDescriptor.h"

namespace RVX
{
    class DX11Device;

    // =============================================================================
    // DX11 Descriptor Set Layout
    // =============================================================================
    // DX11 没有真正的 Descriptor Set Layout，但我们需要存储布局信息
    // 以便在绑定时正确映射 RHI binding -> DX11 slot
    class DX11DescriptorSetLayout : public RHIDescriptorSetLayout
    {
    public:
        DX11DescriptorSetLayout(DX11Device* device, const RHIDescriptorSetLayoutDesc& desc);
        ~DX11DescriptorSetLayout() override;

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const { return m_entries; }
        
        // Binding remapping info
        struct SlotMapping
        {
            uint32 cbSlot = UINT32_MAX;    // Constant buffer slot
            uint32 srvSlot = UINT32_MAX;   // Shader resource view slot  
            uint32 uavSlot = UINT32_MAX;   // Unordered access view slot
            uint32 samplerSlot = UINT32_MAX;
        };
        
        const SlotMapping& GetSlotMapping(uint32 binding) const;

    private:
        std::vector<RHIBindingLayoutEntry> m_entries;
        std::unordered_map<uint32, SlotMapping> m_slotMappings;
    };

    // =============================================================================
    // DX11 Pipeline Layout
    // =============================================================================
    class DX11PipelineLayout : public RHIPipelineLayout
    {
    public:
        DX11PipelineLayout(DX11Device* device, const RHIPipelineLayoutDesc& desc);
        ~DX11PipelineLayout() override;

        const std::vector<RHIDescriptorSetLayout*>& GetSetLayouts() const { return m_setLayouts; }
        uint32 GetPushConstantSize() const { return m_pushConstantSize; }

        // Push constant buffer (DX11: 实现为 constant buffer)
        ID3D11Buffer* GetPushConstantBuffer() const { return m_pushConstantBuffer.Get(); }

    private:
        std::vector<RHIDescriptorSetLayout*> m_setLayouts;
        uint32 m_pushConstantSize = 0;
        ComPtr<ID3D11Buffer> m_pushConstantBuffer;
    };

    // =============================================================================
    // DX11 Descriptor Set
    // =============================================================================
    class DX11DescriptorSet : public RHIDescriptorSet
    {
    public:
        DX11DescriptorSet(DX11Device* device, const RHIDescriptorSetDesc& desc);
        ~DX11DescriptorSet() override;

        // RHIDescriptorSet interface
        void Update(const std::vector<RHIDescriptorBinding>& bindings) override;

        // DX11 Specific - apply bindings to context
        void Apply(ID3D11DeviceContext* context, RHIShaderStage stages) const;

    private:
        DX11Device* m_device = nullptr;
        DX11DescriptorSetLayout* m_layout = nullptr;

        // Cached bindings
        struct Binding
        {
            RHIBindingType type;
            uint32 binding;
            
            // Resource references
            ID3D11Buffer* buffer = nullptr;
            uint64 bufferOffset = 0;
            uint64 bufferRange = 0;
            ID3D11ShaderResourceView* srv = nullptr;
            ID3D11UnorderedAccessView* uav = nullptr;
            ID3D11SamplerState* sampler = nullptr;
        };
        
        std::vector<Binding> m_bindings;
    };

    // =============================================================================
    // DX11 Graphics Pipeline
    // =============================================================================
    class DX11GraphicsPipeline : public RHIPipeline
    {
    public:
        DX11GraphicsPipeline(DX11Device* device, const RHIGraphicsPipelineDesc& desc);
        ~DX11GraphicsPipeline() override;

        bool IsCompute() const override { return false; }

        // Apply pipeline state to context
        void Apply(ID3D11DeviceContext* context, DX11StateCache& stateCache) const;

        // Accessors
        DX11Shader* GetVertexShader() const { return m_vertexShader; }
        DX11Shader* GetPixelShader() const { return m_pixelShader; }
        DX11Shader* GetGeometryShader() const { return m_geometryShader; }
        DX11Shader* GetHullShader() const { return m_hullShader; }
        DX11Shader* GetDomainShader() const { return m_domainShader; }
        DX11PipelineLayout* GetLayout() const { return m_layout; }

    private:
        DX11Device* m_device = nullptr;
        DX11PipelineLayout* m_layout = nullptr;

        // Shader references (owned by user)
        DX11Shader* m_vertexShader = nullptr;
        DX11Shader* m_pixelShader = nullptr;
        DX11Shader* m_geometryShader = nullptr;
        DX11Shader* m_hullShader = nullptr;
        DX11Shader* m_domainShader = nullptr;

        // State objects (cached in StateCache, just store config)
        RHIRasterizerState m_rasterizerDesc;
        RHIDepthStencilState m_depthStencilDesc;
        RHIBlendState m_blendDesc;
        RHIInputLayoutDesc m_inputLayoutDesc;
        D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

        // Cached state objects (from StateCache)
        ID3D11RasterizerState* m_rasterizerState = nullptr;
        ID3D11DepthStencilState* m_depthStencilState = nullptr;
        ID3D11BlendState* m_blendState = nullptr;
        ID3D11InputLayout* m_inputLayout = nullptr;
    };

    // =============================================================================
    // DX11 Compute Pipeline
    // =============================================================================
    class DX11ComputePipeline : public RHIPipeline
    {
    public:
        DX11ComputePipeline(DX11Device* device, const RHIComputePipelineDesc& desc);
        ~DX11ComputePipeline() override;

        bool IsCompute() const override { return true; }

        void Apply(ID3D11DeviceContext* context) const;

        DX11Shader* GetComputeShader() const { return m_computeShader; }
        DX11PipelineLayout* GetLayout() const { return m_layout; }

    private:
        DX11Device* m_device = nullptr;
        DX11PipelineLayout* m_layout = nullptr;
        DX11Shader* m_computeShader = nullptr;
    };

} // namespace RVX
```

### 2.6 DX11CommandContext - 命令上下文

```cpp
// DX11CommandContext.h
#pragma once

#include "DX11Common.h"
#include "RHI/RHICommandContext.h"
#include <mutex>

namespace RVX
{
    class DX11Device;
    class DX11GraphicsPipeline;
    class DX11ComputePipeline;

    // =============================================================================
    // DX11 Command Context
    // =============================================================================
    // DX11 支持两种模式:
    // 1. Immediate Context - 直接执行命令
    // 2. Deferred Context - 录制命令列表，稍后执行
    class DX11CommandContext : public RHICommandContext
    {
    public:
        DX11CommandContext(DX11Device* device, RHICommandQueueType type);
        ~DX11CommandContext() override;

        // =========================================================================
        // Lifecycle
        // =========================================================================
        void Begin() override;
        void End() override;
        void Reset() override;

        // =========================================================================
        // Debug Markers
        // =========================================================================
        void BeginEvent(const char* name, uint32 color) override;
        void EndEvent() override;
        void SetMarker(const char* name, uint32 color) override;

        // =========================================================================
        // Resource Barriers (DX11: No-op, driver handles automatically)
        // =========================================================================
        void BufferBarrier(const RHIBufferBarrier& barrier) override;
        void TextureBarrier(const RHITextureBarrier& barrier) override;
        void Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                      std::span<const RHITextureBarrier> textureBarriers) override;

        // =========================================================================
        // Render Pass
        // =========================================================================
        void BeginRenderPass(const RHIRenderPassDesc& desc) override;
        void EndRenderPass() override;

        // =========================================================================
        // Pipeline Binding
        // =========================================================================
        void SetPipeline(RHIPipeline* pipeline) override;

        // =========================================================================
        // Vertex/Index Buffers
        // =========================================================================
        void SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset) override;
        void SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, 
                              std::span<const uint64> offsets) override;
        void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset) override;

        // =========================================================================
        // Descriptor Sets
        // =========================================================================
        void SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, 
                              std::span<const uint32> dynamicOffsets) override;
        void SetPushConstants(const void* data, uint32 size, uint32 offset) override;

        // =========================================================================
        // Viewport/Scissor
        // =========================================================================
        void SetViewport(const RHIViewport& viewport) override;
        void SetViewports(std::span<const RHIViewport> viewports) override;
        void SetScissor(const RHIRect& scissor) override;
        void SetScissors(std::span<const RHIRect> scissors) override;

        // =========================================================================
        // Draw Commands
        // =========================================================================
        void Draw(uint32 vertexCount, uint32 instanceCount, 
                  uint32 firstVertex, uint32 firstInstance) override;
        void DrawIndexed(uint32 indexCount, uint32 instanceCount, 
                         uint32 firstIndex, int32 vertexOffset, uint32 firstInstance) override;
        void DrawIndirect(RHIBuffer* buffer, uint64 offset, 
                          uint32 drawCount, uint32 stride) override;
        void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, 
                                 uint32 drawCount, uint32 stride) override;

        // =========================================================================
        // Compute Commands
        // =========================================================================
        void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override;
        void DispatchIndirect(RHIBuffer* buffer, uint64 offset) override;

        // =========================================================================
        // Copy Commands
        // =========================================================================
        void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, 
                        uint64 srcOffset, uint64 dstOffset, uint64 size) override;
        void CopyTexture(RHITexture* src, RHITexture* dst, 
                         const RHITextureCopyDesc& desc) override;
        void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, 
                                 const RHIBufferTextureCopyDesc& desc) override;
        void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, 
                                 const RHIBufferTextureCopyDesc& desc) override;

        // =========================================================================
        // DX11 Specific
        // =========================================================================
        ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
        ID3D11CommandList* GetCommandList() const { return m_commandList.Get(); }
        bool IsDeferred() const { return m_isDeferred; }

    private:
        void FlushState();
        void UnbindRenderTargets();
        void ApplyPendingDescriptorSets();

        DX11Device* m_device = nullptr;
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;

        // Context (immediate or deferred)
        ComPtr<ID3D11DeviceContext> m_context;
        ComPtr<ID3D11CommandList> m_commandList;  // For deferred context
        bool m_isDeferred = false;
        bool m_isRecording = false;
        bool m_inRenderPass = false;

        // Current state
        DX11GraphicsPipeline* m_currentGraphicsPipeline = nullptr;
        DX11ComputePipeline* m_currentComputePipeline = nullptr;

        // Pending descriptor sets
        std::array<RHIDescriptorSet*, 4> m_pendingDescriptorSets = {};
        std::array<std::vector<uint32>, 4> m_pendingDynamicOffsets;

        // Render pass state
        std::array<ID3D11RenderTargetView*, RVX_MAX_RENDER_TARGETS> m_currentRTVs = {};
        ID3D11DepthStencilView* m_currentDSV = nullptr;
        uint32 m_currentRTVCount = 0;
    };

} // namespace RVX
```

### 2.7 DX11SwapChain

```cpp
// DX11SwapChain.h
#pragma once

#include "DX11Common.h"
#include "DX11Resources.h"
#include "RHI/RHISwapChain.h"

namespace RVX
{
    class DX11Device;

    class DX11SwapChain : public RHISwapChain
    {
    public:
        DX11SwapChain(DX11Device* device, const RHISwapChainDesc& desc);
        ~DX11SwapChain() override;

        // RHISwapChain interface
        RHITexture* GetCurrentBackBuffer() override;
        RHITextureView* GetCurrentBackBufferView() override;
        uint32 GetCurrentBackBufferIndex() const override;

        void Present() override;
        void Resize(uint32 width, uint32 height) override;

        uint32 GetWidth() const override { return m_width; }
        uint32 GetHeight() const override { return m_height; }
        RHIFormat GetFormat() const override { return m_format; }
        uint32 GetBufferCount() const override { return m_bufferCount; }

        // DX11 Specific
        IDXGISwapChain1* GetSwapChain() const { return m_swapChain.Get(); }

    private:
        void CreateBackBufferResources();
        void ReleaseBackBufferResources();

        DX11Device* m_device = nullptr;

        ComPtr<IDXGISwapChain1> m_swapChain;
        ComPtr<IDXGISwapChain4> m_swapChain4;  // For HDR support

        uint32 m_width = 0;
        uint32 m_height = 0;
        RHIFormat m_format = RHIFormat::BGRA8_UNORM;
        uint32 m_bufferCount = 2;
        bool m_vsync = true;

        // Back buffers - DX11 uses flip model or bitblt
        std::vector<Ref<DX11Texture>> m_backBuffers;
        std::vector<Ref<DX11TextureView>> m_backBufferViews;
        uint32 m_currentBackBufferIndex = 0;
    };

} // namespace RVX
```

### 2.8 DX11BindingRemapper - 绑定重映射

由于 RHI 使用 Vulkan 风格的 set/binding 模型，而 DX11 使用 slot 模型，需要重映射：

```cpp
// DX11BindingRemapper.h
#pragma once

#include "DX11Common.h"
#include "RHI/RHIDescriptor.h"
#include <unordered_map>

namespace RVX
{
    // =============================================================================
    // DX11 Binding Remapper
    // =============================================================================
    // 将 RHI 的 (set, binding) 模型映射到 DX11 的 slot 模型
    //
    // 映射规则:
    // - Set 0-3 映射到不同的 slot 范围
    // - CBV: slots 0-13 (DX11 limit is 14 per stage)
    // - SRV: slots 0-127 
    // - UAV: slots 0-7 (DX11.0) or 0-63 (DX11.1)
    // - Sampler: slots 0-15
    //
    // 示例映射:
    // Set 0, Binding 0 (CBV) -> CB slot 0
    // Set 0, Binding 1 (SRV) -> Texture slot 0
    // Set 1, Binding 0 (CBV) -> CB slot 4 (offset by set)
    
    class DX11BindingRemapper
    {
    public:
        struct SlotAssignment
        {
            uint32 cbSlotBase = 0;      // 该 set 的 CB 起始 slot
            uint32 srvSlotBase = 0;     // 该 set 的 SRV 起始 slot
            uint32 uavSlotBase = 0;     // 该 set 的 UAV 起始 slot
            uint32 samplerSlotBase = 0; // 该 set 的 Sampler 起始 slot
        };

        // 初始化默认槽位分配
        void Initialize();

        // 根据 set 和 binding 获取 DX11 槽位
        uint32 GetCBSlot(uint32 set, uint32 binding) const;
        uint32 GetSRVSlot(uint32 set, uint32 binding) const;
        uint32 GetUAVSlot(uint32 set, uint32 binding) const;
        uint32 GetSamplerSlot(uint32 set, uint32 binding) const;

        // 设置槽位分配策略
        void SetSlotAssignment(uint32 set, const SlotAssignment& assignment);

        // Push constants 使用的 CB 槽位 (默认 slot 14，保留给 push constants)
        static constexpr uint32 PUSH_CONSTANT_SLOT = 13;

    private:
        std::array<SlotAssignment, 4> m_setAssignments;
    };

    // 全局绑定重映射器
    DX11BindingRemapper& GetDX11BindingRemapper();

} // namespace RVX
```

---

## 3. 特殊处理

### 3.1 资源状态管理

DX11 由驱动自动管理资源状态，Barrier 操作为空实现：

```cpp
void DX11CommandContext::BufferBarrier(const RHIBufferBarrier& barrier)
{
    // DX11: 驱动自动处理资源状态，无需显式 barrier
    (void)barrier;
}

void DX11CommandContext::TextureBarrier(const RHITextureBarrier& barrier)
{
    // DX11: 驱动自动处理资源状态，无需显式 barrier
    (void)barrier;
}
```

### 3.2 Heap 和 Placed Resources

DX11 不支持 Heap 和 Placed Resources，提供降级实现：

```cpp
RHIHeapRef DX11Device::CreateHeap(const RHIHeapDesc& desc)
{
    RVX_RHI_WARN("DX11 does not support Heaps, returning nullptr");
    return nullptr;
}

RHITextureRef DX11Device::CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc)
{
    RVX_RHI_WARN("DX11 does not support Placed Resources, creating standalone texture");
    // 忽略 heap 和 offset，创建独立资源
    return CreateTexture(desc);
}

IRHIDevice::MemoryRequirements DX11Device::GetTextureMemoryRequirements(const RHITextureDesc& desc)
{
    // 估算纹理内存需求（无法精确获取）
    uint64 size = desc.width * desc.height * GetFormatBytesPerPixel(desc.format);
    size *= desc.arraySize * desc.mipLevels;
    return { size, 256 };  // 256 字节对齐是常见默认值
}
```

### 3.3 Fence 实现

DX11 的 Fence 支持取决于 Windows 版本：

```cpp
DX11Fence::DX11Fence(DX11Device* device, uint64 initialValue)
    : m_device(device), m_value(initialValue)
{
    // 尝试使用 Win10+ 的原生 Fence
    if (auto device5 = device->GetD3DDevice5())
    {
        HRESULT hr = device5->CreateFence(
            initialValue, 
            D3D11_FENCE_FLAG_NONE, 
            IID_PPV_ARGS(&m_fence)
        );
        
        if (SUCCEEDED(hr))
        {
            m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            RVX_RHI_DEBUG("DX11: Using native ID3D11Fence");
            return;
        }
    }

    // 降级到 Query-based fence
    RVX_RHI_DEBUG("DX11: Using Query-based fence (legacy)");
    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_EVENT;
    device->GetD3DDevice()->CreateQuery(&queryDesc, &m_query);
}

void DX11Fence::Wait(uint64 value, uint64 timeoutNs)
{
    if (m_fence)
    {
        // 使用原生 Fence
        if (m_fence->GetCompletedValue() < value)
        {
            m_fence->SetEventOnCompletion(value, m_event);
            DWORD timeoutMs = (timeoutNs == UINT64_MAX) ? INFINITE : 
                              static_cast<DWORD>(timeoutNs / 1000000);
            WaitForSingleObject(m_event, timeoutMs);
        }
    }
    else if (m_query)
    {
        // Legacy: 使用 Query 轮询
        auto* context = m_device->GetImmediateContext();
        BOOL result = FALSE;
        while (context->GetData(m_query.Get(), &result, sizeof(result), 0) == S_FALSE)
        {
            // 自旋等待或 yield
            std::this_thread::yield();
        }
    }
}
```

### 3.4 Multi-threading 支持

DX11 通过 Deferred Context 支持多线程命令录制：

```cpp
RHICommandContextRef DX11Device::CreateCommandContext(RHICommandQueueType type)
{
    // 根据线程模式决定使用 Immediate 还是 Deferred Context
    bool useDeferred = false;
    
    switch (m_threadingMode)
    {
        case DX11ThreadingMode::SingleThreaded:
            useDeferred = false;
            break;
            
        case DX11ThreadingMode::DeferredContext:
            useDeferred = m_capabilities.dx11.supportsDeferredContext;
            break;
            
        case DX11ThreadingMode::Adaptive:
            // 根据当前帧的 draw call 数量动态决定
            // 需要上一帧的统计信息
            useDeferred = m_capabilities.dx11.supportsDeferredContext;
            break;
    }

    return Ref<DX11CommandContext>(new DX11CommandContext(this, type, useDeferred));
}

void DX11Device::SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence)
{
    auto* dx11Context = static_cast<DX11CommandContext*>(context);
    
    if (dx11Context->IsDeferred())
    {
        // 执行 Deferred Context 的命令列表
        ID3D11CommandList* cmdList = dx11Context->GetCommandList();
        if (cmdList)
        {
            m_immediateContext->ExecuteCommandList(cmdList, FALSE);
        }
    }
    // Immediate Context: 命令已经执行，无需额外操作

    // Signal fence if provided
    if (signalFence)
    {
        auto* dx11Fence = static_cast<DX11Fence*>(signalFence);
        dx11Fence->Signal(dx11Fence->GetCompletedValue() + 1);
    }
}
```

---

## 4. 格式转换表

### 4.1 RHIFormat -> DXGI_FORMAT

```cpp
// DX11Conversions.h
inline DXGI_FORMAT ToDXGIFormat(RHIFormat format)
{
    switch (format)
    {
        case RHIFormat::Unknown:           return DXGI_FORMAT_UNKNOWN;
        
        // 8-bit
        case RHIFormat::R8_UNORM:          return DXGI_FORMAT_R8_UNORM;
        case RHIFormat::R8_SNORM:          return DXGI_FORMAT_R8_SNORM;
        case RHIFormat::R8_UINT:           return DXGI_FORMAT_R8_UINT;
        case RHIFormat::R8_SINT:           return DXGI_FORMAT_R8_SINT;
        
        // 16-bit
        case RHIFormat::R16_FLOAT:         return DXGI_FORMAT_R16_FLOAT;
        case RHIFormat::R16_UNORM:         return DXGI_FORMAT_R16_UNORM;
        case RHIFormat::R16_UINT:          return DXGI_FORMAT_R16_UINT;
        case RHIFormat::R16_SINT:          return DXGI_FORMAT_R16_SINT;
        case RHIFormat::RG8_UNORM:         return DXGI_FORMAT_R8G8_UNORM;
        case RHIFormat::RG8_SNORM:         return DXGI_FORMAT_R8G8_SNORM;
        case RHIFormat::RG8_UINT:          return DXGI_FORMAT_R8G8_UINT;
        case RHIFormat::RG8_SINT:          return DXGI_FORMAT_R8G8_SINT;
        
        // 32-bit
        case RHIFormat::R32_FLOAT:         return DXGI_FORMAT_R32_FLOAT;
        case RHIFormat::R32_UINT:          return DXGI_FORMAT_R32_UINT;
        case RHIFormat::R32_SINT:          return DXGI_FORMAT_R32_SINT;
        case RHIFormat::RG16_FLOAT:        return DXGI_FORMAT_R16G16_FLOAT;
        case RHIFormat::RG16_UNORM:        return DXGI_FORMAT_R16G16_UNORM;
        case RHIFormat::RG16_UINT:         return DXGI_FORMAT_R16G16_UINT;
        case RHIFormat::RG16_SINT:         return DXGI_FORMAT_R16G16_SINT;
        case RHIFormat::RGBA8_UNORM:       return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RHIFormat::RGBA8_UNORM_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case RHIFormat::RGBA8_SNORM:       return DXGI_FORMAT_R8G8B8A8_SNORM;
        case RHIFormat::RGBA8_UINT:        return DXGI_FORMAT_R8G8B8A8_UINT;
        case RHIFormat::RGBA8_SINT:        return DXGI_FORMAT_R8G8B8A8_SINT;
        case RHIFormat::BGRA8_UNORM:       return DXGI_FORMAT_B8G8R8A8_UNORM;
        case RHIFormat::BGRA8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case RHIFormat::RGB10A2_UNORM:     return DXGI_FORMAT_R10G10B10A2_UNORM;
        case RHIFormat::RGB10A2_UINT:      return DXGI_FORMAT_R10G10B10A2_UINT;
        case RHIFormat::RG11B10_FLOAT:     return DXGI_FORMAT_R11G11B10_FLOAT;
        
        // 64-bit
        case RHIFormat::RG32_FLOAT:        return DXGI_FORMAT_R32G32_FLOAT;
        case RHIFormat::RG32_UINT:         return DXGI_FORMAT_R32G32_UINT;
        case RHIFormat::RG32_SINT:         return DXGI_FORMAT_R32G32_SINT;
        case RHIFormat::RGBA16_FLOAT:      return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RHIFormat::RGBA16_UNORM:      return DXGI_FORMAT_R16G16B16A16_UNORM;
        case RHIFormat::RGBA16_UINT:       return DXGI_FORMAT_R16G16B16A16_UINT;
        case RHIFormat::RGBA16_SINT:       return DXGI_FORMAT_R16G16B16A16_SINT;
        
        // 96-bit (vertex data)
        case RHIFormat::RGB32_FLOAT:       return DXGI_FORMAT_R32G32B32_FLOAT;
        case RHIFormat::RGB32_UINT:        return DXGI_FORMAT_R32G32B32_UINT;
        case RHIFormat::RGB32_SINT:        return DXGI_FORMAT_R32G32B32_SINT;
        
        // 128-bit
        case RHIFormat::RGBA32_FLOAT:      return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case RHIFormat::RGBA32_UINT:       return DXGI_FORMAT_R32G32B32A32_UINT;
        case RHIFormat::RGBA32_SINT:       return DXGI_FORMAT_R32G32B32A32_SINT;
        
        // Depth-Stencil
        case RHIFormat::D16_UNORM:         return DXGI_FORMAT_D16_UNORM;
        case RHIFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case RHIFormat::D32_FLOAT:         return DXGI_FORMAT_D32_FLOAT;
        case RHIFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        
        // Compressed
        case RHIFormat::BC1_UNORM:         return DXGI_FORMAT_BC1_UNORM;
        case RHIFormat::BC1_UNORM_SRGB:    return DXGI_FORMAT_BC1_UNORM_SRGB;
        case RHIFormat::BC2_UNORM:         return DXGI_FORMAT_BC2_UNORM;
        case RHIFormat::BC2_UNORM_SRGB:    return DXGI_FORMAT_BC2_UNORM_SRGB;
        case RHIFormat::BC3_UNORM:         return DXGI_FORMAT_BC3_UNORM;
        case RHIFormat::BC3_UNORM_SRGB:    return DXGI_FORMAT_BC3_UNORM_SRGB;
        case RHIFormat::BC4_UNORM:         return DXGI_FORMAT_BC4_UNORM;
        case RHIFormat::BC4_SNORM:         return DXGI_FORMAT_BC4_SNORM;
        case RHIFormat::BC5_UNORM:         return DXGI_FORMAT_BC5_UNORM;
        case RHIFormat::BC5_SNORM:         return DXGI_FORMAT_BC5_SNORM;
        case RHIFormat::BC6H_UF16:         return DXGI_FORMAT_BC6H_UF16;
        case RHIFormat::BC6H_SF16:         return DXGI_FORMAT_BC6H_SF16;
        case RHIFormat::BC7_UNORM:         return DXGI_FORMAT_BC7_UNORM;
        case RHIFormat::BC7_UNORM_SRGB:    return DXGI_FORMAT_BC7_UNORM_SRGB;
        
        default:                           return DXGI_FORMAT_UNKNOWN;
    }
}
```

---

## 5. 实现优先级

### Phase 1: 核心功能 (MVP)

1. **DX11Device**: 设备创建、销毁、能力查询
2. **DX11Buffer**: Vertex/Index/Constant buffer
3. **DX11Texture**: 2D 纹理、渲染目标、深度缓冲
4. **DX11Shader**: VS/PS 着色器创建
5. **DX11GraphicsPipeline**: 基本图形管线
6. **DX11CommandContext**: 立即模式上下文
7. **DX11SwapChain**: 基本交换链

### Phase 2: 完整图形功能

1. **DX11TextureView**: 纹理视图
2. **DX11Sampler**: 采样器
3. **DX11DescriptorSet**: 描述符集
4. **DX11StateCache**: 状态缓存优化
5. 完整的状态对象（Rasterizer/DepthStencil/Blend）
6. Input Layout 支持

### Phase 3: 高级功能

1. **DX11ComputePipeline**: 计算管线
2. **DX11Fence**: 同步原语
3. **Deferred Context**: 多线程命令录制
4. UAV 支持
5. Geometry/Hull/Domain Shader

### Phase 4: 优化

1. 状态缓存优化
2. 资源池化
3. 动态常量缓冲区池
4. 调试标记 (PIX 支持)

---

## 6. 测试计划

### 6.1 单元测试

1. 资源创建/销毁
2. 格式转换正确性
3. 状态缓存行为
4. Binding 映射

### 6.2 集成测试

使用现有 Samples 进行测试：

1. **Triangle**: 基本渲染管线
2. **TexturedQuad**: 纹理采样
3. **Cube3D**: 3D 变换、深度缓冲
4. **ComputeDemo**: 计算着色器

### 6.3 验证测试

1. **DX11Validation**: 专用 DX11 验证测试
2. **CrossBackendValidation**: 跨后端一致性验证

---

## 7. 已知限制

| 限制 | 描述 | 解决方案 |
|------|------|----------|
| 无 Heap 支持 | DX11 不支持 Placed Resources | 返回 nullptr，创建独立资源 |
| 单命令队列 | 无专用 Compute/Copy 队列 | 串行执行 |
| Fence 兼容性 | Win7/8 无 ID3D11Fence | Query-based 降级 |
| UAV 限制 | DX11.0 PS UAV 仅 8 个 | 检测特性级别 |
| Bindless 资源 | 不支持 | 不可用 |
| 多显示器 | DXGI 1.2+ 支持 | 需要检测 |

---

## 8. 快速问题定位指南

### 8.1 常见错误类型及诊断

#### 1. DEVICE_REMOVED / DEVICE_HUNG

**症状**: 程序突然崩溃或停止响应

**诊断步骤**:
```cpp
// 在每个关键操作后检查设备状态
void DX11Device::CheckDeviceStatus()
{
    HRESULT hr = m_device->GetDeviceRemovedReason();
    if (hr != S_OK)
    {
        DX11Debug::Get().DiagnoseDeviceRemoved(m_device.Get());
        
        // 输出最后执行的操作
        RVX_RHI_ERROR("Last operations before device removed:");
        // ... 输出操作日志 ...
    }
}
```

**常见原因**:
- GPU 超时（TDR）- 着色器执行时间过长
- 内存不足
- 驱动程序崩溃
- 无效的资源访问

#### 2. 资源绑定错误

**症状**: 渲染结果不正确，黑屏，部分渲染

**诊断宏**:
```cpp
// 验证所有绑定在 Draw 之前
#define DX11_VALIDATE_DRAW_STATE(ctx) \
    do { \
        if (!DX11Debug::Get().ValidateDrawState(ctx)) { \
            RVX_RHI_ERROR("Invalid draw state detected!"); \
            return; \
        } \
    } while(0)

bool DX11Debug::ValidateDrawState(ID3D11DeviceContext* context)
{
    // 检查是否有 RTV 绑定
    ID3D11RenderTargetView* rtvs[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(8, rtvs, &dsv);
    
    bool hasRT = false;
    for (auto rtv : rtvs) {
        if (rtv) { hasRT = true; rtv->Release(); }
    }
    if (dsv) dsv->Release();
    
    if (!hasRT && !dsv) {
        RVX_RHI_ERROR("No render target or depth stencil bound!");
        return false;
    }

    // 检查着色器
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    context->VSGetShader(&vs, nullptr, nullptr);
    context->PSGetShader(&ps, nullptr, nullptr);
    
    if (!vs) RVX_RHI_ERROR("No vertex shader bound!");
    if (!ps) RVX_RHI_WARN("No pixel shader bound");
    
    if (vs) vs->Release();
    if (ps) ps->Release();

    return vs != nullptr;
}
```

#### 3. Shader 编译/链接错误

**诊断**:
```cpp
RHIShaderRef DX11Device::CreateShader(const RHIShaderDesc& desc)
{
    // DXBC bytecode 直接使用
    // 如果使用 D3DCompile，捕获错误信息
    
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(
        sourceCode, sourceCodeLength,
        debugName, defines, includes,
        entryPoint, target, flags, 0,
        &shaderBlob, &errorBlob
    );
    
    if (FAILED(hr))
    {
        if (errorBlob)
        {
            RVX_RHI_ERROR("Shader compilation failed for '{}':", debugName);
            RVX_RHI_ERROR("{}", static_cast<char*>(errorBlob->GetBufferPointer()));
            
            // 输出源代码位置信息
            LogShaderSourceContext(sourceCode, errorBlob);
        }
        return nullptr;
    }
    
    // ...
}
```

### 8.2 调试输出分级

```cpp
// 根据严重程度分级的日志系统
enum class DX11DebugLevel
{
    Verbose,    // 所有操作
    Info,       // 资源创建/销毁
    Warning,    // 潜在问题
    Error,      // 确定性错误
    Fatal       // 导致崩溃的错误
};

// 配置调试级别
void DX11Debug::SetDebugLevel(DX11DebugLevel level)
{
    m_debugLevel = level;
    
    if (m_infoQueue)
    {
        // 根据级别配置 InfoQueue
        m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, 
            level <= DX11DebugLevel::Error);
        m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, 
            level <= DX11DebugLevel::Warning);
    }
}
```

### 8.3 操作日志追踪

```cpp
// 环形缓冲区记录最近的操作
class DX11OperationLog
{
public:
    struct Operation
    {
        uint64 frameIndex;
        uint64 timestamp;
        const char* type;
        std::string details;
    };

    void Log(const char* type, const std::string& details)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        Operation op;
        op.frameIndex = m_currentFrame;
        op.timestamp = GetTimestamp();
        op.type = type;
        op.details = details;
        
        m_operations[m_writeIndex] = std::move(op);
        m_writeIndex = (m_writeIndex + 1) % MAX_OPERATIONS;
    }

    void DumpRecent(size_t count = 20)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        RVX_RHI_ERROR("=== Recent DX11 Operations ===");
        size_t start = (m_writeIndex + MAX_OPERATIONS - count) % MAX_OPERATIONS;
        
        for (size_t i = 0; i < count; ++i)
        {
            const auto& op = m_operations[(start + i) % MAX_OPERATIONS];
            if (op.type)
            {
                RVX_RHI_ERROR("  [Frame {}] {}: {}", 
                    op.frameIndex, op.type, op.details);
            }
        }
    }

private:
    static constexpr size_t MAX_OPERATIONS = 256;
    std::array<Operation, MAX_OPERATIONS> m_operations;
    size_t m_writeIndex = 0;
    uint64 m_currentFrame = 0;
    std::mutex m_mutex;
};

// 使用宏简化日志记录
#define DX11_LOG_OP(type, fmt, ...) \
    DX11OperationLog::Get().Log(type, std::format(fmt, ##__VA_ARGS__))

// 示例使用
void DX11CommandContext::Draw(uint32 vertexCount, ...)
{
    DX11_LOG_OP("Draw", "vertices={} instances={}", vertexCount, instanceCount);
    m_context->DrawInstanced(...);
}

void DX11Device::CreateBuffer(...)
{
    DX11_LOG_OP("CreateBuffer", "size={} usage={}", desc.size, ToString(desc.usage));
    // ...
}
```

### 8.4 PIX/RenderDoc 集成

```cpp
// 确保调试标记在 GPU 捕获中可见
class DX11DebugMarkers
{
public:
    // 使用 WinPixEventRuntime 获得最佳 PIX 支持
    static void BeginEvent(ID3D11DeviceContext* ctx, const wchar_t* name, UINT64 color = 0)
    {
        #ifdef USE_PIX
        PIXBeginEvent(ctx, color, name);
        #else
        // 回退到 D3DPERF
        D3DPERF_BeginEvent(static_cast<DWORD>(color), name);
        #endif
    }

    static void EndEvent(ID3D11DeviceContext* ctx)
    {
        #ifdef USE_PIX
        PIXEndEvent(ctx);
        #else
        D3DPERF_EndEvent();
        #endif
    }

    static void SetMarker(ID3D11DeviceContext* ctx, const wchar_t* name, UINT64 color = 0)
    {
        #ifdef USE_PIX
        PIXSetMarker(ctx, color, name);
        #else
        D3DPERF_SetMarker(static_cast<DWORD>(color), name);
        #endif
    }
};

// RAII 作用域标记
class DX11ScopedEvent
{
public:
    DX11ScopedEvent(ID3D11DeviceContext* ctx, const wchar_t* name, UINT64 color = 0)
        : m_ctx(ctx)
    {
        DX11DebugMarkers::BeginEvent(ctx, name, color);
    }
    
    ~DX11ScopedEvent()
    {
        DX11DebugMarkers::EndEvent(m_ctx);
    }

private:
    ID3D11DeviceContext* m_ctx;
};

#define DX11_SCOPED_EVENT(ctx, name) \
    DX11ScopedEvent _scopedEvent##__LINE__(ctx, L##name)

// 颜色常量（用于 PIX 可视化）
namespace DX11EventColors
{
    constexpr UINT64 Draw     = 0xFF00FF00;  // Green
    constexpr UINT64 Dispatch = 0xFF0000FF;  // Blue
    constexpr UINT64 Copy     = 0xFFFFFF00;  // Yellow
    constexpr UINT64 Clear    = 0xFF00FFFF;  // Cyan
    constexpr UINT64 Barrier  = 0xFFFF00FF;  // Magenta
    constexpr UINT64 Pass     = 0xFFFF8000;  // Orange
}
```

### 8.5 资源状态验证

虽然 DX11 不需要显式状态管理，但验证资源使用模式仍有价值：

```cpp
class DX11ResourceValidator
{
public:
    // 验证 Buffer 用于正确的绑定点
    bool ValidateBufferBinding(ID3D11Buffer* buffer, D3D11_BIND_FLAG expectedBind)
    {
        if (!buffer) return false;
        
        D3D11_BUFFER_DESC desc;
        buffer->GetDesc(&desc);
        
        if (!(desc.BindFlags & expectedBind))
        {
            RVX_RHI_ERROR("Buffer {} bound incorrectly:", GetResourceName(buffer));
            RVX_RHI_ERROR("  Expected bind flag: 0x{:X}", expectedBind);
            RVX_RHI_ERROR("  Actual bind flags: 0x{:X}", desc.BindFlags);
            return false;
        }
        
        return true;
    }

    // 验证 SRV 不与当前 RTV/UAV 冲突
    bool ValidateSRVNotBoundAsOutput(ID3D11ShaderResourceView* srv,
                                     ID3D11RenderTargetView** rtvs, uint32 rtvCount,
                                     ID3D11UnorderedAccessView** uavs, uint32 uavCount)
    {
        if (!srv) return true;
        
        ComPtr<ID3D11Resource> srvResource;
        srv->GetResource(&srvResource);
        
        // 检查 RTV 冲突
        for (uint32 i = 0; i < rtvCount; ++i)
        {
            if (!rtvs[i]) continue;
            
            ComPtr<ID3D11Resource> rtvResource;
            rtvs[i]->GetResource(&rtvResource);
            
            if (srvResource.Get() == rtvResource.Get())
            {
                RVX_RHI_ERROR("Resource bound as both SRV and RTV - undefined behavior!");
                LogResourceInfo(srvResource.Get());
                return false;
            }
        }
        
        // 检查 UAV 冲突
        for (uint32 i = 0; i < uavCount; ++i)
        {
            if (!uavs[i]) continue;
            
            ComPtr<ID3D11Resource> uavResource;
            uavs[i]->GetResource(&uavResource);
            
            if (srvResource.Get() == uavResource.Get())
            {
                RVX_RHI_ERROR("Resource bound as both SRV and UAV - potential hazard!");
                LogResourceInfo(srvResource.Get());
                return false;
            }
        }
        
        return true;
    }
    
private:
    std::string GetResourceName(ID3D11DeviceChild* resource)
    {
        char name[256] = {};
        UINT size = sizeof(name);
        if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
        {
            return name;
        }
        return "<unnamed>";
    }
    
    void LogResourceInfo(ID3D11Resource* resource)
    {
        D3D11_RESOURCE_DIMENSION dim;
        resource->GetType(&dim);
        
        switch (dim)
        {
            case D3D11_RESOURCE_DIMENSION_BUFFER:
            {
                auto* buffer = static_cast<ID3D11Buffer*>(resource);
                D3D11_BUFFER_DESC desc;
                buffer->GetDesc(&desc);
                RVX_RHI_ERROR("  Buffer: size={} bind=0x{:X}", desc.ByteWidth, desc.BindFlags);
                break;
            }
            case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
            {
                auto* tex = static_cast<ID3D11Texture2D*>(resource);
                D3D11_TEXTURE2D_DESC desc;
                tex->GetDesc(&desc);
                RVX_RHI_ERROR("  Texture2D: {}x{} format={} bind=0x{:X}",
                    desc.Width, desc.Height, desc.Format, desc.BindFlags);
                break;
            }
            // ... 其他类型 ...
        }
    }
};
```

### 8.6 调试检查清单

当遇到问题时，按以下顺序检查：

1. **黑屏/无输出**
   - [ ] 检查是否有 RTV 绑定
   - [ ] 检查 Viewport 是否设置正确
   - [ ] 检查着色器是否绑定
   - [ ] 验证 Clear 颜色不是黑色（如果使用了 Clear）

2. **渲染错误**
   - [ ] 检查顶点/索引缓冲区绑定
   - [ ] 验证 Input Layout 与顶点着色器匹配
   - [ ] 检查 Constant Buffer 数据是否正确上传
   - [ ] 验证纹理绑定到正确的槽位

3. **性能问题**
   - [ ] 检查是否有不必要的状态切换
   - [ ] 验证资源是否在正确的内存类型中创建
   - [ ] 检查是否频繁 Map/Unmap

4. **崩溃/Device Removed**
   - [ ] 启用调试层并检查 InfoQueue 消息
   - [ ] 检查最后的操作日志
   - [ ] 使用 PIX/RenderDoc 捕获并分析

---

## 9. 参考资料

### 核心文档
- [Direct3D 11 Programming Guide](https://docs.microsoft.com/en-us/windows/win32/direct3d11/dx-graphics-overviews)
- [DXGI Overview](https://docs.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)
- [ID3D11Device Interface](https://docs.microsoft.com/en-us/windows/win32/api/d3d11/nn-d3d11-id3d11device)
- [Deferred Context](https://docs.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro)

### 调试相关
- [Using the Debug Layer](https://docs.microsoft.com/en-us/windows/win32/direct3d11/using-the-debug-layer-to-test-apps)
- [ID3D11InfoQueue Interface](https://docs.microsoft.com/en-us/windows/win32/api/d3d11sdklayers/nn-d3d11sdklayers-id3d11infoqueue)
- [ID3D11Debug Interface](https://docs.microsoft.com/en-us/windows/win32/api/d3d11sdklayers/nn-d3d11sdklayers-id3d11debug)
- [DXGI Debug](https://docs.microsoft.com/en-us/windows/win32/api/dxgidebug/)
- [PIX for Windows](https://devblogs.microsoft.com/pix/)
- [RenderDoc](https://renderdoc.org/)

### 最佳实践
- [D3D11 on 12](https://docs.microsoft.com/en-us/windows/win32/direct3d12/direct3d-11-on-12)
- [Resource Binding in D3D11](https://docs.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-resources-intro)
- [State Objects](https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-api-features-state-objects)

### 工具
- [Graphics Debugger in Visual Studio](https://docs.microsoft.com/en-us/visualstudio/debugger/graphics/visual-studio-graphics-diagnostics)
- [DXCap.exe (DXGI Capture Tool)](https://docs.microsoft.com/en-us/windows/win32/direct3dtools/dxcap)

---

## 附录 A: 实施检查表

### Phase 1: 基础框架
- [ ] `DX11Common.h` 创建完成
- [ ] `DX11Conversions.h` 创建完成
- [ ] `DX11Debug.h/cpp` 创建完成
- [ ] `DX11Device.h` (Private) 创建完成
- [ ] `DX11Device.cpp` 初始化实现完成
- [ ] 编译通过: `cmake --build build/Debug --target RVX_RHI_DX11`
- [ ] 设备创建成功，GPU名称可打印

### Phase 2: 核心资源
- [ ] `DX11Resources.h` 创建完成
- [ ] `DX11Buffer` 实现完成
  - [ ] CreateBuffer() 工作
  - [ ] Map()/Unmap() 工作
  - [ ] SRV/UAV 创建
- [ ] `DX11Texture` 实现完成
  - [ ] Texture2D 创建
  - [ ] SRV/RTV/DSV 创建
- [ ] `DX11TextureView` 实现完成
- [ ] `DX11Sampler` 实现完成
- [ ] `DX11Shader` 实现完成
  - [ ] VS 创建
  - [ ] PS 创建
- [ ] `DX11Fence` 实现完成

### Phase 3: 渲染管线
- [ ] `DX11StateCache.h/cpp` 创建完成
- [ ] `DX11BindingRemapper.h/cpp` 实现完成
- [ ] `DX11Pipeline.h` 创建完成
- [ ] `DX11DescriptorSetLayout` 实现完成
- [ ] `DX11PipelineLayout` 实现完成
- [ ] `DX11DescriptorSet` 实现完成
- [ ] `DX11GraphicsPipeline` 实现完成

### Phase 4: 命令执行
- [ ] `DX11CommandContext.h` 创建完成
- [ ] Begin()/End()/Reset() 实现
- [ ] BeginRenderPass()/EndRenderPass() 实现
- [ ] SetPipeline() 实现
- [ ] SetVertexBuffer()/SetIndexBuffer() 实现
- [ ] SetDescriptorSet() 实现
- [ ] SetViewport()/SetScissor() 实现
- [ ] Draw()/DrawIndexed() 实现

### Phase 5: 呈现系统
- [ ] `DX11SwapChain.h/cpp` 实现完成
  - [ ] SwapChain 创建
  - [ ] BackBuffer 管理
  - [ ] Present() 工作
  - [ ] Resize() 工作
- [ ] BeginFrame()/EndFrame() 完善
- [ ] SubmitCommandContext() 实现

### Phase 6: 验证测试
- [ ] Triangle Sample 运行
- [ ] TexturedQuad Sample 运行
- [ ] Cube3D Sample 运行
- [ ] 无 Debug Layer 错误

### Phase 7: 完善优化
- [ ] 调试系统完善
  - [ ] 资源跟踪
  - [ ] 操作日志
  - [ ] Device Removed 诊断
- [ ] ComputePipeline 实现
- [ ] Copy 命令实现
- [ ] 性能优化
- [ ] ComputeDemo 运行
- [ ] 所有测试通过

---

## 附录 B: DX11 调试层安装

在某些系统上，DX11 调试层可能需要单独安装：

### Windows 10/11
```powershell
# 检查是否已安装
Get-WindowsOptionalFeature -Online -FeatureName "Microsoft-Windows-GraphicsTools"

# 安装（需要管理员权限）
Enable-WindowsOptionalFeature -Online -FeatureName "Microsoft-Windows-GraphicsTools"
```

### 或通过 Windows 设置
1. 设置 → 应用 → 可选功能
2. 添加功能
3. 搜索 "Graphics Tools"
4. 安装

### 验证安装
```cpp
bool IsDX11DebugLayerAvailable()
{
    // 尝试创建带调试层的设备
    ComPtr<ID3D11Device> testDevice;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_NULL,  // 使用 NULL 驱动快速测试
        nullptr,
        D3D11_CREATE_DEVICE_DEBUG,
        nullptr, 0,
        D3D11_SDK_VERSION,
        &testDevice,
        nullptr, nullptr
    );
    
    return SUCCEEDED(hr);
}
```
