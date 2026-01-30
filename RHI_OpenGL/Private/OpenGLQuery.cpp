#include "OpenGLQuery.h"
#include "OpenGLDevice.h"
#include "Core/Log.h"

namespace RVX
{
    OpenGLQueryPool::OpenGLQueryPool(OpenGLDevice* device, const RHIQueryPoolDesc& desc)
        : m_device(device)
        , m_type(desc.type)
        , m_count(desc.count)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Determine GL query target based on query type
        switch (m_type)
        {
            case RHIQueryType::Timestamp:
                m_glTarget = GL_TIMESTAMP;
                break;
            case RHIQueryType::Occlusion:
                m_glTarget = GL_SAMPLES_PASSED;
                break;
            case RHIQueryType::BinaryOcclusion:
                m_glTarget = GL_ANY_SAMPLES_PASSED;
                break;
            case RHIQueryType::PipelineStatistics:
                // OpenGL doesn't have a direct equivalent
                // We could use GL_PRIMITIVES_GENERATED for partial stats
                m_glTarget = GL_PRIMITIVES_GENERATED;
                RVX_RHI_WARN("OpenGL: PipelineStatistics queries only partially supported");
                break;
            default:
                RVX_RHI_ERROR("OpenGL: Unsupported query type");
                return;
        }

        // Get timestamp frequency (in Hz, typically 1 GHz on modern GPUs)
        if (m_type == RHIQueryType::Timestamp)
        {
            // Query the GPU timestamp counter frequency
            GLint64 frequency = 0;
            
            // OpenGL doesn't have a direct way to query timestamp frequency
            // The standard says timestamps are in nanoseconds on most implementations
            // We can verify by checking if GL_TIMESTAMP_PERIOD_NV is available (NVIDIA extension)
            // For portability, assume 1 GHz (1 nanosecond resolution)
            m_timestampFrequency = 1000000000;  // 1 GHz = 1ns resolution
            
            RVX_RHI_DEBUG("OpenGL: Using timestamp frequency {} Hz (assumed 1ns resolution)", 
                         m_timestampFrequency);
        }

        // Create query objects
        m_queries.resize(m_count);
        GL_CHECK(glGenQueries(static_cast<GLsizei>(m_count), m_queries.data()));

        // Set debug labels if available
        if (desc.debugName && device->GetExtensions().GL_KHR_debug)
        {
            for (uint32 i = 0; i < m_count; ++i)
            {
                std::string label = std::string(desc.debugName) + "[" + std::to_string(i) + "]";
                GL_CHECK(glObjectLabel(GL_QUERY, m_queries[i], 
                                       static_cast<GLsizei>(label.length()), label.c_str()));
            }
        }

        RVX_RHI_DEBUG("OpenGL: Created query pool '{}' with {} queries of type {}", 
                     desc.debugName ? desc.debugName : "", m_count, static_cast<int>(m_type));
    }

    OpenGLQueryPool::~OpenGLQueryPool()
    {
        if (!m_queries.empty())
        {
            // Queue for deletion on the GL thread
            if (m_device)
            {
                m_device->GetDeletionQueue().DeleteQueries(m_queries);
            }
            else
            {
                // Direct deletion if device is gone
                glDeleteQueries(static_cast<GLsizei>(m_queries.size()), m_queries.data());
            }
            m_queries.clear();
        }
    }

    GLuint OpenGLQueryPool::GetQuery(uint32 index) const
    {
        if (index < m_queries.size())
        {
            return m_queries[index];
        }
        return 0;
    }

    bool OpenGLQueryPool::IsResultAvailable(uint32 index) const
    {
        if (index >= m_queries.size())
        {
            return false;
        }

        GLuint available = GL_FALSE;
        GL_CHECK(glGetQueryObjectuiv(m_queries[index], GL_QUERY_RESULT_AVAILABLE, &available));
        return available == GL_TRUE;
    }

    uint64 OpenGLQueryPool::GetResult(uint32 index) const
    {
        if (index >= m_queries.size())
        {
            return 0;
        }

        GLuint64 result = 0;
        // This call blocks until the result is available
        GL_CHECK(glGetQueryObjectui64v(m_queries[index], GL_QUERY_RESULT, &result));
        return static_cast<uint64>(result);
    }

    bool OpenGLQueryPool::TryGetResult(uint32 index, uint64& outResult) const
    {
        if (index >= m_queries.size())
        {
            return false;
        }

        // Check if result is available first
        GLuint available = GL_FALSE;
        GL_CHECK(glGetQueryObjectuiv(m_queries[index], GL_QUERY_RESULT_AVAILABLE, &available));

        if (available == GL_TRUE)
        {
            GLuint64 result = 0;
            GL_CHECK(glGetQueryObjectui64v(m_queries[index], GL_QUERY_RESULT, &result));
            outResult = static_cast<uint64>(result);
            return true;
        }

        return false;
    }

} // namespace RVX
