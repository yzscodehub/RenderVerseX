#include "OpenGLQuery.h"
#include "OpenGLDevice.h"
#include "Core/Log.h"

namespace RVX
{
    OpenGLQueryPool::OpenGLQueryPool(OpenGLDevice* device, const RHIQueryPoolDesc& desc)
        : RHIQueryPool(desc.queueType, 0)
        , m_device(device)
        , m_type(desc.type)
        , m_count(desc.count)
    {
        auto validation = ValidateRHIQueryPoolDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("OpenGL: Invalid query pool description: {}", validation.message);
            m_count = 0;
            return;
        }

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

        // The device rejects timestamp pools until it can publish both a
        // verified Graphics frequency and timestamp valid-bit width.  Preserve
        // the constructor's fail-closed behavior for direct backend callers.
        if (m_type == RHIQueryType::Timestamp)
        {
            RVX_RHI_ERROR("OpenGL: Timestamp query pool creation requires verified timestamp metadata");
            m_count = 0;
            return;
        }

        // Create query objects
        m_queries.resize(m_count);
        m_debugLabelsApplied.resize(m_count, 0);
        GL_CHECK(glGenQueries(static_cast<GLsizei>(m_count), m_queries.data()));

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

    void OpenGLQueryPool::ApplyDebugLabel(uint32 index)
    {
        if (index >= m_queries.size() || index >= m_debugLabelsApplied.size())
        {
            return;
        }

        if (m_debugLabelsApplied[index] || !m_device || GetDebugName().empty() ||
            !m_device->GetExtensions().GL_KHR_debug)
        {
            return;
        }

        const GLuint query = m_queries[index];
        if (query == 0 || glIsQuery(query) != GL_TRUE)
        {
            return;
        }

        const std::string label = GetDebugName() + "[" + std::to_string(index) + "]";
        GL_CHECK(glObjectLabel(GL_QUERY, query,
                               static_cast<GLsizei>(label.length()), label.c_str()));
        m_debugLabelsApplied[index] = 1;
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
