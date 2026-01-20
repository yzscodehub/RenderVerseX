#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "OpenGLShader.h"
#include "OpenGLConversions.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIDescriptor.h"
#include <memory>

namespace RVX
{
    class OpenGLDevice;
    class OpenGLDescriptorSetLayout;

    // =============================================================================
    // Push Constant Buffer (UBO simulation)
    // =============================================================================
    class OpenGLPushConstantBuffer
    {
    public:
        static constexpr uint32 PUSH_CONSTANT_BINDING = 0;
        static constexpr size_t MAX_PUSH_CONSTANT_SIZE = 256;

        OpenGLPushConstantBuffer();
        ~OpenGLPushConstantBuffer();

        void Update(const void* data, uint32 size, uint32 offset);
        void Bind();
        GLuint GetHandle() const { return m_buffer; }

    private:
        GLuint m_buffer = 0;
    };

    // =============================================================================
    // OpenGL Descriptor Set Layout
    // =============================================================================
    class OpenGLDescriptorSetLayout : public RHIDescriptorSetLayout
    {
    public:
        OpenGLDescriptorSetLayout(OpenGLDevice* device, const RHIDescriptorSetLayoutDesc& desc);
        ~OpenGLDescriptorSetLayout() override = default;

        const RHIDescriptorSetLayoutDesc& GetDesc() const { return m_desc; }
        const std::vector<RHIBindingLayoutEntry>& GetEntries() const { return m_desc.entries; }

        // Get the OpenGL binding for a given RHI binding
        uint32 GetGLBinding(uint32 rhiBinding, RHIBindingType type) const;

    private:
        OpenGLDevice* m_device = nullptr;
        RHIDescriptorSetLayoutDesc m_desc;

        // Mapping from RHI binding to OpenGL binding point (by type)
        struct BindingInfo
        {
            uint32 glBinding = 0;
            RHIBindingType type = RHIBindingType::UniformBuffer;
        };
        std::unordered_map<uint32, BindingInfo> m_bindingMap;
    };

    // =============================================================================
    // OpenGL Pipeline Layout
    // =============================================================================
    class OpenGLPipelineLayout : public RHIPipelineLayout
    {
    public:
        OpenGLPipelineLayout(OpenGLDevice* device, const RHIPipelineLayoutDesc& desc);
        ~OpenGLPipelineLayout() override = default;

        const RHIPipelineLayoutDesc& GetDesc() const { return m_desc; }
        const std::vector<OpenGLDescriptorSetLayout*>& GetSetLayouts() const { return m_setLayouts; }
        uint32 GetPushConstantSize() const { return m_desc.pushConstantSize; }

    private:
        OpenGLDevice* m_device = nullptr;
        RHIPipelineLayoutDesc m_desc;
        std::vector<OpenGLDescriptorSetLayout*> m_setLayouts;
    };

    // =============================================================================
    // OpenGL Graphics Pipeline
    // =============================================================================
    class OpenGLGraphicsPipeline : public RHIPipeline
    {
    public:
        OpenGLGraphicsPipeline(OpenGLDevice* device, const RHIGraphicsPipelineDesc& desc);
        ~OpenGLGraphicsPipeline() override;

        bool IsCompute() const override { return false; }

        // Get the linked program
        OpenGLProgram* GetProgram() const { return m_program.get(); }
        GLuint GetProgramHandle() const { return m_program ? m_program->GetHandle() : 0; }

        // Get pipeline state
        const RHIRasterizerState& GetRasterizerState() const { return m_rasterizerState; }
        const RHIDepthStencilState& GetDepthStencilState() const { return m_depthStencilState; }
        const RHIBlendState& GetBlendState() const { return m_blendState; }
        GLenum GetPrimitiveMode() const { return m_primitiveMode; }
        
        // Input layout
        const RHIInputLayoutDesc& GetInputLayout() const { return m_inputLayout; }
        uint64 GetInputLayoutHash() const { return m_inputLayoutHash; }

        // Pipeline layout
        OpenGLPipelineLayout* GetPipelineLayout() const { return m_pipelineLayout; }

        bool IsValid() const { return m_program && m_program->IsLinked(); }

    private:
        void ComputeInputLayoutHash();

        OpenGLDevice* m_device = nullptr;
        std::unique_ptr<OpenGLProgram> m_program;
        
        // Cached state
        RHIRasterizerState m_rasterizerState;
        RHIDepthStencilState m_depthStencilState;
        RHIBlendState m_blendState;
        RHIInputLayoutDesc m_inputLayout;
        GLenum m_primitiveMode = GL_TRIANGLES;
        uint64 m_inputLayoutHash = 0;

        OpenGLPipelineLayout* m_pipelineLayout = nullptr;
    };

    // =============================================================================
    // OpenGL Compute Pipeline
    // =============================================================================
    class OpenGLComputePipeline : public RHIPipeline
    {
    public:
        OpenGLComputePipeline(OpenGLDevice* device, const RHIComputePipelineDesc& desc);
        ~OpenGLComputePipeline() override;

        bool IsCompute() const override { return true; }

        OpenGLProgram* GetProgram() const { return m_program.get(); }
        GLuint GetProgramHandle() const { return m_program ? m_program->GetHandle() : 0; }
        OpenGLPipelineLayout* GetPipelineLayout() const { return m_pipelineLayout; }

        bool IsValid() const { return m_program && m_program->IsLinked(); }

    private:
        OpenGLDevice* m_device = nullptr;
        std::unique_ptr<OpenGLProgram> m_program;
        OpenGLPipelineLayout* m_pipelineLayout = nullptr;
    };

} // namespace RVX
