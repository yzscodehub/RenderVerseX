#pragma once

#include "OpenGLDevice.h"
#include "RHI/RHIUpload.h"
#include <atomic>
#include <mutex>
#include <array>

namespace RVX
{
    // =============================================================================
    // OpenGL Staging Buffer
    // Uses PBO (Pixel Buffer Object) for efficient CPU->GPU transfers
    // =============================================================================
    class OpenGLStagingBuffer : public RHIStagingBuffer
    {
    public:
        OpenGLStagingBuffer(OpenGLDevice* device, const RHIStagingBufferDesc& desc);
        ~OpenGLStagingBuffer() override;

        // RHIStagingBuffer interface
        void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override;
        void Unmap() override;
        uint64 GetSize() const override { return m_size; }
        RHIBuffer* GetBuffer() const override;

        // OpenGL specific
        GLuint GetPBO() const { return m_pbo; }

    private:
        OpenGLDevice* m_device = nullptr;
        GLuint m_pbo = 0;
        uint64 m_size = 0;
        void* m_mappedData = nullptr;
        bool m_isMapped = false;
        mutable RHIBufferRef m_wrapperBuffer;
    };

    // =============================================================================
    // OpenGL Ring Buffer
    // Uses double-buffered PBOs
    // =============================================================================
    class OpenGLRingBuffer : public RHIRingBuffer
    {
    public:
        OpenGLRingBuffer(OpenGLDevice* device, const RHIRingBufferDesc& desc);
        ~OpenGLRingBuffer() override;

        // RHIRingBuffer interface
        RHIRingAllocation Allocate(uint64 size) override;
        void Reset(uint32 frameIndex) override;
        RHIBuffer* GetBuffer() const override;
        uint64 GetSize() const override { return m_totalSize; }
        uint32 GetAlignment() const override { return m_alignment; }

        // OpenGL specific
        GLuint GetPBO() const { return m_pbos[m_currentBuffer]; }

    private:
        OpenGLDevice* m_device = nullptr;
        std::array<GLuint, 2> m_pbos = {};

        uint64 m_totalSize = 0;
        uint32 m_alignment = 256;
        uint32 m_currentBuffer = 0;

        uint64 m_currentOffset = 0;
        void* m_mappedData = nullptr;

        std::mutex m_mutex;
        mutable RHIBufferRef m_wrapperBuffer;
    };

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIStagingBufferRef CreateOpenGLStagingBuffer(OpenGLDevice* device, const RHIStagingBufferDesc& desc);
    RHIRingBufferRef CreateOpenGLRingBuffer(OpenGLDevice* device, const RHIRingBufferDesc& desc);

} // namespace RVX
