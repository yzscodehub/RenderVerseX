# RHI OpenGL 后端实现规划

> **文档版本**: 1.0  
> **创建日期**: 2026-01-20  
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
8. [现有代码修改清单](#8-现有代码修改清单)
9. [依赖项配置](#9-依赖项配置)
10. [实施路线图](#10-实施路线图)
11. [平台支持矩阵](#11-平台支持矩阵)
12. [最佳实践与性能优化](#12-最佳实践与性能优化)

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

**命令结构设计**:

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
    SetViewport,
    SetScissor,
    Draw,
    DrawIndexed,
    Dispatch,
    CopyBuffer,
    // ...
};

// 命令基类
struct GLCommand
{
    GLCommandType type;
    virtual void Execute(GLStateCache& cache) = 0;
};

// 示例：绘制命令
struct GLDrawCommand : GLCommand
{
    uint32 vertexCount;
    uint32 instanceCount;
    uint32 firstVertex;
    uint32 firstInstance;
    
    void Execute(GLStateCache& cache) override
    {
        glDrawArraysInstancedBaseInstance(
            cache.primitiveTopology,
            firstVertex,
            vertexCount,
            instanceCount,
            firstInstance
        );
    }
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

### 5.2 SPIRV-Cross GLSL 选项

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
        
        spirv_cross::CompilerGLSL::Options glslOptions;
        glslOptions.version = options.glslVersion;  // 450
        glslOptions.es = false;  // Desktop GLSL
        glslOptions.vulkan_semantics = false;
        glslOptions.enable_420pack_extension = true;
        glslOptions.emit_push_constant_as_uniform_buffer = true;
        glslOptions.emit_uniform_buffer_as_plain_uniforms = false;
        
        glslCompiler.set_common_options(glslOptions);
        
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
```

### 5.3 ShaderCompileResult 扩展

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
};
```

### 5.4 着色器变体处理

```cpp
// 针对 OpenGL 的特殊处理
std::string PreprocessGLSL(const std::string& glsl, const OpenGLCapabilities& caps)
{
    std::string processed = glsl;
    
    // 处理绑定点差异
    // HLSL: register(b0, space0) → GLSL: layout(binding = 0, std140)
    
    // 处理采样器分离
    // HLSL: Texture2D + SamplerState → GLSL: sampler2D (combined)
    // 或使用 GL_ARB_separate_shader_objects
    
    // 处理 push constant
    // HLSL: cbuffer → GLSL: uniform block 或 uniform
    
    return processed;
}
```

---

## 6. 模块结构设计

### 6.1 目录结构

```
RHI_OpenGL/
├── CMakeLists.txt
├── Include/
│   └── OpenGL/
│       └── OpenGLDevice.h              # 公开设备接口
└── Private/
    ├── OpenGLCommon.h                  # 通用定义、错误检查宏
    ├── OpenGLConversions.h             # RHI → OpenGL 类型转换
    ├── OpenGLStateCache.h              # 状态缓存系统
    ├── OpenGLStateCache.cpp
    ├── OpenGLDevice.h                  # 设备类定义
    ├── OpenGLDevice.cpp                # 设备实现
    ├── OpenGLResources.h               # Buffer, Texture, Sampler, TextureView
    ├── OpenGLResources.cpp
    ├── OpenGLShader.h                  # Shader, Program
    ├── OpenGLShader.cpp
    ├── OpenGLPipeline.h                # Pipeline State
    ├── OpenGLPipeline.cpp
    ├── OpenGLDescriptor.h              # DescriptorSetLayout, DescriptorSet
    ├── OpenGLDescriptor.cpp
    ├── OpenGLCommandContext.h          # Command List + Execution
    ├── OpenGLCommandContext.cpp
    ├── OpenGLSwapChain.h               # SwapChain (GLFW integration)
    ├── OpenGLSwapChain.cpp
    └── OpenGLSync.h/.cpp               # Fence (GLsync)
```

### 6.2 CMakeLists.txt

```cmake
# =============================================================================
# RHI_OpenGL Module - OpenGL backend implementation
# =============================================================================
add_library(RVX_RHI_OpenGL STATIC)

target_sources(RVX_RHI_OpenGL PRIVATE
    Private/OpenGLStateCache.cpp
    Private/OpenGLDevice.cpp
    Private/OpenGLResources.cpp
    Private/OpenGLShader.cpp
    Private/OpenGLPipeline.cpp
    Private/OpenGLDescriptor.cpp
    Private/OpenGLCommandContext.cpp
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
    GLAD_GLAPI_EXPORT=1
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

### 7.4 OpenGLCommandContext

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
    void Flush();

private:
    OpenGLDevice* m_device = nullptr;
    RHICommandQueueType m_queueType;
    
    // Command storage
    std::vector<std::unique_ptr<GLCommand>> m_commands;
    
    // Current state tracking
    OpenGLPipeline* m_currentPipeline = nullptr;
    GLuint m_currentVAO = 0;
    GLuint m_currentFBO = 0;
    
    // Render pass state
    bool m_inRenderPass = false;
    GLuint m_renderPassFBO = 0;
    
    // VAO management
    std::unordered_map<size_t, GLuint> m_vaoCache;
    GLuint GetOrCreateVAO(const VertexBufferBindings& bindings);
};
```

---

## 8. 现有代码修改清单

### 8.1 RHI/Include/RHI/RHIDefinitions.h

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

### 8.2 RHI/Include/RHI/RHICapabilities.h

```cpp
struct RHICapabilities
{
    // 现有字段...

    // OpenGL-specific (新增)
    struct OpenGLSpecific
    {
        uint32 majorVersion = 0;       // e.g., 4
        uint32 minorVersion = 0;       // e.g., 5
        bool coreProfile = true;
        bool hasDSA = false;           // Direct State Access (4.5+)
        bool hasARBSpirv = false;      // GL_ARB_gl_spirv (4.6+)
        bool hasBindlessTexture = false;
        bool hasComputeShader = false; // 4.3+
        bool hasSSBO = false;          // 4.3+
        bool hasMultiBind = false;     // 4.4+
        std::string renderer;          // GPU 名称
        std::string glslVersion;       // GLSL 版本字符串
    } opengl;
};
```

### 8.3 RHI/Private/RHIModule.cpp

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

### 8.4 RHI/CMakeLists.txt

```cmake
# 现有内容...

if(RVX_ENABLE_OPENGL)
    target_compile_definitions(RVX_RHI PUBLIC RVX_ENABLE_OPENGL=1)
endif()
```

### 8.5 CMakeLists.txt (根目录)

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

### 8.6 ShaderCompiler 扩展

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

## 9. 依赖项配置

### 9.1 vcpkg.json

```json
{
  "name": "renderversex",
  "dependencies": [
    // 现有依赖...
    
    {
      "name": "glad",
      "version>=": "0.1.36",
      "platform": "linux | windows"
    }
  ],
  "overrides": [],
  "features": {
    "opengl": {
      "description": "OpenGL backend support",
      "dependencies": [
        "glad"
      ]
    }
  }
}
```

### 9.2 glad 配置

推荐使用 glad2 生成器，配置选项：

```
API: gl:core=4.5
Extensions:
  - GL_ARB_gl_spirv
  - GL_ARB_bindless_texture
  - GL_ARB_shader_draw_parameters
  - GL_ARB_indirect_parameters
  - GL_KHR_debug
```

或使用 vcpkg 默认的 glad 包。

---

## 10. 实施路线图

### Phase 1: 基础设施 (1 周)

| 任务 | 描述 | 状态 |
|------|------|------|
| 修改 RHIDefinitions.h | 添加 OpenGL 后端类型 | ⬜ |
| 修改 RHICapabilities.h | 添加 OpenGLSpecific 结构 | ⬜ |
| 修改 CMakeLists.txt | 添加 RVX_ENABLE_OPENGL 选项 | ⬜ |
| 创建 RHI_OpenGL 目录 | 按照模块结构创建文件 | ⬜ |
| 配置 vcpkg 依赖 | 添加 glad | ⬜ |
| 实现 OpenGLDevice 骨架 | 基本初始化和能力查询 | ⬜ |

### Phase 2: 核心资源 (1-2 周)

| 任务 | 描述 | 状态 |
|------|------|------|
| OpenGLBuffer | 缓冲区创建、映射 | ⬜ |
| OpenGLTexture | 纹理创建、视图 | ⬜ |
| OpenGLSampler | 采样器状态 | ⬜ |
| OpenGLStateCache | 状态缓存系统 | ⬜ |
| OpenGLConversions | 格式/枚举转换 | ⬜ |

### Phase 3: 着色器与管线 (1-2 周)

| 任务 | 描述 | 状态 |
|------|------|------|
| SPIRVCrossTranslator 扩展 | 添加 GLSL 输出 | ⬜ |
| OpenGLShader | Program 创建和链接 | ⬜ |
| OpenGLPipeline | 管线状态封装 | ⬜ |
| OpenGLDescriptor | 描述符集绑定 | ⬜ |

### Phase 4: 命令执行 (1-2 周)

| 任务 | 描述 | 状态 |
|------|------|------|
| OpenGLCommandContext | 命令记录和执行 | ⬜ |
| VAO 管理 | 顶点数组对象缓存 | ⬜ |
| FBO 管理 | 帧缓冲对象处理 | ⬜ |
| OpenGLSwapChain | 与 GLFW 集成 | ⬜ |
| OpenGLSync | Fence 同步 | ⬜ |

### Phase 5: 测试验证 (1 周)

| 任务 | 描述 | 状态 |
|------|------|------|
| Triangle 示例 | 基本渲染验证 | ⬜ |
| TexturedQuad 示例 | 纹理采样验证 | ⬜ |
| Cube3D 示例 | 深度测试验证 | ⬜ |
| ComputeDemo 示例 | 计算着色器验证 | ⬜ |
| 创建 OpenGLValidation 测试 | 自动化测试 | ⬜ |

### 估计总工期: 5-8 周

---

## 11. 平台支持矩阵

### 11.1 OpenGL 版本支持

| GPU 厂商 | 最低版本 | 推荐版本 | OpenGL 4.5 支持 |
|----------|---------|----------|----------------|
| NVIDIA (Kepler+) | 4.6 | 4.6 | ✅ |
| AMD (GCN+) | 4.6 | 4.6 | ✅ |
| Intel (Haswell+) | 4.5 | 4.6 | ✅ |
| Intel (Ivy Bridge) | 4.0 | 4.0 | ❌ |
| Mesa (Linux) | 4.6 | 4.6 | ✅ (大部分) |

### 11.2 平台适用性

| 平台 | OpenGL 版本 | 适用场景 | 备注 |
|------|------------|---------|------|
| **Linux** | 4.5-4.6 | ✅ 主要目标 | Mesa/NVIDIA/AMD 驱动 |
| **Windows** | 4.5-4.6 | ⚠️ 备选 | 老硬件或无 Vulkan 场景 |
| **macOS** | 4.1 | ❌ 不推荐 | 已废弃，使用 Metal |

### 11.3 功能支持矩阵

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

## 12. 最佳实践与性能优化

### 12.1 状态管理

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

### 12.2 缓冲区更新策略

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

### 12.3 绘制调用优化

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

### 12.4 纹理绑定优化

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

### 12.5 错误处理

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
