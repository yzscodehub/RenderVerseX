#include "OpenGLUpload.h"
#include "OpenGLResources.h"
#include "Core/Logger.h"

namespace RVX
{
    // =============================================================================
    // OpenGL Staging Buffer
    // =============================================================================
    OpenGLStagingBuffer::OpenGLStagingBuffer(OpenGLDevice* device, const RHIStagingBufferDesc& desc)
        : m_device(device), m_size(desc.size)
    {
        glGenBuffers(1, &m_pbo);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, m_size, nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        RVX_RHI_INFO("OpenGLStagingBuffer: Created PBO {} ({} bytes)", m_pbo, m_size);
    }

    OpenGLStagingBuffer::~OpenGLStagingBuffer()
    {
        if (m_pbo != 0)
        {
            glDeleteBuffers(1, &m_pbo);
            m_pbo = 0;
        }
    }

    void* OpenGLStagingBuffer::Map(uint64 offset, uint64 size)
    {
        if (m_isMapped)
        {
            return m_mappedData;
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo);

        GLbitfield access = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT;
        void* data = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, static_cast<GLintptr>(offset),
                                       static_cast<GLsizeiptr>(size == RVX_WHOLE_SIZE ? m_size - offset : size),
                                       access);

        if (data == nullptr)
        {
            RVX_RHI_ERROR("OpenGLStagingBuffer: Failed to map buffer");
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            return nullptr;
        }

        m_mappedData = data;
        m_isMapped = true;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        return m_mappedData;
    }

    void OpenGLStagingBuffer::Unmap()
    {
        if (!m_isMapped)
        {
            return;
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        m_mappedData = nullptr;
        m_isMapped = false;
    }

    RHIBuffer* OpenGLStagingBuffer::GetBuffer() const
    {
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc desc = {};
            desc.size = m_size;
            desc.usage = RHIBufferUsage::CopySrc;
            desc.memoryType = RHIMemoryType::Upload;
            m_wrapperBuffer = CreateOpenGLBuffer(m_device, desc);
        }
        return m_wrapperBuffer.get();
    }

    // =============================================================================
    // OpenGL Ring Buffer
    // =============================================================================
    OpenGLRingBuffer::OpenGLRingBuffer(OpenGLDevice* device, const RHIRingBufferDesc& desc)
        : m_device(device), m_totalSize(desc.size), m_alignment(desc.alignment)
    {
        // Create double PBOs
        glGenBuffers(2, m_pbos.data());

        for (uint32 i = 0; i < 2; ++i)
        {
            glBindBuffer(GL_ARRAY_BUFFER, m_pbos[i]);
            glBufferData(GL_ARRAY_BUFFER, m_totalSize, nullptr, GL_DYNAMIC_DRAW);
            RVX_RHI_INFO("OpenGLRingBuffer: Created PBO {} ({} bytes)", m_pbos[i], m_totalSize);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Initial mapping
        glBindBuffer(GL_ARRAY_BUFFER, m_pbos[0]);
        m_mappedData = glMapBufferRange(GL_ARRAY_BUFFER, 0, m_totalSize, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (m_mappedData)
        {
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    OpenGLRingBuffer::~OpenGLRingBuffer()
    {
        if (m_pbos[0] != 0)
        {
            glDeleteBuffers(2, m_pbos.data());
            m_pbos[0] = 0;
            m_pbos[1] = 0;
        }
    }

    RHIRingAllocation OpenGLRingBuffer::Allocate(uint64 size)
    {
        uint64 alignedSize = (size + m_alignment - 1) & ~(m_alignment - 1);

        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_currentOffset + alignedSize > m_totalSize)
        {
            // Switch buffer
            m_currentBuffer = (m_currentBuffer + 1) % 2;
            m_currentOffset = 0;

            // Unmap old, map new
            glBindBuffer(GL_ARRAY_BUFFER, m_pbos[m_currentBuffer]);
            if (m_mappedData)
            {
                glUnmapBuffer(GL_ARRAY_BUFFER);
            }
            m_mappedData = glMapBufferRange(GL_ARRAY_BUFFER, 0, m_totalSize, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (m_mappedData)
            {
                glUnmapBuffer(GL_ARRAY_BUFFER);
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        RHIRingAllocation alloc;
        alloc.cpuAddress = static_cast<uint8*>(m_mappedData) + m_currentOffset;
        alloc.gpuOffset = m_currentOffset;
        alloc.size = alignedSize;
        alloc.buffer = m_wrapperBuffer.get();

        m_currentOffset += alignedSize;
        return alloc;
    }

    void OpenGLRingBuffer::Reset(uint32 frameIndex)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        uint32 newBuffer = (frameIndex % 2);
        if (newBuffer != m_currentBuffer)
        {
            m_currentBuffer = newBuffer;
            m_currentOffset = 0;

            glBindBuffer(GL_ARRAY_BUFFER, m_pbos[m_currentBuffer]);
            if (m_mappedData)
            {
                glUnmapBuffer(GL_ARRAY_BUFFER);
            }
            m_mappedData = glMapBufferRange(GL_ARRAY_BUFFER, 0, m_totalSize, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (m_mappedData)
            {
                glUnmapBuffer(GL_ARRAY_BUFFER);
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
        else
        {
            m_currentOffset = 0;
        }
    }

    RHIBuffer* OpenGLRingBuffer::GetBuffer() const
    {
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc desc = {};
            desc.size = m_totalSize;
            desc.usage = RHIBufferUsage::Vertex | RHIBufferUsage::Uniform;
            desc.memoryType = RHIMemoryType::Dynamic;
            m_wrapperBuffer = CreateOpenGLBuffer(m_device, desc);
        }
        return m_wrapperBuffer.get();
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIStagingBufferRef CreateOpenGLStagingBuffer(OpenGLDevice* device, const RHIStagingBufferDesc& desc)
    {
        return new OpenGLStagingBuffer(device, desc);
    }

    RHIRingBufferRef CreateOpenGLRingBuffer(OpenGLDevice* device, const RHIRingBufferDesc& desc)
    {
        return new OpenGLRingBuffer(device, desc);
    }

} // namespace RVX
