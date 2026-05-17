# RHI OpenGL 后端实现规划

> **文档版本**: 2.0  
> **创建日期**: 2026-01-20  
> **更新日期**: 2026-01-20  
> **目标**: 为 RenderVerseX 引擎添加 OpenGL 图形 API 后端支持，主要面向 Linux 和 Windows 老硬件

---

## 目录

1. [设计目标与适用场景](#1-设计目标与适用场景)
2. [OpenGL 版本选择与特性评估](#2-opengl-版本选择与特性评估)
3. [架构设计挑战与解决方案](#3-架构设计挑战与解决方案)
4. [RHI 接口到 OpenGL 的映射](#4-rhi-接口到-opengl-的映射)
5. [着色器编译策略](#5-着色器编译策略)
6. [模块结构设计](#6-模块结构设计)
7. [核心类设计](#7-核心类设计)
8. [OpenGL 上下文与线程安全](#8-opengl-上下文与线程安全)
9. [资源生命周期管理](#9-资源生命周期管理)
10. [FBO 与 VAO 缓存策略](#10-fbo-与-vao-缓存策略)
11. [现有代码修改清单](#11-现有代码修改清单)
12. [依赖项配置](#12-依赖项配置)
13. [实施路线图](#13-实施路线图)
14. [平台支持矩阵](#14-平台支持矩阵)
15. [最佳实践与性能优化](#15-最佳实践与性能优化)

---

## 1. 设计目标与适用场景

### 1.1 主要目标

| 目标 | 描述 |
|------|------|
| **Linux 支持** | 作为 Linux 平台的主要图形后端 |
| **老硬件兼容** | 支持不支持 Vulkan 的老旧 GPU |
| **跨平台验证** | 提供第三方图形 API 验证参考 |
| **教学与调试** | OpenGL 的即时模式便于调试 |

### 1.2 非目标

| 非目标 | 原因 |
|--------|------|
| **macOS 主力支持** | macOS 已废弃 OpenGL（最高 4.1），应使用 Metal |
| **移动端支持** | OpenGL ES 可作为后续扩展，不在初期范围内 |
| **追求最高性能** | Vulkan/DX12/Metal 更适合性能关键场景 |

### 1.3 后端优先级矩阵

| 平台 | 首选后端 | 备选后端 | OpenGL 适用场景 |
|------|----------|----------|-----------------|
| Windows | DX12 | DX11, Vulkan | 老硬件、集成显卡 |
| Linux | Vulkan | **OpenGL** | 无 Vulkan 驱动的系统 |
| macOS | Metal | - | ⚠️ 不推荐（已废弃） |

---

## 2. OpenGL 版本选择与特性评估

### 2.1 版本选项对比

| 版本 | 发布年份 | 关键特性 | 硬件覆盖率 | 推荐度 |
|------|----------|----------|-----------|--------|
| OpenGL 3.3 | 2010 | Core Profile 基础 | 99% | ⭐⭐ |
| OpenGL 4.3 | 2012 | Compute Shader, SSBO | 95% | ⭐⭐⭐ |
| **OpenGL 4.5** | 2014 | **DSA, 无需绑定** | 90% | ⭐⭐⭐⭐⭐ |
| OpenGL 4.6 | 2017 | 原生 SPIR-V | 80% | ⭐⭐⭐⭐ |

### 2.2 推荐版本: OpenGL 4.5 Core Profile

**选择理由**:

1. **Direct State Access (DSA)** - 无需绑定即可操作对象，与现代 API 风格一致
2. **广泛硬件支持** - 覆盖 2012 年后的大部分 GPU
3. **Compute Shader** - 支持通用计算
4. **SSBO (Shader Storage Buffer)** - 支持可读写存储缓冲
5. **多绑定 (Multi-Bind)** - 批量绑定优化

### 2.3 可选启用的扩展

```cpp
// 运行时检测并启用
struct OpenGLExtensions
{
    bool GL_ARB_gl_spirv = false;           // OpenGL 4.6: 原生 SPIR-V 着色器
    bool GL_ARB_bindless_texture = false;   // 无绑定纹理
    bool GL_ARB_shader_draw_parameters = false;  // gl_DrawID 等
    bool GL_ARB_indirect_parameters = false;     // 间接绘制参数
    bool GL_NV_mesh_shader = false;         // NVIDIA Mesh Shader
    bool GL_NV_ray_tracing = false;         // NVIDIA 光追
};
```

---

## 3. 架构设计挑战与解决方案

### 3.1 核心差异对比

| 设计维度 | 现代 API (DX12/Vulkan/Metal) | OpenGL | 挑战级别 |
|----------|----------------------------|--------|---------|
| **执行模型** | 命令缓冲区录制 + 批量提交 | 即时执行 | 🔴 高 |
| **状态管理** | 绑定到命令缓冲区 | 全局状态机 | 🔴 高 |
| **线程模型** | 多线程录制 | 单线程为主 | 🟡 中 |
| **资源绑定** | 描述符表/参数缓冲区 | 绑定点 | 🟡 中 |
| **管线对象** | 完整 PSO | 分离的程序对象 | 🟡 中 |
| **同步控制** | 显式栅栏/信号量 | 隐式同步 | 🟢 低 |
| **内存管理** | 显式堆分配 | 驱动管理 | 🟢 低 |

### 3.2 解决方案设计

#### 方案 A: 伪命令缓冲区 (推荐)

```
┌─────────────────────────────────────────────────────────┐
│                   RHICommandContext                     │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────┐   │
│  │              Command List (std::vector)          │   │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐            │   │
│  │  │ SetPipe │ │ SetVB   │ │ Draw    │  ...       │   │
│  │  └─────────┘ └─────────┘ └─────────┘            │   │
│  └─────────────────────────────────────────────────┘   │
│                         │                               │
│                         ▼ Submit()                      │
│  ┌─────────────────────────────────────────────────┐   │
│  │           Immediate Execution (Flush)            │   │
│  │   glBindPipeline() → glBindVAO() → glDraw()     │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

**优点**:
- 保持 RHI 语义一致性
- 便于命令排序和优化
- 支持延迟执行验证

**命令结构设计** (优化版 - 避免虚函数调用):

```cpp
// 命令类型枚举
enum class GLCommandType : uint8
{
    BeginRenderPass,
    EndRenderPass,
    SetPipeline,
    SetVertexBuffer,
    SetIndexBuffer,
    SetDescriptorSet,
    SetPushConstants,
    SetViewport,
    SetScissor,
    Draw,
    DrawIndexed,
    DrawIndirect,
    Dispatch,
    DispatchIndirect,
    CopyBuffer,
    CopyTexture,
    CopyBufferToTexture,
    BufferBarrier,
    TextureBarrier,
    BeginEvent,
    EndEvent,
    // ...
};

// ⚠️ 优化：使用 Type-Punned 结构避免虚函数
// 参考 id Tech / DOOM 的命令缓冲区设计

// 命令头 (所有命令共享)
struct GLCommandHeader
{
    GLCommandType type;
    uint8 padding;
    uint16 size;  // 命令总大小，用于遍历
};

// 具体命令 - 内联数据，无虚函数
struct GLDrawCommand
{
    GLCommandHeader header = {GLCommandType::Draw, 0, sizeof(GLDrawCommand)};
    GLenum primitiveMode;
    uint32 vertexCount;
    uint32 instanceCount;
    uint32 firstVertex;
    uint32 firstInstance;
};

struct GLDrawIndexedCommand
{
    GLCommandHeader header = {GLCommandType::DrawIndexed, 0, sizeof(GLDrawIndexedCommand)};
    GLenum primitiveMode;
    GLenum indexType;
    uint32 indexCount;
    uint32 instanceCount;
    uint32 firstIndex;
    int32 baseVertex;
    uint32 firstInstance;
};

struct GLSetPipelineCommand
{
    GLCommandHeader header = {GLCommandType::SetPipeline, 0, sizeof(GLSetPipelineCommand)};
    OpenGLPipeline* pipeline;
};

struct GLBeginRenderPassCommand
{
    GLCommandHeader header = {GLCommandType::BeginRenderPass, 0, sizeof(GLBeginRenderPassCommand)};
    GLuint framebuffer;
    float clearColor[4];
    float clearDepth;
    uint8 clearStencil;
    bool clearColorEnabled;
    bool clearDepthEnabled;
    bool clearStencilEnabled;
    uint32 width, height;
};

struct GLSetDescriptorSetCommand
{
    GLCommandHeader header = {GLCommandType::SetDescriptorSet, 0, sizeof(GLSetDescriptorSetCommand)};
    uint32 slot;
    OpenGLDescriptorSet* descriptorSet;
    uint32 dynamicOffsetCount;
    uint32 dynamicOffsets[8];  // 内联小数组
};

struct GLSetPushConstantsCommand
{
    GLCommandHeader header = {GLCommandType::SetPushConstants, 0, sizeof(GLSetPushConstantsCommand)};
    uint32 offset;
    uint32 size;
    uint8 data[256];  // 最大 push constant 大小
};

// 命令缓冲区 - 线性内存分配
class GLCommandBuffer
{
public:
    static constexpr size_t INITIAL_SIZE = 64 * 1024;  // 64KB
    
    GLCommandBuffer()
    {
        m_buffer.reserve(INITIAL_SIZE);
    }
    
    template<typename T>
    T* Allocate()
    {
        size_t offset = m_buffer.size();
        m_buffer.resize(offset + sizeof(T));
        return reinterpret_cast<T*>(m_buffer.data() + offset);
    }
    
    void Reset()
    {
        m_buffer.clear();
    }
    
    const uint8* Begin() const { return m_buffer.data(); }
    const uint8* End() const { return m_buffer.data() + m_buffer.size(); }
    size_t Size() const { return m_buffer.size(); }

private:
    std::vector<uint8> m_buffer;
};

// 命令执行器 - 使用 switch-case 而非虚函数
class GLCommandExecutor
{
public:
    void Execute(GLCommandBuffer& buffer, GLStateCache& cache)
    {
        const uint8* ptr = buffer.Begin();
        const uint8* end = buffer.End();
        
        while (ptr < end)
        {
            auto* header = reinterpret_cast<const GLCommandHeader*>(ptr);
            
            switch (header->type)
            {
                case GLCommandType::Draw:
                    ExecuteDraw(*reinterpret_cast<const GLDrawCommand*>(ptr), cache);
                    break;
                case GLCommandType::DrawIndexed:
                    ExecuteDrawIndexed(*reinterpret_cast<const GLDrawIndexedCommand*>(ptr), cache);
                    break;
                case GLCommandType::SetPipeline:
                    ExecuteSetPipeline(*reinterpret_cast<const GLSetPipelineCommand*>(ptr), cache);
                    break;
                case GLCommandType::BeginRenderPass:
                    ExecuteBeginRenderPass(*reinterpret_cast<const GLBeginRenderPassCommand*>(ptr), cache);
                    break;
                case GLCommandType::SetDescriptorSet:
                    ExecuteSetDescriptorSet(*reinterpret_cast<const GLSetDescriptorSetCommand*>(ptr), cache);
                    break;
                case GLCommandType::SetPushConstants:
                    ExecuteSetPushConstants(*reinterpret_cast<const GLSetPushConstantsCommand*>(ptr), cache);
                    break;
                // ... 其他命令
                default:
                    RVX_ASSERT(false && "Unknown command type");
            }
            
            ptr += header->size;
        }
    }

private:
    void ExecuteDraw(const GLDrawCommand& cmd, GLStateCache& cache)
    {
        glDrawArraysInstancedBaseInstance(
            cmd.primitiveMode,
            cmd.firstVertex,
            cmd.vertexCount,
            cmd.instanceCount,
            cmd.firstInstance
        );
    }
    
    void ExecuteDrawIndexed(const GLDrawIndexedCommand& cmd, GLStateCache& cache)
    {
        const void* offset = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(cmd.firstIndex * (cmd.indexType == GL_UNSIGNED_SHORT ? 2 : 4))
        );
        
        glDrawElementsInstancedBaseVertexBaseInstance(
            cmd.primitiveMode,
            cmd.indexCount,
            cmd.indexType,
            offset,
            cmd.instanceCount,
            cmd.baseVertex,
            cmd.firstInstance
        );
    }
    
    void ExecuteSetPipeline(const GLSetPipelineCommand& cmd, GLStateCache& cache)
    {
        cache.BindProgram(cmd.pipeline->GetProgram());
        cmd.pipeline->ApplyState(cache);
    }
    
    void ExecuteBeginRenderPass(const GLBeginRenderPassCommand& cmd, GLStateCache& cache)
    {
        cache.BindFramebuffer(cmd.framebuffer);
        glViewport(0, 0, cmd.width, cmd.height);
        
        if (cmd.clearColorEnabled)
        {
            glClearColor(cmd.clearColor[0], cmd.clearColor[1], 
                        cmd.clearColor[2], cmd.clearColor[3]);
        }
        if (cmd.clearDepthEnabled)
        {
            glClearDepth(cmd.clearDepth);
        }
        if (cmd.clearStencilEnabled)
        {
            glClearStencil(cmd.clearStencil);
        }
        
        GLbitfield clearMask = 0;
        if (cmd.clearColorEnabled) clearMask |= GL_COLOR_BUFFER_BIT;
        if (cmd.clearDepthEnabled) clearMask |= GL_DEPTH_BUFFER_BIT;
        if (cmd.clearStencilEnabled) clearMask |= GL_STENCIL_BUFFER_BIT;
        
        if (clearMask != 0)
        {
            glClear(clearMask);
        }
    }
    
    void ExecuteSetPushConstants(const GLSetPushConstantsCommand& cmd, GLStateCache& cache);
    void ExecuteSetDescriptorSet(const GLSetDescriptorSetCommand& cmd, GLStateCache& cache);
    // ... 其他执行函数
};
```

#### 方案 B: 状态缓存系统

```cpp
// 状态缓存 - 减少冗余 OpenGL 调用
class GLStateCache
{
public:
    // 绑定 Program (仅在变化时调用)
    void BindProgram(GLuint program)
    {
        if (m_boundProgram != program)
        {
            glUseProgram(program);
            m_boundProgram = program;
        }
    }

    // 绑定 VAO
    void BindVertexArray(GLuint vao)
    {
        if (m_boundVAO != vao)
        {
            glBindVertexArray(vao);
            m_boundVAO = vao;
        }
    }

    // 绑定 Framebuffer
    void BindFramebuffer(GLuint fbo)
    {
        if (m_boundFramebuffer != fbo)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            m_boundFramebuffer = fbo;
        }
    }

    // 设置深度状态
    void SetDepthState(bool enable, GLenum func, bool write)
    {
        if (m_depthTestEnabled != enable)
        {
            enable ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
            m_depthTestEnabled = enable;
        }
        if (enable && m_depthFunc != func)
        {
            glDepthFunc(func);
            m_depthFunc = func;
        }
        if (m_depthWrite != write)
        {
            glDepthMask(write ? GL_TRUE : GL_FALSE);
            m_depthWrite = write;
        }
    }

    // 重置状态 (帧开始时调用)
    void Invalidate()
    {
        m_boundProgram = 0;
        m_boundVAO = 0;
        m_boundFramebuffer = 0;
        // ...
    }

private:
    GLuint m_boundProgram = 0;
    GLuint m_boundVAO = 0;
    GLuint m_boundFramebuffer = 0;
    bool m_depthTestEnabled = false;
    GLenum m_depthFunc = GL_LESS;
    bool m_depthWrite = true;
    // ... 更多状态
};
```

#### 方案 C: 描述符集到绑定点映射

```
┌─────────────────────────────────────────────────────────┐
│                 RHI Descriptor Set                       │
│  ┌──────────────────────────────────────────────────┐   │
│  │ Set 0: Uniform Buffer (binding 0)                │   │
│  │ Set 1: Texture + Sampler (binding 0, 1)          │   │
│  │ Set 2: Storage Buffer (binding 0)                │   │
│  └──────────────────────────────────────────────────┘   │
│                         │                                │
│                         ▼ 映射                           │
│  ┌──────────────────────────────────────────────────┐   │
│  │              OpenGL Binding Points                │   │
│  │  UBO:     [0] [1] [2] ... [15]                   │   │
│  │  SSBO:    [0] [1] [2] ... [15]                   │   │
│  │  Texture: [0] [1] [2] ... [31]                   │   │
│  │  Sampler: [0] [1] [2] ... [15]                   │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

**绑定点分配策略**:

```cpp
struct GLBindingPointAllocator
{
    // 绑定点范围定义
    static constexpr uint32 UBO_BASE = 0;
    static constexpr uint32 UBO_MAX = 15;
    static constexpr uint32 SSBO_BASE = 0;
    static constexpr uint32 SSBO_MAX = 15;
    static constexpr uint32 TEXTURE_BASE = 0;
    static constexpr uint32 TEXTURE_MAX = 31;
    static constexpr uint32 IMAGE_BASE = 0;
    static constexpr uint32 IMAGE_MAX = 7;

    // 从 RHI set/binding 计算 OpenGL 绑定点
    uint32 CalculateUBOIndex(uint32 set, uint32 binding)
    {
        return (set * 4 + binding) % (UBO_MAX + 1);
    }

    uint32 CalculateTextureUnit(uint32 set, uint32 binding)
    {
        return (set * 8 + binding) % (TEXTURE_MAX + 1);
    }
};
```

---

## 4. RHI 接口到 OpenGL 的映射

### 4.1 资源类型映射

| RHI 类型 | OpenGL 类型 | 创建方式 | 备注 |
|----------|------------|----------|------|
| `RHIBuffer` | `GLuint` (Buffer Object) | `glCreateBuffers` (DSA) | VBO/UBO/SSBO 共用 |
| `RHITexture` | `GLuint` (Texture Object) | `glCreateTextures` (DSA) | 支持 2D/3D/Cube/Array |
| `RHITextureView` | `GLuint` (Texture View) | `glTextureView` | OpenGL 4.3+ |
| `RHISampler` | `GLuint` (Sampler Object) | `glCreateSamplers` | 独立采样器 |
| `RHIShader` | `GLuint` (Program Object) | `glCreateProgram` | 链接后的程序 |
| `RHIPipeline` | 自定义 `GLPipelineState` | 组合多个状态 | 包含 Program + State |
| `RHIDescriptorSetLayout` | `GLDescriptorLayout` | 元数据结构 | 仅记录布局信息 |
| `RHIDescriptorSet` | `GLDescriptorSet` | 绑定记录 | 资源引用列表 |
| `RHIPipelineLayout` | `GLPipelineLayout` | 元数据结构 | 轻量封装 |
| `RHISwapChain` | 默认帧缓冲 | GLFW 管理 | `glfwSwapBuffers` |
| `RHIFence` | `GLsync` | `glFenceSync` | 同步对象 |
| `RHICommandContext` | `GLCommandList` | 命令记录 | 伪命令缓冲区 |
| `RHIHeap` | ❌ 不支持 | - | 返回空实现或模拟 |

### 4.2 格式映射

```cpp
// OpenGLConversions.h
inline GLenum ToGLInternalFormat(RHIFormat format)
{
    switch (format)
    {
        // 8-bit
        case RHIFormat::R8_UNORM:           return GL_R8;
        case RHIFormat::R8_SNORM:           return GL_R8_SNORM;
        case RHIFormat::R8_UINT:            return GL_R8UI;
        case RHIFormat::R8_SINT:            return GL_R8I;

        // 16-bit
        case RHIFormat::R16_FLOAT:          return GL_R16F;
        case RHIFormat::RG8_UNORM:          return GL_RG8;

        // 32-bit
        case RHIFormat::R32_FLOAT:          return GL_R32F;
        case RHIFormat::R32_UINT:           return GL_R32UI;
        case RHIFormat::RGBA8_UNORM:        return GL_RGBA8;
        case RHIFormat::RGBA8_UNORM_SRGB:   return GL_SRGB8_ALPHA8;
        case RHIFormat::BGRA8_UNORM:        return GL_RGBA8;  // 注意: OpenGL 无原生 BGRA internal
        case RHIFormat::RGB10A2_UNORM:      return GL_RGB10_A2;
        case RHIFormat::RG11B10_FLOAT:      return GL_R11F_G11F_B10F;

        // 64-bit
        case RHIFormat::RG32_FLOAT:         return GL_RG32F;
        case RHIFormat::RGBA16_FLOAT:       return GL_RGBA16F;

        // 128-bit
        case RHIFormat::RGBA32_FLOAT:       return GL_RGBA32F;

        // Depth-Stencil
        case RHIFormat::D16_UNORM:          return GL_DEPTH_COMPONENT16;
        case RHIFormat::D24_UNORM_S8_UINT:  return GL_DEPTH24_STENCIL8;
        case RHIFormat::D32_FLOAT:          return GL_DEPTH_COMPONENT32F;
        case RHIFormat::D32_FLOAT_S8_UINT:  return GL_DEPTH32F_STENCIL8;

        // Compressed (BC/DXT)
        case RHIFormat::BC1_UNORM:          return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        case RHIFormat::BC1_UNORM_SRGB:     return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT;
        case RHIFormat::BC3_UNORM:          return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        case RHIFormat::BC3_UNORM_SRGB:     return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
        case RHIFormat::BC7_UNORM:          return GL_COMPRESSED_RGBA_BPTC_UNORM;
        case RHIFormat::BC7_UNORM_SRGB:     return GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;

        default:
            return GL_RGBA8;
    }
}

inline GLenum ToGLFormat(RHIFormat format)
{
    if (IsDepthFormat(format))
    {
        if (IsStencilFormat(format))
            return GL_DEPTH_STENCIL;
        return GL_DEPTH_COMPONENT;
    }
    
    // 根据分量数返回
    switch (format)
    {
        case RHIFormat::R8_UNORM:
        case RHIFormat::R16_FLOAT:
        case RHIFormat::R32_FLOAT:
            return GL_RED;
        
        case RHIFormat::RG8_UNORM:
        case RHIFormat::RG16_FLOAT:
        case RHIFormat::RG32_FLOAT:
            return GL_RG;
        
        case RHIFormat::RGB32_FLOAT:
            return GL_RGB;
            
        default:
            return GL_RGBA;
    }
}

inline GLenum ToGLType(RHIFormat format)
{
    switch (format)
    {
        case RHIFormat::R8_UNORM:
        case RHIFormat::RG8_UNORM:
        case RHIFormat::RGBA8_UNORM:
        case RHIFormat::RGBA8_UNORM_SRGB:
            return GL_UNSIGNED_BYTE;
            
        case RHIFormat::R16_FLOAT:
        case RHIFormat::RG16_FLOAT:
        case RHIFormat::RGBA16_FLOAT:
            return GL_HALF_FLOAT;
            
        case RHIFormat::R32_FLOAT:
        case RHIFormat::RG32_FLOAT:
        case RHIFormat::RGB32_FLOAT:
        case RHIFormat::RGBA32_FLOAT:
            return GL_FLOAT;
            
        case RHIFormat::D16_UNORM:
            return GL_UNSIGNED_SHORT;
        case RHIFormat::D24_UNORM_S8_UINT:
            return GL_UNSIGNED_INT_24_8;
        case RHIFormat::D32_FLOAT:
            return GL_FLOAT;
        case RHIFormat::D32_FLOAT_S8_UINT:
            return GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
            
        default:
            return GL_UNSIGNED_BYTE;
    }
}
```

### 4.3 比较函数/混合模式映射

```cpp
inline GLenum ToGLCompareOp(RHICompareOp op)
{
    switch (op)
    {
        case RHICompareOp::Never:        return GL_NEVER;
        case RHICompareOp::Less:         return GL_LESS;
        case RHICompareOp::Equal:        return GL_EQUAL;
        case RHICompareOp::LessEqual:    return GL_LEQUAL;
        case RHICompareOp::Greater:      return GL_GREATER;
        case RHICompareOp::NotEqual:     return GL_NOTEQUAL;
        case RHICompareOp::GreaterEqual: return GL_GEQUAL;
        case RHICompareOp::Always:       return GL_ALWAYS;
        default:                         return GL_LESS;
    }
}

inline GLenum ToGLBlendFactor(RHIBlendFactor factor)
{
    switch (factor)
    {
        case RHIBlendFactor::Zero:              return GL_ZERO;
        case RHIBlendFactor::One:               return GL_ONE;
        case RHIBlendFactor::SrcColor:          return GL_SRC_COLOR;
        case RHIBlendFactor::InvSrcColor:       return GL_ONE_MINUS_SRC_COLOR;
        case RHIBlendFactor::SrcAlpha:          return GL_SRC_ALPHA;
        case RHIBlendFactor::InvSrcAlpha:       return GL_ONE_MINUS_SRC_ALPHA;
        case RHIBlendFactor::DstColor:          return GL_DST_COLOR;
        case RHIBlendFactor::InvDstColor:       return GL_ONE_MINUS_DST_COLOR;
        case RHIBlendFactor::DstAlpha:          return GL_DST_ALPHA;
        case RHIBlendFactor::InvDstAlpha:       return GL_ONE_MINUS_DST_ALPHA;
        case RHIBlendFactor::SrcAlphaSaturate:  return GL_SRC_ALPHA_SATURATE;
        case RHIBlendFactor::ConstantColor:     return GL_CONSTANT_COLOR;
        case RHIBlendFactor::InvConstantColor:  return GL_ONE_MINUS_CONSTANT_COLOR;
        default:                                return GL_ONE;
    }
}

inline GLenum ToGLBlendOp(RHIBlendOp op)
{
    switch (op)
    {
        case RHIBlendOp::Add:             return GL_FUNC_ADD;
        case RHIBlendOp::Subtract:        return GL_FUNC_SUBTRACT;
        case RHIBlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case RHIBlendOp::Min:             return GL_MIN;
        case RHIBlendOp::Max:             return GL_MAX;
        default:                          return GL_FUNC_ADD;
    }
}

inline GLenum ToGLPrimitiveTopology(RHIPrimitiveTopology topology)
{
    switch (topology)
    {
        case RHIPrimitiveTopology::PointList:     return GL_POINTS;
        case RHIPrimitiveTopology::LineList:      return GL_LINES;
        case RHIPrimitiveTopology::LineStrip:     return GL_LINE_STRIP;
        case RHIPrimitiveTopology::TriangleList:  return GL_TRIANGLES;
        case RHIPrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
        default:                                  return GL_TRIANGLES;
    }
}
```

---

## 5. 着色器编译策略

### 5.1 编译管线

与 Metal 后端保持一致，使用统一 HLSL 源码：

```
┌─────────────────────────────────────────────────────────┐
│                    HLSL Source                          │
│              (统一着色器源码)                             │
└────────────────────────┬────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────┐
│                       DXC                                │
│              HLSL → SPIR-V 编译                          │
│         (dxc -spirv -T vs_6_0/ps_6_0)                   │
└────────────────────────┬────────────────────────────────┘
                         │
                         ▼
           ┌─────────────┴─────────────┐
           │                           │
           ▼                           ▼
┌──────────────────────┐   ┌──────────────────────┐
│    SPIRV-Cross       │   │  GL_ARB_gl_spirv     │
│   SPIR-V → GLSL      │   │   (OpenGL 4.6+)      │
│    (离线/运行时)       │   │  直接加载 SPIR-V     │
└──────────┬───────────┘   └──────────┬───────────┘
           │                           │
           ▼                           ▼
┌──────────────────────┐   ┌──────────────────────┐
│   glShaderSource()   │   │  glShaderBinary()    │
│   glCompileShader()  │   │  glSpecializeShader()|
└──────────────────────┘   └──────────────────────┘
```

### 5.2 SPIRV-Cross GLSL 翻译结构 (完整定义)

```cpp
// ShaderCompiler/Private/SPIRVCrossTranslator.h

// =============================================================================
// GLSL 翻译选项
// =============================================================================
struct SPIRVToGLSLOptions
{
    uint32 glslVersion = 450;          // GLSL 版本 (450 = OpenGL 4.5)
    bool es = false;                   // OpenGL ES (移动端用)
    bool vulkanSemantics = false;      // 保持 false
    bool enable420Pack = true;         // 启用 layout(binding=) 支持
    bool emitPushConstantAsUBO = true; // Push constant 转为 UBO
    bool flattenUniformArrays = false; // 是否展开 uniform 数组
    bool forceZeroInit = true;         // 强制零初始化防止未定义行为
    
    // 绑定点重映射配置
    uint32 uboBindingOffset = 0;       // UBO 起始绑定点
    uint32 ssboBindingOffset = 0;      // SSBO 起始绑定点
    uint32 textureBindingOffset = 0;   // 纹理起始绑定点
    uint32 samplerBindingOffset = 0;   // 采样器起始绑定点
    uint32 imageBindingOffset = 0;     // 存储图像起始绑定点
};

// =============================================================================
// 绑定点重映射信息 (关键！)
// =============================================================================
struct GLSLBindingRemap
{
    std::string name;           // 资源名称 (如 "PerFrameUBO")
    uint32 originalSet;         // 原始 descriptor set
    uint32 originalBinding;     // 原始 binding
    uint32 glBinding;           // OpenGL 绑定点
    RHIBindingType type;        // 资源类型
};

// =============================================================================
// GLSL 翻译结果
// =============================================================================
struct SPIRVToGLSLResult
{
    bool success = false;
    std::string glslSource;
    std::string errorMessage;
    ShaderReflection reflection;
    
    // 绑定点重映射表 (用于运行时绑定)
    std::vector<GLSLBindingRemap> bindingRemaps;
    
    // Push Constant 信息
    struct PushConstantInfo
    {
        std::string uboName;     // 转换后的 UBO 名称
        uint32 glBinding;        // OpenGL 绑定点
        uint32 size;             // 大小 (bytes)
    };
    std::optional<PushConstantInfo> pushConstantInfo;
};

// =============================================================================
// SPIRV-Cross GLSL 翻译器
// =============================================================================
class SPIRVCrossTranslator
{
public:
    // 翻译到 MSL (已有)
    SPIRVToMSLResult TranslateToMSL(...);
    
    // 翻译到 GLSL (新增)
    SPIRVToGLSLResult TranslateToGLSL(
        const std::vector<uint8_t>& spirvBytecode,
        RHIShaderStage stage,
        const char* entryPoint,
        const SPIRVToGLSLOptions& options = {});

private:
    // 提取并重映射绑定点
    std::vector<GLSLBindingRemap> ExtractAndRemapBindings(
        spirv_cross::CompilerGLSL& compiler,
        const SPIRVToGLSLOptions& options);
};
```

### 5.3 SPIRV-Cross GLSL 翻译实现

```cpp
// SPIRVCrossTranslator.cpp (扩展)
SPIRVToGLSLResult SPIRVCrossTranslator::TranslateToGLSL(
    const std::vector<uint8_t>& spirvBytecode,
    RHIShaderStage stage,
    const char* entryPoint,
    const SPIRVToGLSLOptions& options)
{
    SPIRVToGLSLResult result;
    
    try
    {
        spirv_cross::CompilerGLSL glslCompiler(
            reinterpret_cast<const uint32_t*>(spirvBytecode.data()),
            spirvBytecode.size() / sizeof(uint32_t)
        );
        
        // 配置 GLSL 编译选项
        spirv_cross::CompilerGLSL::Options glslOptions;
        glslOptions.version = options.glslVersion;
        glslOptions.es = options.es;
        glslOptions.vulkan_semantics = options.vulkanSemantics;
        glslOptions.enable_420pack_extension = options.enable420Pack;
        glslOptions.emit_push_constant_as_uniform_buffer = options.emitPushConstantAsUBO;
        glslOptions.emit_uniform_buffer_as_plain_uniforms = false;
        glslOptions.force_zero_initialized_variables = options.forceZeroInit;
        
        glslCompiler.set_common_options(glslOptions);
        
        // ⚠️ 关键：提取并重映射绑定点
        result.bindingRemaps = ExtractAndRemapBindings(glslCompiler, options);
        
        // 处理 Push Constants
        auto resources = glslCompiler.get_shader_resources();
        if (!resources.push_constant_buffers.empty())
        {
            auto& pc = resources.push_constant_buffers[0];
            auto& type = glslCompiler.get_type(pc.base_type_id);
            
            result.pushConstantInfo = SPIRVToGLSLResult::PushConstantInfo{
                .uboName = glslCompiler.get_name(pc.id),
                .glBinding = 0,  // Push constant UBO 固定在 binding 0
                .size = static_cast<uint32>(glslCompiler.get_declared_struct_size(type))
            };
        }
        
        // 编译生成 GLSL 源码
        result.glslSource = glslCompiler.compile();
        result.success = true;
        
        // 提取反射信息
        result.reflection = ExtractReflection(glslCompiler, stage);
    }
    catch (const spirv_cross::CompilerError& e)
    {
        result.errorMessage = e.what();
    }
    
    return result;
}

std::vector<GLSLBindingRemap> SPIRVCrossTranslator::ExtractAndRemapBindings(
    spirv_cross::CompilerGLSL& compiler,
    const SPIRVToGLSLOptions& options)
{
    std::vector<GLSLBindingRemap> remaps;
    auto resources = compiler.get_shader_resources();
    
    uint32 uboIndex = options.uboBindingOffset;
    uint32 ssboIndex = options.ssboBindingOffset;
    uint32 textureIndex = options.textureBindingOffset;
    uint32 samplerIndex = options.samplerBindingOffset;
    uint32 imageIndex = options.imageBindingOffset;
    
    // 处理 Uniform Buffers
    for (auto& ubo : resources.uniform_buffers)
    {
        uint32 set = compiler.get_decoration(ubo.id, spv::DecorationDescriptorSet);
        uint32 binding = compiler.get_decoration(ubo.id, spv::DecorationBinding);
        
        // 重映射到 OpenGL 绑定点
        compiler.set_decoration(ubo.id, spv::DecorationBinding, uboIndex);
        compiler.unset_decoration(ubo.id, spv::DecorationDescriptorSet);
        
        remaps.push_back({
            .name = compiler.get_name(ubo.id),
            .originalSet = set,
            .originalBinding = binding,
            .glBinding = uboIndex++,
            .type = RHIBindingType::UniformBuffer
        });
    }
    
    // 处理 Storage Buffers (SSBO)
    for (auto& ssbo : resources.storage_buffers)
    {
        uint32 set = compiler.get_decoration(ssbo.id, spv::DecorationDescriptorSet);
        uint32 binding = compiler.get_decoration(ssbo.id, spv::DecorationBinding);
        
        compiler.set_decoration(ssbo.id, spv::DecorationBinding, ssboIndex);
        compiler.unset_decoration(ssbo.id, spv::DecorationDescriptorSet);
        
        remaps.push_back({
            .name = compiler.get_name(ssbo.id),
            .originalSet = set,
            .originalBinding = binding,
            .glBinding = ssboIndex++,
            .type = RHIBindingType::StorageBuffer
        });
    }
    
    // 处理纹理 (Sampled Images)
    for (auto& tex : resources.sampled_images)
    {
        uint32 set = compiler.get_decoration(tex.id, spv::DecorationDescriptorSet);
        uint32 binding = compiler.get_decoration(tex.id, spv::DecorationBinding);
        
        compiler.set_decoration(tex.id, spv::DecorationBinding, textureIndex);
        compiler.unset_decoration(tex.id, spv::DecorationDescriptorSet);
        
        remaps.push_back({
            .name = compiler.get_name(tex.id),
            .originalSet = set,
            .originalBinding = binding,
            .glBinding = textureIndex++,
            .type = RHIBindingType::CombinedTextureSampler
        });
    }
    
    // 处理存储图像 (Storage Images)
    for (auto& img : resources.storage_images)
    {
        uint32 set = compiler.get_decoration(img.id, spv::DecorationDescriptorSet);
        uint32 binding = compiler.get_decoration(img.id, spv::DecorationBinding);
        
        compiler.set_decoration(img.id, spv::DecorationBinding, imageIndex);
        compiler.unset_decoration(img.id, spv::DecorationDescriptorSet);
        
        remaps.push_back({
            .name = compiler.get_name(img.id),
            .originalSet = set,
            .originalBinding = binding,
            .glBinding = imageIndex++,
            .type = RHIBindingType::StorageTexture
        });
    }
    
    // 处理独立采样器
    for (auto& sampler : resources.separate_samplers)
    {
        uint32 set = compiler.get_decoration(sampler.id, spv::DecorationDescriptorSet);
        uint32 binding = compiler.get_decoration(sampler.id, spv::DecorationBinding);
        
        compiler.set_decoration(sampler.id, spv::DecorationBinding, samplerIndex);
        compiler.unset_decoration(sampler.id, spv::DecorationDescriptorSet);
        
        remaps.push_back({
            .name = compiler.get_name(sampler.id),
            .originalSet = set,
            .originalBinding = binding,
            .glBinding = samplerIndex++,
            .type = RHIBindingType::Sampler
        });
    }
    
    return remaps;
}
```

### 5.4 ShaderCompileResult 扩展 (完整版)

```cpp
// ShaderCompiler/Include/ShaderCompiler/ShaderCompiler.h
struct ShaderCompileResult
{
    bool success = false;
    std::vector<uint8> bytecode;       // DXBC/DXIL/SPIR-V
    std::string errorMessage;
    uint64 permutationHash = 0;
    ShaderReflection reflection;

    // Metal-specific
    std::string mslSource;
    std::string mslEntryPoint;
    
    // OpenGL-specific (新增)
    std::string glslSource;            // GLSL 4.50 源码
    uint32 glslVersion = 450;          // GLSL 版本
    
    // ⚠️ 关键：GLSL 绑定点映射信息
    struct GLSLBindingInfo
    {
        // 资源名称 → OpenGL 绑定点的映射
        std::unordered_map<std::string, uint32> uboBindings;
        std::unordered_map<std::string, uint32> ssboBindings;
        std::unordered_map<std::string, uint32> textureBindings;
        std::unordered_map<std::string, uint32> samplerBindings;
        std::unordered_map<std::string, uint32> imageBindings;
        
        // (set, binding) → OpenGL 绑定点的映射 (用于运行时快速查找)
        std::unordered_map<uint64, uint32> setBindingToGLBinding;
        
        // 辅助函数
        static uint64 MakeKey(uint32 set, uint32 binding)
        {
            return (static_cast<uint64>(set) << 32) | binding;
        }
        
        uint32 GetGLBinding(uint32 set, uint32 binding) const
        {
            auto it = setBindingToGLBinding.find(MakeKey(set, binding));
            return it != setBindingToGLBinding.end() ? it->second : UINT32_MAX;
        }
    } glslBindings;
    
    // Push Constant 信息
    struct PushConstantBinding
    {
        uint32 glBinding = 0;    // OpenGL UBO 绑定点
        uint32 size = 0;         // 大小
    };
    std::optional<PushConstantBinding> glslPushConstant;
};
```

### 5.5 着色器变体处理

```cpp
// 针对 OpenGL 的特殊处理
std::string PreprocessGLSL(const std::string& glsl, const OpenGLCapabilities& caps)
{
    std::string processed = glsl;
    
    // 处理绑定点差异
    // HLSL: register(b0, space0) → GLSL: layout(binding = 0, std140)
    // 已在 SPIRV-Cross 翻译时处理
    
    // 处理采样器分离 (如果需要)
    // HLSL: Texture2D + SamplerState → GLSL: sampler2D (combined)
    // SPIRV-Cross 默认生成 combined sampler
    
    // 运行时特性检测调整
    if (!caps.opengl.hasSSBO)
    {
        // 降级 SSBO 到 UBO (有大小限制)
        // 或生成警告
    }
    
    return processed;
}
```

---

## 6. 模块结构设计

### 6.1 目录结构 (完整版)

```
RHI_OpenGL/
├── CMakeLists.txt
├── Include/
│   └── OpenGL/
│       └── OpenGLDevice.h                  # 公开设备接口
└── Private/
    ├── OpenGLCommon.h                      # 通用定义、错误检查宏、GL_CHECK
    ├── OpenGLConversions.h                 # RHI → OpenGL 类型转换
    │
    ├── OpenGLContext.h                     # [新增] OpenGL 上下文管理
    ├── OpenGLContext.cpp
    │
    ├── OpenGLStateCache.h                  # 状态缓存系统
    ├── OpenGLStateCache.cpp
    │
    ├── OpenGLDeletionQueue.h               # [新增] 资源延迟删除队列
    ├── OpenGLDeletionQueue.cpp
    │
    ├── OpenGLDevice.h                      # 设备类定义
    ├── OpenGLDevice.cpp                    # 设备实现
    │
    ├── OpenGLResources.h                   # Buffer, Texture, TextureView
    ├── OpenGLResources.cpp
    ├── OpenGLSampler.h                     # Sampler (分离文件便于维护)
    ├── OpenGLSampler.cpp
    │
    ├── OpenGLShader.h                      # Shader, Program
    ├── OpenGLShader.cpp
    │
    ├── OpenGLPipeline.h                    # Pipeline State
    ├── OpenGLPipeline.cpp
    │
    ├── OpenGLDescriptor.h                  # DescriptorSetLayout, DescriptorSet
    ├── OpenGLDescriptor.cpp
    ├── OpenGLPushConstants.h               # [新增] Push Constants UBO 封装
    ├── OpenGLPushConstants.cpp
    │
    ├── OpenGLFramebufferCache.h            # [新增] FBO 缓存管理
    ├── OpenGLFramebufferCache.cpp
    ├── OpenGLVAOCache.h                    # [新增] VAO 缓存管理
    ├── OpenGLVAOCache.cpp
    │
    ├── OpenGLCommandBuffer.h               # [新增] 线性命令缓冲区
    ├── OpenGLCommandBuffer.cpp
    ├── OpenGLCommandExecutor.h             # [新增] 命令执行器
    ├── OpenGLCommandExecutor.cpp
    ├── OpenGLCommandContext.h              # RHI CommandContext 实现
    ├── OpenGLCommandContext.cpp
    │
    ├── OpenGLSwapChain.h                   # SwapChain (GLFW integration)
    ├── OpenGLSwapChain.cpp
    │
    └── OpenGLSync.h                        # Fence (GLsync)
    └── OpenGLSync.cpp
```

### 6.2 CMakeLists.txt (完整版)

```cmake
# =============================================================================
# RHI_OpenGL Module - OpenGL backend implementation
# =============================================================================
add_library(RVX_RHI_OpenGL STATIC)

target_sources(RVX_RHI_OpenGL PRIVATE
    # 核心
    Private/OpenGLContext.cpp
    Private/OpenGLStateCache.cpp
    Private/OpenGLDeletionQueue.cpp
    Private/OpenGLDevice.cpp
    
    # 资源
    Private/OpenGLResources.cpp
    Private/OpenGLSampler.cpp
    
    # 着色器与管线
    Private/OpenGLShader.cpp
    Private/OpenGLPipeline.cpp
    Private/OpenGLDescriptor.cpp
    Private/OpenGLPushConstants.cpp
    
    # 缓存
    Private/OpenGLFramebufferCache.cpp
    Private/OpenGLVAOCache.cpp
    
    # 命令
    Private/OpenGLCommandBuffer.cpp
    Private/OpenGLCommandExecutor.cpp
    Private/OpenGLCommandContext.cpp
    
    # SwapChain 与同步
    Private/OpenGLSwapChain.cpp
    Private/OpenGLSync.cpp
)

target_include_directories(RVX_RHI_OpenGL PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(RVX_RHI_OpenGL PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(RVX_RHI_OpenGL PUBLIC
    RVX::RHI
    RVX::Core
    RVX::ShaderCompiler
)

# OpenGL Loader (glad)
find_package(glad CONFIG REQUIRED)
target_link_libraries(RVX_RHI_OpenGL PRIVATE glad::glad)

# GLFW for context/window
target_link_libraries(RVX_RHI_OpenGL PRIVATE glfw)

# Platform-specific OpenGL libraries
if(WIN32)
    target_link_libraries(RVX_RHI_OpenGL PRIVATE opengl32)
elseif(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(RVX_RHI_OpenGL PRIVATE OpenGL::GL)
endif()

# Compile definitions
target_compile_definitions(RVX_RHI_OpenGL PRIVATE
    RVX_OPENGL_BACKEND=1
)

# Debug 模式启用详细 GL 错误检查
target_compile_definitions(RVX_RHI_OpenGL PRIVATE
    $<$<CONFIG:Debug>:RVX_GL_DEBUG=1>
)

add_library(RVX::RHI_OpenGL ALIAS RVX_RHI_OpenGL)
```

---

## 7. 核心类设计

### 7.1 OpenGLDevice

```cpp
// OpenGLDevice.h
#pragma once

#include "RHI/RHIDevice.h"
#include "OpenGLCommon.h"
#include "OpenGLStateCache.h"

namespace RVX
{
    class OpenGLDevice : public IRHIDevice
    {
    public:
        OpenGLDevice(const RHIDeviceDesc& desc);
        ~OpenGLDevice() override;

        // =========================================================================
        // Resource Creation
        // =========================================================================
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override;
        RHITextureRef CreateTexture(const RHITextureDesc& desc) override;
        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc) override;
        RHISamplerRef CreateSampler(const RHISamplerDesc& desc) override;
        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override;

        // =========================================================================
        // Heap Management (Not supported in OpenGL - stub implementation)
        // =========================================================================
        RHIHeapRef CreateHeap(const RHIHeapDesc& desc) override;
        RHITextureRef CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc) override;
        RHIBufferRef CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc) override;
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc& desc) override;
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) override;

        // =========================================================================
        // Pipeline Creation
        // =========================================================================
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) override;
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override;
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override;
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) override;

        // =========================================================================
        // Descriptor Set
        // =========================================================================
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override;

        // =========================================================================
        // Command Context
        // =========================================================================
        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override;
        void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override;
        void SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence) override;

        // =========================================================================
        // SwapChain
        // =========================================================================
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) override;

        // =========================================================================
        // Synchronization
        // =========================================================================
        RHIFenceRef CreateFence(uint64 initialValue) override;
        void WaitForFence(RHIFence* fence, uint64 value) override;
        void WaitIdle() override;

        // =========================================================================
        // Frame Management
        // =========================================================================
        void BeginFrame() override;
        void EndFrame() override;
        uint32 GetCurrentFrameIndex() const override { return m_currentFrameIndex; }

        // =========================================================================
        // Capabilities
        // =========================================================================
        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::OpenGL; }

        // =========================================================================
        // OpenGL Specific
        // =========================================================================
        GLStateCache& GetStateCache() { return m_stateCache; }
        const OpenGLExtensions& GetExtensions() const { return m_extensions; }

    private:
        void QueryCapabilities();
        void LoadExtensions();

        RHICapabilities m_capabilities;
        OpenGLExtensions m_extensions;
        GLStateCache m_stateCache;
        uint32 m_currentFrameIndex = 0;
    };

    // Factory function
    std::unique_ptr<IRHIDevice> CreateOpenGLDevice(const RHIDeviceDesc& desc);

} // namespace RVX
```

### 7.2 OpenGLBuffer

```cpp
// OpenGLResources.h (部分)
class OpenGLBuffer : public RHIBuffer
{
public:
    OpenGLBuffer(const RHIBufferDesc& desc);
    ~OpenGLBuffer() override;

    uint64 GetSize() const override { return m_size; }
    RHIBufferUsage GetUsage() const override { return m_usage; }
    RHIMemoryType GetMemoryType() const override { return m_memoryType; }
    uint32 GetStride() const override { return m_stride; }

    void* Map() override;
    void Unmap() override;

    // OpenGL Specific
    GLuint GetGLBuffer() const { return m_buffer; }
    GLenum GetGLTarget() const;  // GL_ARRAY_BUFFER, GL_UNIFORM_BUFFER, etc.

private:
    GLuint m_buffer = 0;
    uint64 m_size = 0;
    RHIBufferUsage m_usage = RHIBufferUsage::None;
    RHIMemoryType m_memoryType = RHIMemoryType::Default;
    uint32 m_stride = 0;
    void* m_mappedPtr = nullptr;
};
```

### 7.3 OpenGLPipeline

```cpp
// OpenGLPipeline.h
class OpenGLPipeline : public RHIPipeline
{
public:
    OpenGLPipeline(const RHIGraphicsPipelineDesc& desc);
    OpenGLPipeline(const RHIComputePipelineDesc& desc);
    ~OpenGLPipeline() override;

    bool IsCompute() const override { return m_isCompute; }
    
    // OpenGL Specific
    GLuint GetProgram() const { return m_program; }
    
    // Apply non-program state to OpenGL context
    void ApplyState(GLStateCache& cache) const;
    
    // Vertex input configuration
    struct VertexAttribute
    {
        uint32 location;
        GLenum type;
        GLint size;
        GLboolean normalized;
        uint32 stride;
        uint32 offset;
        uint32 binding;
        bool perInstance;
    };
    
    const std::vector<VertexAttribute>& GetVertexAttributes() const { return m_vertexAttributes; }
    GLenum GetPrimitiveTopology() const { return m_primitiveTopology; }

private:
    GLuint m_program = 0;
    bool m_isCompute = false;
    
    // Cached state
    GLenum m_primitiveTopology = GL_TRIANGLES;
    std::vector<VertexAttribute> m_vertexAttributes;
    
    // Rasterizer state
    GLenum m_cullFace = GL_BACK;
    GLenum m_frontFace = GL_CCW;
    GLenum m_polygonMode = GL_FILL;
    bool m_cullEnabled = true;
    bool m_depthClampEnabled = false;
    
    // Depth-Stencil state
    bool m_depthTestEnabled = true;
    bool m_depthWriteEnabled = true;
    GLenum m_depthFunc = GL_LESS;
    bool m_stencilTestEnabled = false;
    // ... stencil ops
    
    // Blend state (per render target)
    struct BlendState
    {
        bool enabled = false;
        GLenum srcColor = GL_ONE;
        GLenum dstColor = GL_ZERO;
        GLenum colorOp = GL_FUNC_ADD;
        GLenum srcAlpha = GL_ONE;
        GLenum dstAlpha = GL_ZERO;
        GLenum alphaOp = GL_FUNC_ADD;
        uint8 colorWriteMask = 0xF;
    };
    std::array<BlendState, 8> m_blendStates;
};
```

### 7.4 OpenGLCommandContext (完整版)

```cpp
// OpenGLCommandContext.h
class OpenGLCommandContext : public RHICommandContext
{
public:
    OpenGLCommandContext(OpenGLDevice* device, RHICommandQueueType type);
    ~OpenGLCommandContext() override;

    void Begin() override;
    void End() override;
    void Reset() override;

    // Debug markers
    void BeginEvent(const char* name, uint32 color) override;
    void EndEvent() override;
    void SetMarker(const char* name, uint32 color) override;

    // Barriers (mostly no-op in OpenGL, but record for validation)
    void BufferBarrier(const RHIBufferBarrier& barrier) override;
    void TextureBarrier(const RHITextureBarrier& barrier) override;
    void Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                  std::span<const RHITextureBarrier> textureBarriers) override;

    // Render pass
    void BeginRenderPass(const RHIRenderPassDesc& desc) override;
    void EndRenderPass() override;

    // Pipeline binding
    void SetPipeline(RHIPipeline* pipeline) override;
    void SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset) override;
    void SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, std::span<const uint64> offsets) override;
    void SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset) override;

    // Descriptor binding
    void SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets) override;
    void SetPushConstants(const void* data, uint32 size, uint32 offset) override;

    // Viewport/Scissor
    void SetViewport(const RHIViewport& viewport) override;
    void SetViewports(std::span<const RHIViewport> viewports) override;
    void SetScissor(const RHIRect& scissor) override;
    void SetScissors(std::span<const RHIRect> scissors) override;

    // Draw commands
    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance) override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset, uint32 firstInstance) override;
    void DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;
    void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override;

    // Compute dispatch
    void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override;
    void DispatchIndirect(RHIBuffer* buffer, uint64 offset) override;

    // Copy operations
    void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size) override;
    void CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc) override;
    void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc) override;
    void CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc) override;

    // Execute all recorded commands
    void Execute();

private:
    OpenGLDevice* m_device = nullptr;
    RHICommandQueueType m_queueType;
    
    // ⚠️ 优化：使用线性命令缓冲区
    GLCommandBuffer m_commandBuffer;
    GLCommandExecutor m_executor;
    
    // Current state tracking (用于命令录制时)
    OpenGLPipeline* m_currentPipeline = nullptr;
    GLenum m_currentPrimitiveMode = GL_TRIANGLES;
    GLenum m_currentIndexType = GL_UNSIGNED_INT;
    
    // Vertex buffer bindings (延迟到 Draw 时创建 VAO)
    struct VertexBufferState
    {
        GLuint buffer = 0;
        uint64 offset = 0;
    };
    std::array<VertexBufferState, 8> m_vertexBuffers;
    uint32 m_vertexBufferCount = 0;
    GLuint m_indexBuffer = 0;
    uint64 m_indexBufferOffset = 0;
    
    // Render pass state
    bool m_inRenderPass = false;
    GLuint m_currentFBO = 0;
    uint32 m_renderWidth = 0;
    uint32 m_renderHeight = 0;
    
    // Push Constants 状态
    static constexpr size_t MAX_PUSH_CONSTANT_SIZE = 256;
    std::array<uint8, MAX_PUSH_CONSTANT_SIZE> m_pushConstantData = {};
    bool m_pushConstantsDirty = false;
};
```

### 7.5 Push Constants 实现

OpenGL 没有原生 Push Constants，需要通过 UBO 模拟。

```cpp
// OpenGLPushConstants.h
class OpenGLPushConstantBuffer
{
public:
    static constexpr uint32 PUSH_CONSTANT_BINDING = 0;  // 固定在 binding 0
    static constexpr size_t MAX_SIZE = 256;
    
    OpenGLPushConstantBuffer()
    {
        // 创建专用 UBO
        glCreateBuffers(1, &m_buffer);
        glNamedBufferStorage(m_buffer, MAX_SIZE, nullptr,
            GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT);
    }
    
    ~OpenGLPushConstantBuffer()
    {
        if (m_buffer != 0)
        {
            glDeleteBuffers(1, &m_buffer);
        }
    }
    
    void Update(const void* data, uint32 size, uint32 offset)
    {
        RVX_ASSERT(offset + size <= MAX_SIZE);
        glNamedBufferSubData(m_buffer, offset, size, data);
    }
    
    void Bind()
    {
        glBindBufferBase(GL_UNIFORM_BUFFER, PUSH_CONSTANT_BINDING, m_buffer);
    }
    
    GLuint GetBuffer() const { return m_buffer; }

private:
    GLuint m_buffer = 0;
};

// 在 OpenGLCommandContext 中使用
void OpenGLCommandContext::SetPushConstants(const void* data, uint32 size, uint32 offset)
{
    RVX_ASSERT(offset + size <= MAX_PUSH_CONSTANT_SIZE);
    
    // 记录到命令缓冲区
    auto* cmd = m_commandBuffer.Allocate<GLSetPushConstantsCommand>();
    cmd->offset = offset;
    cmd->size = size;
    memcpy(cmd->data + offset, data, size);
    
    // 同时更新本地缓存 (用于验证)
    memcpy(m_pushConstantData.data() + offset, data, size);
    m_pushConstantsDirty = true;
}

// 执行器中的实现
void GLCommandExecutor::ExecuteSetPushConstants(
    const GLSetPushConstantsCommand& cmd, 
    GLStateCache& cache)
{
    // 更新 Push Constant UBO
    m_pushConstantBuffer.Update(cmd.data + cmd.offset, cmd.size, cmd.offset);
    m_pushConstantBuffer.Bind();
}
```

### 7.6 OpenGLSwapChain (完整版)

```cpp
// OpenGLSwapChain.h
class OpenGLSwapChain : public RHISwapChain
{
public:
    OpenGLSwapChain(OpenGLDevice* device, const RHISwapChainDesc& desc);
    ~OpenGLSwapChain() override;

    uint32 GetWidth() const override { return m_width; }
    uint32 GetHeight() const override { return m_height; }
    RHIFormat GetFormat() const override { return m_format; }
    uint32 GetBufferCount() const override { return m_bufferCount; }
    
    // ⚠️ 关键：OpenGL 特殊处理
    // 方案 A: 返回代理纹理，Present 时 blit 到默认帧缓冲
    // 方案 B: 返回 nullptr，BeginRenderPass 特殊处理默认帧缓冲
    RHITexture* GetCurrentTexture() override;
    
    uint32 GetCurrentImageIndex() const override { return m_currentImageIndex; }
    
    void Resize(uint32 width, uint32 height) override;
    void Present() override;
    
    // OpenGL 特有
    GLFWwindow* GetWindow() const { return m_window; }
    bool IsDefaultFramebuffer() const { return m_useDefaultFramebuffer; }

private:
    void CreateProxyTextures();
    void DestroyProxyTextures();

    OpenGLDevice* m_device = nullptr;
    GLFWwindow* m_window = nullptr;
    
    uint32 m_width = 0;
    uint32 m_height = 0;
    RHIFormat m_format = RHIFormat::RGBA8_UNORM;
    uint32 m_bufferCount = 2;
    uint32 m_currentImageIndex = 0;
    
    // 代理纹理方案 (可选)
    bool m_useDefaultFramebuffer = true;  // 默认使用默认帧缓冲
    std::vector<RHITextureRef> m_proxyTextures;
    std::vector<GLuint> m_proxyFBOs;
};

// OpenGLSwapChain.cpp
OpenGLSwapChain::OpenGLSwapChain(OpenGLDevice* device, const RHISwapChainDesc& desc)
    : m_device(device)
    , m_window(static_cast<GLFWwindow*>(desc.windowHandle))
    , m_width(desc.width)
    , m_height(desc.height)
    , m_format(desc.format)
    , m_bufferCount(desc.bufferCount)
{
    // OpenGL 默认帧缓冲由 GLFW 管理
    // 如果需要与 RHI 纹理接口兼容，创建代理纹理
    
    // 方案 A: 使用代理纹理 (更兼容但有性能开销)
    if (!m_useDefaultFramebuffer)
    {
        CreateProxyTextures();
    }
    
    // 设置 VSync
    glfwSwapInterval(desc.enableVSync ? 1 : 0);
}

RHITexture* OpenGLSwapChain::GetCurrentTexture()
{
    if (m_useDefaultFramebuffer)
    {
        // 返回 nullptr，让调用者知道这是默认帧缓冲
        // BeginRenderPass 需要特殊处理这种情况
        return nullptr;
    }
    
    return m_proxyTextures[m_currentImageIndex].get();
}

void OpenGLSwapChain::Present()
{
    if (!m_useDefaultFramebuffer && !m_proxyTextures.empty())
    {
        // 将代理纹理 blit 到默认帧缓冲
        GLuint srcFBO = m_proxyFBOs[m_currentImageIndex];
        
        glBlitNamedFramebuffer(
            srcFBO, 0,  // src, dst (0 = default)
            0, 0, m_width, m_height,
            0, 0, m_width, m_height,
            GL_COLOR_BUFFER_BIT,
            GL_NEAREST
        );
    }
    
    // 交换缓冲区
    glfwSwapBuffers(m_window);
    
    // 更新当前图像索引
    m_currentImageIndex = (m_currentImageIndex + 1) % m_bufferCount;
}

void OpenGLSwapChain::Resize(uint32 width, uint32 height)
{
    if (m_width == width && m_height == height) return;
    
    m_width = width;
    m_height = height;
    
    // GLFW 会自动处理默认帧缓冲的 resize
    // 如果使用代理纹理，需要重新创建
    if (!m_useDefaultFramebuffer)
    {
        DestroyProxyTextures();
        CreateProxyTextures();
    }
}

void OpenGLSwapChain::CreateProxyTextures()
{
    for (uint32 i = 0; i < m_bufferCount; ++i)
    {
        RHITextureDesc texDesc = {};
        texDesc.width = m_width;
        texDesc.height = m_height;
        texDesc.format = m_format;
        texDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::CopySrc;
        texDesc.dimension = RHITextureDimension::Texture2D;
        
        m_proxyTextures.push_back(m_device->CreateTexture(texDesc));
        
        // 创建对应的 FBO
        GLuint fbo;
        glCreateFramebuffers(1, &fbo);
        
        auto* glTexture = static_cast<OpenGLTexture*>(m_proxyTextures.back().get());
        glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, glTexture->GetGLTexture(), 0);
        
        m_proxyFBOs.push_back(fbo);
    }
}
```

### 7.7 Debug 标记支持

```cpp
// 使用 GL_KHR_debug 实现调试标记
void OpenGLCommandContext::BeginEvent(const char* name, uint32 color)
{
    if (m_device->GetExtensions().GL_KHR_debug)
    {
        // 直接执行 (调试标记不需要延迟)
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
    }
}

void OpenGLCommandContext::EndEvent()
{
    if (m_device->GetExtensions().GL_KHR_debug)
    {
        glPopDebugGroup();
    }
}

void OpenGLCommandContext::SetMarker(const char* name, uint32 color)
{
    if (m_device->GetExtensions().GL_KHR_debug)
    {
        glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, 
                            GL_DEBUG_TYPE_MARKER, 
                            0, 
                            GL_DEBUG_SEVERITY_NOTIFICATION,
                            -1, name);
    }
}
```

---

## 8. OpenGL 上下文与线程安全

### 8.1 OpenGL 上下文管理

OpenGL 的核心限制是 **单线程上下文**。所有 GL 调用必须在创建上下文的线程执行。

```cpp
// OpenGLContext.h
#pragma once

#include <GLFW/glfw3.h>
#include <thread>
#include <mutex>

namespace RVX
{
    class OpenGLContext
    {
    public:
        // 初始化主上下文 (必须在主线程调用)
        static bool Initialize(GLFWwindow* window);
        
        // 使上下文在当前线程生效
        static void MakeCurrent(GLFWwindow* window);
        
        // 解绑当前上下文
        static void MakeNoneCurrent();
        
        // 检查是否在 GL 线程
        static bool IsOnGLThread()
        {
            return std::this_thread::get_id() == s_glThreadId;
        }
        
        // 获取 GL 线程 ID
        static std::thread::id GetGLThreadId() { return s_glThreadId; }
        
        // 创建共享上下文 (用于资源加载线程，可选)
        static GLFWwindow* CreateSharedContext();

    private:
        static bool LoadGLFunctions();  // glad 初始化
        
        static std::thread::id s_glThreadId;
        static bool s_initialized;
    };

} // namespace RVX
```

```cpp
// OpenGLContext.cpp
#include "OpenGLContext.h"
#include <glad/glad.h>

namespace RVX
{
    std::thread::id OpenGLContext::s_glThreadId;
    bool OpenGLContext::s_initialized = false;

    bool OpenGLContext::Initialize(GLFWwindow* window)
    {
        if (s_initialized) return true;
        
        // 设置 GL 线程
        s_glThreadId = std::this_thread::get_id();
        
        // 使上下文当前
        glfwMakeContextCurrent(window);
        
        // 加载 OpenGL 函数
        if (!LoadGLFunctions())
        {
            RVX_RHI_ERROR("Failed to load OpenGL functions");
            return false;
        }
        
        // 打印 OpenGL 信息
        RVX_RHI_INFO("OpenGL Version: {}", (const char*)glGetString(GL_VERSION));
        RVX_RHI_INFO("GLSL Version: {}", (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
        RVX_RHI_INFO("Renderer: {}", (const char*)glGetString(GL_RENDERER));
        RVX_RHI_INFO("Vendor: {}", (const char*)glGetString(GL_VENDOR));
        
        // 验证 OpenGL 版本
        GLint major, minor;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        
        if (major < 4 || (major == 4 && minor < 5))
        {
            RVX_RHI_ERROR("OpenGL 4.5 or higher required, got {}.{}", major, minor);
            return false;
        }
        
        s_initialized = true;
        return true;
    }

    bool OpenGLContext::LoadGLFunctions()
    {
        return gladLoadGLLoader((GLADloadproc)glfwGetProcAddress) != 0;
    }
    
    void OpenGLContext::MakeCurrent(GLFWwindow* window)
    {
        glfwMakeContextCurrent(window);
    }
    
    void OpenGLContext::MakeNoneCurrent()
    {
        glfwMakeContextCurrent(nullptr);
    }

} // namespace RVX
```

### 8.2 线程安全处理策略

由于 OpenGL 的单线程限制，从其他线程调用需要特殊处理：

```cpp
// OpenGLDevice 中的线程安全机制
class OpenGLDevice : public IRHIDevice
{
public:
    // =========================================================================
    // 线程安全：延迟执行队列
    // =========================================================================
    
    // 检查是否在 GL 线程
    bool IsOnGLThread() const
    {
        return OpenGLContext::IsOnGLThread();
    }
    
    // 从任意线程延迟到 GL 线程执行
    template<typename F>
    void DeferToGLThread(F&& func)
    {
        if (IsOnGLThread())
        {
            func();
        }
        else
        {
            std::lock_guard lock(m_pendingMutex);
            m_pendingCommands.push_back(std::forward<F>(func));
        }
    }
    
    // 在 GL 线程执行所有待处理命令
    void FlushPendingCommands()
    {
        RVX_ASSERT(IsOnGLThread() && "FlushPendingCommands must be called on GL thread");
        
        std::vector<std::function<void()>> commands;
        {
            std::lock_guard lock(m_pendingMutex);
            commands = std::move(m_pendingCommands);
        }
        
        for (auto& cmd : commands)
        {
            cmd();
        }
    }

private:
    mutable std::mutex m_pendingMutex;
    std::vector<std::function<void()>> m_pendingCommands;
};
```

### 8.3 帧生命周期中的线程处理

```cpp
void OpenGLDevice::BeginFrame()
{
    RVX_ASSERT(IsOnGLThread());
    
    // 处理来自其他线程的待处理命令
    FlushPendingCommands();
    
    // 处理延迟删除的资源
    m_deletionQueue.ProcessDeletions(m_currentFrameIndex);
    
    // 重置状态缓存
    m_stateCache.Invalidate();
}

void OpenGLDevice::EndFrame()
{
    RVX_ASSERT(IsOnGLThread());
    
    // 确保所有 GL 命令已提交
    glFlush();
    
    m_currentFrameIndex = (m_currentFrameIndex + 1) % RVX_MAX_FRAME_COUNT;
}
```

---

## 9. 资源生命周期管理

### 9.1 延迟删除队列

OpenGL 资源删除必须在 GL 线程执行，且需要确保 GPU 不再使用该资源。

```cpp
// OpenGLDeletionQueue.h
#pragma once

#include <mutex>
#include <vector>
#include <glad/glad.h>

namespace RVX
{
    class OpenGLDeletionQueue
    {
    public:
        // 资源类型
        enum class ResourceType
        {
            Buffer,
            Texture,
            Sampler,
            Program,
            Shader,
            VAO,
            FBO,
            Sync
        };
        
        // 将资源加入删除队列
        void QueueDeletion(ResourceType type, GLuint handle, uint32 frameQueued)
        {
            std::lock_guard lock(m_mutex);
            m_pendingDeletions.push_back({type, handle, frameQueued});
        }
        
        // 处理删除 (在 GL 线程，帧开始时调用)
        void ProcessDeletions(uint32 currentFrame)
        {
            std::lock_guard lock(m_mutex);
            
            // 删除 N 帧前的资源 (确保 GPU 不再使用)
            constexpr uint32 FRAME_DELAY = 3;
            
            auto it = std::remove_if(m_pendingDeletions.begin(), m_pendingDeletions.end(),
                [currentFrame, FRAME_DELAY, this](const PendingDeletion& item) {
                    // 处理帧计数环绕
                    uint32 framesSince = (currentFrame >= item.frameQueued) 
                        ? (currentFrame - item.frameQueued)
                        : (RVX_MAX_FRAME_COUNT - item.frameQueued + currentFrame);
                    
                    if (framesSince >= FRAME_DELAY)
                    {
                        DeleteResource(item.type, item.handle);
                        return true;
                    }
                    return false;
                });
            
            m_pendingDeletions.erase(it, m_pendingDeletions.end());
        }
        
        // 强制删除所有资源 (shutdown 时调用)
        void FlushAll()
        {
            std::lock_guard lock(m_mutex);
            for (auto& item : m_pendingDeletions)
            {
                DeleteResource(item.type, item.handle);
            }
            m_pendingDeletions.clear();
        }

    private:
        void DeleteResource(ResourceType type, GLuint handle)
        {
            switch (type)
            {
                case ResourceType::Buffer:
                    glDeleteBuffers(1, &handle);
                    break;
                case ResourceType::Texture:
                    glDeleteTextures(1, &handle);
                    break;
                case ResourceType::Sampler:
                    glDeleteSamplers(1, &handle);
                    break;
                case ResourceType::Program:
                    glDeleteProgram(handle);
                    break;
                case ResourceType::Shader:
                    glDeleteShader(handle);
                    break;
                case ResourceType::VAO:
                    glDeleteVertexArrays(1, &handle);
                    break;
                case ResourceType::FBO:
                    glDeleteFramebuffers(1, &handle);
                    break;
                case ResourceType::Sync:
                    glDeleteSync(reinterpret_cast<GLsync>(static_cast<uintptr_t>(handle)));
                    break;
            }
        }
        
        struct PendingDeletion
        {
            ResourceType type;
            GLuint handle;
            uint32 frameQueued;
        };
        
        std::mutex m_mutex;
        std::vector<PendingDeletion> m_pendingDeletions;
    };

} // namespace RVX
```

### 9.2 资源析构处理

```cpp
// OpenGLResources.cpp

OpenGLBuffer::~OpenGLBuffer()
{
    if (m_buffer != 0)
    {
        // 不直接删除，加入删除队列
        m_device->GetDeletionQueue().QueueDeletion(
            OpenGLDeletionQueue::ResourceType::Buffer,
            m_buffer,
            m_device->GetCurrentFrameIndex()
        );
        m_buffer = 0;
    }
}

OpenGLTexture::~OpenGLTexture()
{
    if (m_texture != 0)
    {
        m_device->GetDeletionQueue().QueueDeletion(
            OpenGLDeletionQueue::ResourceType::Texture,
            m_texture,
            m_device->GetCurrentFrameIndex()
        );
        m_texture = 0;
    }
}
```

---

## 10. FBO 与 VAO 缓存策略

### 10.1 Framebuffer Object (FBO) 缓存

FBO 是 RenderPass 实现的核心，需要高效的缓存策略。

```cpp
// OpenGLFramebufferCache.h
#pragma once

#include <unordered_map>
#include <array>
#include <glad/glad.h>

namespace RVX
{
    // FBO 缓存键
    struct FBOCacheKey
    {
        std::array<GLuint, 8> colorAttachments = {};      // 颜色附件纹理
        std::array<uint32, 8> colorMipLevels = {};        // 颜色附件 mip 级别
        std::array<uint32, 8> colorArraySlices = {};      // 颜色附件数组切片
        uint32 colorAttachmentCount = 0;
        
        GLuint depthStencilAttachment = 0;
        uint32 depthMipLevel = 0;
        uint32 depthArraySlice = 0;
        
        uint32 width = 0;
        uint32 height = 0;
        
        bool operator==(const FBOCacheKey& other) const
        {
            return colorAttachments == other.colorAttachments &&
                   colorMipLevels == other.colorMipLevels &&
                   colorArraySlices == other.colorArraySlices &&
                   colorAttachmentCount == other.colorAttachmentCount &&
                   depthStencilAttachment == other.depthStencilAttachment &&
                   depthMipLevel == other.depthMipLevel &&
                   depthArraySlice == other.depthArraySlice;
        }
    };
    
    struct FBOCacheKeyHash
    {
        size_t operator()(const FBOCacheKey& key) const
        {
            size_t hash = 0;
            for (uint32 i = 0; i < key.colorAttachmentCount; ++i)
            {
                hash ^= std::hash<GLuint>{}(key.colorAttachments[i]) << (i * 4);
                hash ^= std::hash<uint32>{}(key.colorMipLevels[i]) << (i * 2 + 1);
            }
            hash ^= std::hash<GLuint>{}(key.depthStencilAttachment) << 16;
            return hash;
        }
    };

    class OpenGLFramebufferCache
    {
    public:
        explicit OpenGLFramebufferCache(class OpenGLDevice* device)
            : m_device(device) {}
        
        ~OpenGLFramebufferCache()
        {
            Clear();
        }
        
        // 获取或创建 FBO
        GLuint GetOrCreateFBO(const RHIRenderPassDesc& desc);
        
        // 当纹理销毁时，使相关 FBO 失效
        void InvalidateTexture(GLuint texture);
        
        // 清理长时间未使用的 FBO (LRU)
        void Cleanup(uint32 currentFrame);
        
        // 清除所有缓存
        void Clear();

    private:
        GLuint CreateFBO(const FBOCacheKey& key, const RHIRenderPassDesc& desc);
        FBOCacheKey MakeKey(const RHIRenderPassDesc& desc);
        
        struct CachedFBO
        {
            GLuint fbo = 0;
            uint32 lastUsedFrame = 0;
        };
        
        OpenGLDevice* m_device;
        std::unordered_map<FBOCacheKey, CachedFBO, FBOCacheKeyHash> m_cache;
        
        // 纹理 → 使用它的 FBO 的反向映射
        std::unordered_multimap<GLuint, FBOCacheKey> m_textureToFBOs;
        
        static constexpr uint32 MAX_UNUSED_FRAMES = 60;  // 60 帧未使用则清理
    };

} // namespace RVX
```

```cpp
// OpenGLFramebufferCache.cpp
GLuint OpenGLFramebufferCache::GetOrCreateFBO(const RHIRenderPassDesc& desc)
{
    // 检查是否渲染到默认帧缓冲 (SwapChain)
    bool isDefaultFramebuffer = true;
    for (const auto& rt : desc.colorAttachments)
    {
        if (rt.texture != nullptr)
        {
            isDefaultFramebuffer = false;
            break;
        }
    }
    if (desc.depthStencilAttachment.texture != nullptr)
    {
        isDefaultFramebuffer = false;
    }
    
    if (isDefaultFramebuffer)
    {
        return 0;  // 返回默认帧缓冲
    }
    
    FBOCacheKey key = MakeKey(desc);
    
    auto it = m_cache.find(key);
    if (it != m_cache.end())
    {
        it->second.lastUsedFrame = m_device->GetCurrentFrameIndex();
        return it->second.fbo;
    }
    
    // 创建新 FBO
    GLuint fbo = CreateFBO(key, desc);
    m_cache[key] = {fbo, m_device->GetCurrentFrameIndex()};
    
    // 建立反向映射
    for (uint32 i = 0; i < key.colorAttachmentCount; ++i)
    {
        if (key.colorAttachments[i] != 0)
        {
            m_textureToFBOs.emplace(key.colorAttachments[i], key);
        }
    }
    if (key.depthStencilAttachment != 0)
    {
        m_textureToFBOs.emplace(key.depthStencilAttachment, key);
    }
    
    return fbo;
}

GLuint OpenGLFramebufferCache::CreateFBO(const FBOCacheKey& key, const RHIRenderPassDesc& desc)
{
    GLuint fbo;
    glCreateFramebuffers(1, &fbo);  // DSA
    
    // 绑定颜色附件
    std::vector<GLenum> drawBuffers;
    for (uint32 i = 0; i < key.colorAttachmentCount; ++i)
    {
        if (key.colorAttachments[i] != 0)
        {
            auto* texture = static_cast<OpenGLTexture*>(desc.colorAttachments[i].texture);
            
            if (key.colorArraySlices[i] > 0 || texture->IsArray())
            {
                glNamedFramebufferTextureLayer(fbo, GL_COLOR_ATTACHMENT0 + i,
                    key.colorAttachments[i], key.colorMipLevels[i], key.colorArraySlices[i]);
            }
            else
            {
                glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + i,
                    key.colorAttachments[i], key.colorMipLevels[i]);
            }
            
            drawBuffers.push_back(GL_COLOR_ATTACHMENT0 + i);
        }
    }
    
    // 设置 draw buffers
    if (!drawBuffers.empty())
    {
        glNamedFramebufferDrawBuffers(fbo, static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
    }
    
    // 绑定深度/模板附件
    if (key.depthStencilAttachment != 0)
    {
        auto* texture = static_cast<OpenGLTexture*>(desc.depthStencilAttachment.texture);
        GLenum attachment = texture->HasStencil() ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        
        if (key.depthArraySlice > 0 || texture->IsArray())
        {
            glNamedFramebufferTextureLayer(fbo, attachment,
                key.depthStencilAttachment, key.depthMipLevel, key.depthArraySlice);
        }
        else
        {
            glNamedFramebufferTexture(fbo, attachment,
                key.depthStencilAttachment, key.depthMipLevel);
        }
    }
    
    // 验证 FBO 完整性
    GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        RVX_RHI_ERROR("Framebuffer incomplete: {:#x}", status);
        glDeleteFramebuffers(1, &fbo);
        return 0;
    }
    
    return fbo;
}

void OpenGLFramebufferCache::InvalidateTexture(GLuint texture)
{
    auto range = m_textureToFBOs.equal_range(texture);
    for (auto it = range.first; it != range.second; ++it)
    {
        auto cacheIt = m_cache.find(it->second);
        if (cacheIt != m_cache.end())
        {
            glDeleteFramebuffers(1, &cacheIt->second.fbo);
            m_cache.erase(cacheIt);
        }
    }
    m_textureToFBOs.erase(texture);
}
```

### 10.2 Vertex Array Object (VAO) 缓存

VAO 封装了顶点输入配置，需要基于 Pipeline 和 VBO 绑定组合进行缓存。

```cpp
// OpenGLVAOCache.h
#pragma once

#include <unordered_map>
#include <array>
#include <glad/glad.h>

namespace RVX
{
    // VAO 缓存键
    struct VAOCacheKey
    {
        // Pipeline 的顶点布局 hash
        uint64 pipelineVertexLayoutHash = 0;
        
        // 顶点缓冲区绑定
        struct VertexBufferBinding
        {
            GLuint buffer = 0;
            GLintptr offset = 0;
            GLsizei stride = 0;
            
            bool operator==(const VertexBufferBinding& other) const
            {
                return buffer == other.buffer &&
                       offset == other.offset &&
                       stride == other.stride;
            }
        };
        std::array<VertexBufferBinding, 8> vertexBuffers = {};
        uint32 vertexBufferCount = 0;
        
        // 索引缓冲区
        GLuint indexBuffer = 0;
        
        bool operator==(const VAOCacheKey& other) const
        {
            if (pipelineVertexLayoutHash != other.pipelineVertexLayoutHash) return false;
            if (vertexBufferCount != other.vertexBufferCount) return false;
            if (indexBuffer != other.indexBuffer) return false;
            
            for (uint32 i = 0; i < vertexBufferCount; ++i)
            {
                if (!(vertexBuffers[i] == other.vertexBuffers[i])) return false;
            }
            return true;
        }
    };
    
    struct VAOCacheKeyHash
    {
        size_t operator()(const VAOCacheKey& key) const
        {
            size_t hash = std::hash<uint64>{}(key.pipelineVertexLayoutHash);
            for (uint32 i = 0; i < key.vertexBufferCount; ++i)
            {
                hash ^= std::hash<GLuint>{}(key.vertexBuffers[i].buffer) << (i * 3);
                hash ^= std::hash<GLintptr>{}(key.vertexBuffers[i].offset) << (i * 2 + 1);
            }
            hash ^= std::hash<GLuint>{}(key.indexBuffer) << 24;
            return hash;
        }
    };

    class OpenGLVAOCache
    {
    public:
        explicit OpenGLVAOCache(class OpenGLDevice* device)
            : m_device(device) {}
        
        ~OpenGLVAOCache() { Clear(); }
        
        // 获取或创建 VAO
        GLuint GetOrCreateVAO(
            const OpenGLPipeline* pipeline,
            const std::array<VertexBufferBinding, 8>& vertexBuffers,
            uint32 vertexBufferCount,
            GLuint indexBuffer);
        
        // 当缓冲区销毁时，使相关 VAO 失效
        void InvalidateBuffer(GLuint buffer);
        
        // 清理
        void Clear();

    private:
        GLuint CreateVAO(const VAOCacheKey& key, const OpenGLPipeline* pipeline);
        
        OpenGLDevice* m_device;
        std::unordered_map<VAOCacheKey, GLuint, VAOCacheKeyHash> m_cache;
        std::unordered_multimap<GLuint, VAOCacheKey> m_bufferToVAOs;  // 反向映射
    };

} // namespace RVX
```

```cpp
// OpenGLVAOCache.cpp
GLuint OpenGLVAOCache::CreateVAO(const VAOCacheKey& key, const OpenGLPipeline* pipeline)
{
    GLuint vao;
    glCreateVertexArrays(1, &vao);  // DSA
    
    // 配置顶点属性
    for (const auto& attr : pipeline->GetVertexAttributes())
    {
        uint32 bindingIndex = attr.binding;
        
        if (bindingIndex >= key.vertexBufferCount) continue;
        
        const auto& vbBinding = key.vertexBuffers[bindingIndex];
        
        // 绑定顶点缓冲区到绑定点
        glVertexArrayVertexBuffer(vao, bindingIndex,
            vbBinding.buffer, vbBinding.offset, attr.stride);
        
        // 启用顶点属性
        glEnableVertexArrayAttrib(vao, attr.location);
        
        // 设置属性格式
        if (attr.type == GL_INT || attr.type == GL_UNSIGNED_INT)
        {
            glVertexArrayAttribIFormat(vao, attr.location,
                attr.size, attr.type, attr.offset);
        }
        else
        {
            glVertexArrayAttribFormat(vao, attr.location,
                attr.size, attr.type, attr.normalized, attr.offset);
        }
        
        // 关联属性到绑定点
        glVertexArrayAttribBinding(vao, attr.location, bindingIndex);
        
        // 设置实例化分频
        if (attr.perInstance)
        {
            glVertexArrayBindingDivisor(vao, bindingIndex, 1);
        }
    }
    
    // 绑定索引缓冲区 (如果有)
    if (key.indexBuffer != 0)
    {
        glVertexArrayElementBuffer(vao, key.indexBuffer);
    }
    
    return vao;
}
```

---

## 11. 现有代码修改清单

### 11.1 RHI/Include/RHI/RHIDefinitions.h (新增 OpenGL)

```cpp
enum class RHIBackendType : uint8
{
    None = 0,
    DX11,
    DX12,
    Vulkan,
    Metal,
    OpenGL    // 新增
};

inline const char* ToString(RHIBackendType type)
{
    switch (type)
    {
        case RHIBackendType::DX11:   return "DirectX 11";
        case RHIBackendType::DX12:   return "DirectX 12";
        case RHIBackendType::Vulkan: return "Vulkan";
        case RHIBackendType::Metal:  return "Metal";
        case RHIBackendType::OpenGL: return "OpenGL";  // 新增
        default:                     return "Unknown";
    }
}
```

### 11.2 RHI/Include/RHI/RHICapabilities.h

```cpp
struct RHICapabilities
{
    // 现有字段...

    // OpenGL-specific (新增 - 完整版)
    struct OpenGLSpecific
    {
        // 版本信息
        uint32 majorVersion = 0;       // e.g., 4
        uint32 minorVersion = 0;       // e.g., 5
        bool coreProfile = true;
        std::string renderer;          // GPU 名称
        std::string vendor;            // 厂商名称
        std::string glslVersion;       // GLSL 版本字符串
        
        // 核心特性检测
        bool hasDSA = false;           // Direct State Access (4.5+)
        bool hasARBSpirv = false;      // GL_ARB_gl_spirv (4.6+)
        bool hasBindlessTexture = false;
        bool hasComputeShader = false; // 4.3+
        bool hasSSBO = false;          // 4.3+
        bool hasMultiBind = false;     // 4.4+
        bool hasTextureView = false;   // GL_ARB_texture_view (4.3+)
        bool hasBufferStorage = false; // GL_ARB_buffer_storage (4.4+)
        bool hasSeparateShaderObjects = false;  // 分离着色器对象
        bool hasDebugOutput = false;   // GL_KHR_debug
        bool hasPersistentMapping = false;  // 持久映射 (4.4+)
        
        // ⚠️ 关键：绑定点限制 (运行时查询)
        uint32 maxUniformBufferBindings = 14;   // GL_MAX_UNIFORM_BUFFER_BINDINGS
        uint32 maxTextureUnits = 16;            // GL_MAX_TEXTURE_IMAGE_UNITS
        uint32 maxImageUnits = 8;               // GL_MAX_IMAGE_UNITS
        uint32 maxSSBOBindings = 8;             // GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
        uint32 maxVertexAttribs = 16;           // GL_MAX_VERTEX_ATTRIBS
        uint32 maxUniformBlockSize = 65536;     // GL_MAX_UNIFORM_BLOCK_SIZE
        uint32 maxSSBOSize = 0;                 // GL_MAX_SHADER_STORAGE_BLOCK_SIZE
        uint32 maxColorAttachments = 8;         // GL_MAX_COLOR_ATTACHMENTS
        
        // 计算着色器限制
        uint32 maxComputeWorkGroupCountX = 65535;
        uint32 maxComputeWorkGroupCountY = 65535;
        uint32 maxComputeWorkGroupCountZ = 65535;
        uint32 maxComputeWorkGroupSizeX = 1024;
        uint32 maxComputeWorkGroupSizeY = 1024;
        uint32 maxComputeWorkGroupSizeZ = 64;
        uint32 maxComputeSharedMemorySize = 32768;
    } opengl;
};
```

### 11.3 RHI/Private/RHIModule.cpp (新增 OpenGL)

```cpp
#if RVX_ENABLE_OPENGL
namespace RVX { std::unique_ptr<IRHIDevice> CreateOpenGLDevice(const RHIDeviceDesc& desc); }
#endif

std::unique_ptr<IRHIDevice> CreateRHIDevice(RHIBackendType backend, const RHIDeviceDesc& desc)
{
    switch (backend)
    {
        // 现有 cases...

#if RVX_ENABLE_OPENGL
        case RHIBackendType::OpenGL:
            return CreateOpenGLDevice(desc);
#endif

        default:
            RVX_RHI_ERROR("Unsupported backend: {}", ToString(backend));
            return nullptr;
    }
}
```

### 11.4 RHI/CMakeLists.txt (新增编译定义)

```cmake
# 现有内容...

if(RVX_ENABLE_OPENGL)
    target_compile_definitions(RVX_RHI PUBLIC RVX_ENABLE_OPENGL=1)
endif()
```

### 11.5 CMakeLists.txt (根目录更新)

```cmake
# Build Options
option(RVX_ENABLE_DX11 "Enable DirectX 11 backend" ON)
option(RVX_ENABLE_DX12 "Enable DirectX 12 backend" ON)
option(RVX_ENABLE_VULKAN "Enable Vulkan backend" ON)
option(RVX_ENABLE_METAL "Enable Metal backend (macOS/iOS)" OFF)
option(RVX_ENABLE_OPENGL "Enable OpenGL backend" OFF)  # 新增

# Platform Detection 调整
if(UNIX AND NOT APPLE)
    # Linux: 启用 Vulkan 和 OpenGL
    set(RVX_ENABLE_DX11 OFF)
    set(RVX_ENABLE_DX12 OFF)
    set(RVX_ENABLE_OPENGL ON)  # Linux 默认启用
endif()

# OpenGL 依赖
if(RVX_ENABLE_OPENGL)
    find_package(glad CONFIG REQUIRED)
endif()

# 添加 OpenGL 子目录
if(RVX_ENABLE_OPENGL)
    add_subdirectory(RHI_OpenGL)
endif()

# Status Summary
message(STATUS "  OpenGL:       ${RVX_ENABLE_OPENGL}")  # 新增
```

### 11.6 ShaderCompiler 扩展 (新增 GLSL 支持)

```cpp
// ShaderCompiler/Include/ShaderCompiler/ShaderCompiler.h
struct ShaderCompileResult
{
    // 现有字段...

    // OpenGL-specific (新增)
    std::string glslSource;
    uint32 glslVersion = 450;
};

// ShaderCompiler/Private/SPIRVCrossTranslator.h (新增方法)
struct SPIRVToGLSLOptions
{
    uint32 glslVersion = 450;
    bool es = false;
    bool vulkanSemantics = false;
};

struct SPIRVToGLSLResult
{
    bool success = false;
    std::string glslSource;
    std::string errorMessage;
    ShaderReflection reflection;
};

SPIRVToGLSLResult TranslateToGLSL(
    const std::vector<uint8_t>& spirvBytecode,
    RHIShaderStage stage,
    const char* entryPoint,
    const SPIRVToGLSLOptions& options = {});
```

---

## 12. 依赖项配置

### 12.1 vcpkg.json (更新版)

```json
{
  "name": "renderversex",
  "dependencies": [
    "spdlog",
    "glfw3",
    {
      "name": "vulkan",
      "platform": "linux | windows"
    },
    {
      "name": "vulkan-memory-allocator",
      "platform": "linux | windows"
    },
    {
      "name": "spirv-reflect",
      "platform": "linux | windows"
    },
    {
      "name": "directx-dxc",
      "platform": "windows"
    },
    
    // ======== OpenGL 相关依赖 (新增) ========
    {
      "name": "glad",
      "version>=": "0.1.36",
      "platform": "linux | windows"
    },
    {
      "name": "spirv-cross",
      "features": ["glsl"],
      "platform": "linux | windows"
    }
  ],
  "features": {
    "opengl": {
      "description": "OpenGL backend support",
      "dependencies": [
        "glad",
        {
          "name": "spirv-cross",
          "features": ["glsl"]
        }
      ]
    },
    "metal": {
      "description": "Metal backend support (macOS/iOS)",
      "dependencies": [
        {
          "name": "spirv-cross",
          "features": ["msl"]
        }
      ]
    }
  }
}
```

### 12.2 根 CMakeLists.txt 更新

```cmake
# OpenGL 依赖 (新增)
if(RVX_ENABLE_OPENGL)
    find_package(glad CONFIG REQUIRED)
    
    # SPIRV-Cross GLSL 支持
    find_package(spirv_cross_core CONFIG REQUIRED)
    find_package(spirv_cross_glsl CONFIG REQUIRED)
endif()

# ShaderCompiler 需要链接 SPIRV-Cross
if(RVX_ENABLE_METAL OR RVX_ENABLE_OPENGL)
    # 确保 SPIRV-Cross 依赖正确配置
    target_link_libraries(RVX_ShaderCompiler PRIVATE
        spirv-cross-core
        $<$<BOOL:${RVX_ENABLE_METAL}>:spirv-cross-msl>
        $<$<BOOL:${RVX_ENABLE_OPENGL}>:spirv-cross-glsl>
    )
endif()
```

### 12.3 glad 配置

#### 方案 A: 使用 vcpkg 默认包

vcpkg 的 glad 包通常包含 OpenGL 4.6 Core Profile。验证方法：

```bash
vcpkg install glad:x64-windows
# 或
vcpkg install glad:x64-linux
```

#### 方案 B: 自定义 glad 生成 (推荐)

使用 [glad2 在线生成器](https://gen.glad.sh/) 配置：

```
Language: C/C++
Specification: OpenGL
API: gl:core=4.5
Profile: Core
Extensions:
  - GL_ARB_gl_spirv               # SPIR-V 着色器 (4.6)
  - GL_ARB_bindless_texture       # 无绑定纹理
  - GL_ARB_shader_draw_parameters # gl_DrawID
  - GL_ARB_indirect_parameters    # 间接绘制参数
  - GL_ARB_buffer_storage         # 持久映射
  - GL_ARB_direct_state_access    # DSA (4.5 核心)
  - GL_ARB_texture_view           # 纹理视图
  - GL_ARB_separate_shader_objects # 分离着色器
  - GL_KHR_debug                  # 调试输出
  - GL_ARB_multi_bind             # 批量绑定
Options:
  - Generate a loader: Yes
  - Reproducible: Yes
```

生成后将 `glad.h` 和 `glad.c` 放入项目 `ThirdParty/glad/` 目录。

### 12.4 ShaderCompiler 依赖更新

```cmake
# ShaderCompiler/CMakeLists.txt 更新

# SPIRV-Cross 依赖
find_package(spirv_cross_core CONFIG REQUIRED)

if(RVX_ENABLE_METAL)
    find_package(spirv_cross_msl CONFIG REQUIRED)
endif()

if(RVX_ENABLE_OPENGL)
    find_package(spirv_cross_glsl CONFIG REQUIRED)
endif()

target_link_libraries(RVX_ShaderCompiler PRIVATE
    spirv-cross-core
    $<$<BOOL:${RVX_ENABLE_METAL}>:spirv-cross-msl>
    $<$<BOOL:${RVX_ENABLE_OPENGL}>:spirv-cross-glsl>
)
```

---

## 13. 实施路线图 (优化版)

### 实施优先级原则

| 优先级 | 说明 | 规则 |
|--------|------|------|
| 🔴 P0 | 阻塞性 | 必须首先完成，后续任务依赖此 |
| 🟡 P1 | 核心功能 | 基本渲染所需 |
| 🟢 P2 | 优化/扩展 | 可后期添加 |

---

### Phase 1: 基础设施与着色器 (P0)

> **目标**: 能够编译 HLSL → GLSL，OpenGL 上下文可用

| 任务 | 描述 | 优先级 | 依赖 | 状态 |
|------|------|--------|------|------|
| 1.1 修改 RHIDefinitions.h | 添加 `RHIBackendType::OpenGL` | 🔴 P0 | - | ✅ |
| 1.2 修改 RHICapabilities.h | 添加 `OpenGLSpecific` 结构 (完整版) | 🔴 P0 | - | ✅ |
| 1.3 修改根 CMakeLists.txt | 添加 `RVX_ENABLE_OPENGL` 选项 | 🔴 P0 | - | ✅ |
| 1.4 配置 vcpkg 依赖 | 添加 glad, spirv-cross-glsl | 🔴 P0 | - | ✅ |
| 1.5 创建 RHI_OpenGL 目录结构 | 按模块结构创建文件骨架 | 🔴 P0 | 1.3 | ✅ |
| 1.6 **SPIRVCrossTranslator GLSL** | 实现 `TranslateToGLSL()` + 绑定点重映射 | 🔴 P0 | 1.4 | ✅ |
| 1.7 扩展 ShaderCompileResult | 添加 glslSource, glslBindings | 🔴 P0 | 1.6 | ✅ |
| 1.8 OpenGLContext 初始化 | glad 加载, 版本检测, 调试回调 | 🔴 P0 | 1.5 | ✅ |
| 1.9 OpenGLDevice 骨架 | 构造/析构, 能力查询 | 🔴 P0 | 1.8 | ✅ |
| 1.10 修改 RHIModule.cpp | 添加 OpenGL 设备创建入口 | 🔴 P0 | 1.9 | ✅ |

**里程碑 1**: ✅ OpenGL 设备可创建，着色器可编译为 GLSL

---

### Phase 2: 核心资源与缓存 (P0-P1)

> **目标**: Buffer, Texture, Sampler 可用，FBO/VAO 缓存就绪

| 任务 | 描述 | 优先级 | 依赖 | 状态 |
|------|------|--------|------|------|
| 2.1 OpenGLConversions.h | RHI 枚举 → OpenGL 枚举映射 | 🔴 P0 | Phase 1 | ✅ |
| 2.2 OpenGLStateCache | 状态缓存系统 (Program, VAO, FBO, 深度等) | 🔴 P0 | Phase 1 | ✅ |
| 2.3 OpenGLDeletionQueue | 延迟删除队列 | 🔴 P0 | Phase 1 | ✅ |
| 2.4 OpenGLBuffer | CreateBuffer, Map/Unmap | 🔴 P0 | 2.1-2.3 | ✅ |
| 2.5 OpenGLTexture | CreateTexture (2D, 3D, Cube, Array) | 🔴 P0 | 2.1-2.3 | ✅ |
| 2.6 OpenGLTextureView | glTextureView 封装 | 🟡 P1 | 2.5 | ✅ |
| 2.7 OpenGLSampler | CreateSampler | 🔴 P0 | 2.1 | ✅ |
| 2.8 **OpenGLFramebufferCache** | FBO 缓存 + 失效机制 | 🔴 P0 | 2.5 | ✅ |
| 2.9 **OpenGLVAOCache** | VAO 缓存 (基于 Pipeline + VBO 组合) | 🔴 P0 | 2.4 | ✅ |
| 2.X OpenGLDebug | 调试基础设施 (资源追踪、验证、统计) | 🔴 P0 | Phase 1 | ✅ |

**里程碑 2**: ✅ 资源创建可用，缓存系统就绪

---

### Phase 3: 着色器与管线 (P0)

> **目标**: Pipeline 可创建，Descriptor Set 可绑定

| 任务 | 描述 | 优先级 | 依赖 | 状态 |
|------|------|--------|------|------|
| 3.1 OpenGLShader | Program 创建/链接/销毁 | 🔴 P0 | 1.6-1.7 | ✅ |
| 3.2 OpenGLPipeline (Graphics) | 图形管线状态封装 | 🔴 P0 | 3.1 | ✅ |
| 3.3 OpenGLPipeline (Compute) | 计算管线 | 🟡 P1 | 3.1 | ✅ |
| 3.4 OpenGLDescriptorSetLayout | 布局元数据 | 🔴 P0 | 1.6 | ✅ |
| 3.5 OpenGLDescriptorSet | 资源绑定表 | 🔴 P0 | 3.4 | ✅ |
| 3.6 OpenGLPipelineLayout | 管线布局 (描述符布局 + Push Constants) | 🔴 P0 | 3.4 | ✅ |
| 3.7 **OpenGLPushConstantBuffer** | Push Constants → UBO 模拟 | 🔴 P0 | 2.4 | ✅ |

**里程碑 3**: ✅ 完整 Pipeline 可创建

---

### Phase 4: 命令执行 (P0)

> **目标**: CommandContext 可录制和执行

| 任务 | 描述 | 优先级 | 依赖 | 状态 |
|------|------|--------|------|------|
| 4.1 GLCommandBuffer | 线性命令缓冲区 (Type-Punned) | 🔴 P0 | Phase 2-3 | ✅ |
| 4.2 GLCommandExecutor | 命令执行器 (switch-case) | 🔴 P0 | 4.1 | ✅ |
| 4.3 OpenGLCommandContext | RHI 接口实现 | 🔴 P0 | 4.2 | ✅ |
| 4.4 BeginRenderPass 实现 | FBO 绑定, Clear, Viewport | 🔴 P0 | 2.8, 4.3 | ✅ |
| 4.5 Draw/DrawIndexed 实现 | VAO 绑定, 绘制调用 | 🔴 P0 | 2.9, 4.3 | ✅ |
| 4.6 SetDescriptorSet 实现 | UBO/SSBO/Texture 绑定 | 🔴 P0 | 3.5, 4.3 | ✅ |
| 4.7 SetPushConstants 实现 | Push Constant UBO 更新 | 🔴 P0 | 3.7, 4.3 | ✅ |
| 4.8 Copy 命令实现 | Buffer/Texture 复制 | 🟡 P1 | 4.3 | ✅ |
| 4.9 Barrier 命令实现 | OpenGL 隐式同步 (主要用于验证) | 🟢 P2 | 4.3 | ✅ |

**里程碑 4**: ✅ 基本绘制可执行

---

### Phase 5: SwapChain 与同步 (P0)

> **目标**: 完整渲染循环可运行

| 任务 | 描述 | 优先级 | 依赖 | 状态 |
|------|------|--------|------|------|
| 5.1 OpenGLSwapChain | GLFW 集成, Present | 🔴 P0 | Phase 4 | ✅ |
| 5.2 OpenGLFence | GLsync 封装 | 🔴 P0 | Phase 1 | ✅ |
| 5.3 Device::BeginFrame | 帧开始处理, 删除队列 | 🔴 P0 | 2.3, 5.2 | ✅ |
| 5.4 Device::EndFrame | 帧结束, glFlush | 🔴 P0 | 5.3 | ✅ |
| 5.5 Device::SubmitCommandContext | 命令提交, Fence 信号 | 🔴 P0 | 4.3, 5.2 | ✅ |
| 5.6 Device::WaitIdle | glFinish | 🔴 P0 | Phase 1 | ✅ |

**里程碑 5**: ✅ 完整渲染循环运行

---

### Phase 6: 测试验证 (P1)

> **目标**: 验证所有示例正常运行

| 任务 | 描述 | 优先级 | 依赖 | 状态 |
|------|------|--------|------|------|
| 6.1 Triangle 示例 | 基础三角形渲染 | 🔴 P0 | Phase 5 | ⬜ |
| 6.2 TexturedQuad 示例 | 纹理采样验证 | 🟡 P1 | 6.1 | ⬜ |
| 6.3 Cube3D 示例 | 深度测试, MVP 变换 | 🟡 P1 | 6.1 | ⬜ |
| 6.4 ComputeDemo 示例 | 计算着色器验证 | 🟡 P1 | 3.3, 6.1 | ⬜ |
| 6.5 OpenGLValidation 测试 | 自动化测试框架集成 | 🟡 P1 | 6.1-6.4 | ⬜ |
| 6.6 跨后端对比测试 | 与 Vulkan/DX12 渲染结果对比 | 🟢 P2 | 6.5 | ⬜ |

**里程碑 6**: 所有示例通过

---

### Phase 7: 优化与扩展 (P2)

> **目标**: 性能优化，扩展功能

| 任务 | 描述 | 优先级 | 依赖 | 状态 |
|------|------|--------|------|------|
| 7.1 Multi-Bind 优化 | glBindTextures, glBindBuffersBase | 🟢 P2 | Phase 6 | ⬜ |
| 7.2 Persistent Mapping | 持久映射缓冲区 | 🟢 P2 | Phase 6 | ⬜ |
| 7.3 Multi-Draw Indirect | 批量绘制优化 | 🟢 P2 | Phase 6 | ⬜ |
| 7.4 GL_ARB_gl_spirv 支持 | OpenGL 4.6 原生 SPIR-V | 🟢 P2 | Phase 6 | ⬜ |
| 7.5 Bindless Texture | GL_ARB_bindless_texture | 🟢 P2 | Phase 6 | ⬜ |
| 7.6 Heap 模拟 | CreateHeap 空实现或模拟 | 🟢 P2 | Phase 6 | ⬜ |
| 7.7 Debug 标记完善 | RenderDoc 集成验证 | 🟢 P2 | Phase 6 | ⬜ |

---

### 任务依赖图

```
Phase 1 (基础设施 + 着色器)
    │
    ├───────────────────────────────────┐
    │                                   │
    ▼                                   ▼
Phase 2 (资源 + 缓存)            Phase 3 (管线)
    │                                   │
    └───────────────┬───────────────────┘
                    │
                    ▼
              Phase 4 (命令)
                    │
                    ▼
              Phase 5 (SwapChain)
                    │
                    ▼
              Phase 6 (测试)
                    │
                    ▼
              Phase 7 (优化)
```

---

### 预估工期

| 阶段 | 预估时间 | 累计时间 |
|------|----------|----------|
| Phase 1: 基础设施 | 3-4 天 | 3-4 天 |
| Phase 2: 资源与缓存 | 4-5 天 | 7-9 天 |
| Phase 3: 管线 | 3-4 天 | 10-13 天 |
| Phase 4: 命令 | 4-5 天 | 14-18 天 |
| Phase 5: SwapChain | 2-3 天 | 16-21 天 |
| Phase 6: 测试 | 3-5 天 | 19-26 天 |
| Phase 7: 优化 | 按需 | - |

**总预估**: 4-5 周 (不含优化阶段)

---

## 14. 平台支持矩阵

### 14.1 OpenGL 版本支持

| GPU 厂商 | 最低版本 | 推荐版本 | OpenGL 4.5 支持 |
|----------|---------|----------|----------------|
| NVIDIA (Kepler+) | 4.6 | 4.6 | ✅ |
| AMD (GCN+) | 4.6 | 4.6 | ✅ |
| Intel (Haswell+) | 4.5 | 4.6 | ✅ |
| Intel (Ivy Bridge) | 4.0 | 4.0 | ❌ |
| Mesa (Linux) | 4.6 | 4.6 | ✅ (大部分) |

### 14.2 平台适用性

| 平台 | OpenGL 版本 | 适用场景 | 备注 |
|------|------------|---------|------|
| **Linux** | 4.5-4.6 | ✅ 主要目标 | Mesa/NVIDIA/AMD 驱动 |
| **Windows** | 4.5-4.6 | ⚠️ 备选 | 老硬件或无 Vulkan 场景 |
| **macOS** | 4.1 | ❌ 不推荐 | 已废弃，使用 Metal |

### 14.3 功能支持矩阵

| 功能 | OpenGL 4.3 | OpenGL 4.5 | OpenGL 4.6 |
|------|-----------|-----------|-----------|
| 基础渲染 | ✅ | ✅ | ✅ |
| Compute Shader | ✅ | ✅ | ✅ |
| SSBO | ✅ | ✅ | ✅ |
| DSA | ❌ | ✅ | ✅ |
| Multi-Bind | ❌ | ✅ | ✅ |
| SPIR-V Shader | ❌ | ❌ | ✅ |
| Bindless Texture | 扩展 | 扩展 | 扩展 |
| Tessellation | ✅ | ✅ | ✅ |
| Geometry Shader | ✅ | ✅ | ✅ |
| Raytracing | ❌ | ❌ | ❌ |
| Mesh Shader | 扩展 | 扩展 | 扩展 |

---

## 15. 最佳实践与性能优化

### 15.1 状态管理

```cpp
// ❌ 不推荐：频繁切换状态
for (auto& object : objects) {
    glUseProgram(object.program);
    glBindVertexArray(object.vao);
    glBindTexture(GL_TEXTURE_2D, object.texture);
    glDrawElements(...);
}

// ✅ 推荐：按状态排序，使用状态缓存
std::sort(objects.begin(), objects.end(), [](a, b) {
    return std::tie(a.program, a.texture) < std::tie(b.program, b.texture);
});

for (auto& object : objects) {
    stateCache.BindProgram(object.program);
    stateCache.BindTexture(0, object.texture);
    stateCache.BindVertexArray(object.vao);
    glDrawElements(...);
}
```

### 15.2 缓冲区更新策略

```cpp
// ❌ 不推荐：每帧重新分配
glBufferData(GL_UNIFORM_BUFFER, size, newData, GL_DYNAMIC_DRAW);

// ✅ 推荐方案 A：Orphaning
glBufferData(GL_UNIFORM_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
glBufferSubData(GL_UNIFORM_BUFFER, 0, size, newData);

// ✅ 推荐方案 B：Persistent Mapping (4.4+)
glBufferStorage(GL_UNIFORM_BUFFER, size * 3, nullptr,
    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
void* ptr = glMapBufferRange(GL_UNIFORM_BUFFER, 0, size * 3,
    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
// 使用 fence 同步三缓冲
```

### 15.3 绘制调用优化

```cpp
// ❌ 不推荐：多次单独绘制
for (int i = 0; i < 1000; i++) {
    glDrawElements(GL_TRIANGLES, counts[i], GL_UNSIGNED_INT, offsets[i]);
}

// ✅ 推荐：Multi-Draw Indirect
struct DrawElementsIndirectCommand {
    GLuint count;
    GLuint instanceCount;
    GLuint firstIndex;
    GLuint baseVertex;
    GLuint baseInstance;
};
// 填充命令缓冲区...
glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 1000, 0);
```

### 15.4 纹理绑定优化

```cpp
// ❌ 不推荐：逐个绑定
for (int i = 0; i < 8; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, textures[i]);
}

// ✅ 推荐：Multi-Bind (4.4+)
glBindTextures(0, 8, textures);
glBindSamplers(0, 8, samplers);
```

### 15.5 错误处理

```cpp
// OpenGLCommon.h
#ifdef RVX_DEBUG
    #define GL_CHECK(call) do { \
        call; \
        GLenum err = glGetError(); \
        if (err != GL_NO_ERROR) { \
            RVX_RHI_ERROR("OpenGL error {} at {}:{}", err, __FILE__, __LINE__); \
        } \
    } while(0)
#else
    #define GL_CHECK(call) call
#endif

// 使用 GL_KHR_debug 进行调试
void GLAPIENTRY DebugCallback(GLenum source, GLenum type, GLuint id,
    GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
    if (severity == GL_DEBUG_SEVERITY_HIGH)
        RVX_RHI_ERROR("OpenGL: {}", message);
    else if (severity == GL_DEBUG_SEVERITY_MEDIUM)
        RVX_RHI_WARN("OpenGL: {}", message);
    else
        RVX_RHI_DEBUG("OpenGL: {}", message);
}

// 初始化时启用
glEnable(GL_DEBUG_OUTPUT);
glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
glDebugMessageCallback(DebugCallback, nullptr);
```

---

## 附录 A: 参考资源

- [OpenGL 4.5 Reference](https://www.khronos.org/registry/OpenGL-Refpages/gl4/)
- [OpenGL Wiki](https://www.khronos.org/opengl/wiki/)
- [SPIRV-Cross Documentation](https://github.com/KhronosGroup/SPIRV-Cross)
- [Glad Generator](https://glad.dav1d.de/)
- [OpenGL Best Practices for GPU Vendors](https://www.khronos.org/opengl/wiki/Common_Mistakes)
- [Approaching Zero Driver Overhead (AZDO)](https://www.gdcvault.com/play/1020791/Approaching-Zero-Driver-Overhead-in)

---

## 附录 B: 与其他后端的对比

| 特性 | DX11 | DX12 | Vulkan | Metal | OpenGL |
|------|------|------|--------|-------|--------|
| **API 风格** | 即时 | 显式 | 显式 | 显式 | 即时 |
| **命令缓冲** | Deferred Context | ✅ | ✅ | ✅ | 模拟 |
| **多线程录制** | 有限 | ✅ | ✅ | ✅ | ❌ |
| **显式同步** | ❌ | ✅ | ✅ | ✅ | ❌ |
| **显式内存** | ❌ | ✅ | ✅ | ✅ | ❌ |
| **Bindless** | 有限 | ✅ | ✅ | ✅ | 扩展 |
| **Raytracing** | ❌ | ✅ | ✅ | ✅ | ❌ |
| **实现复杂度** | 低 | 高 | 高 | 中 | 中 |
| **调试难度** | 低 | 高 | 高 | 中 | 低 |

---

*文档结束*
