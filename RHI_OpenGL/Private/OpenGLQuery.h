#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "RHI/RHIQuery.h"
#include <vector>

namespace RVX
{
    class OpenGLDevice;

    // =============================================================================
    // OpenGL Query Pool
    // Wraps OpenGL query objects for timestamp and occlusion queries
    // =============================================================================
    class OpenGLQueryPool : public RHIQueryPool
    {
    public:
        OpenGLQueryPool(OpenGLDevice* device, const RHIQueryPoolDesc& desc);
        ~OpenGLQueryPool() override;

        // RHIQueryPool interface
        RHIQueryType GetType() const override { return m_type; }
        uint32 GetCount() const override { return m_count; }
        uint64 GetTimestampFrequency() const override { return m_timestampFrequency; }

        // OpenGL specific
        GLuint GetQuery(uint32 index) const;
        GLenum GetGLQueryTarget() const { return m_glTarget; }

        // Check if query result is available (non-blocking)
        bool IsResultAvailable(uint32 index) const;

        // Get query result (blocking)
        uint64 GetResult(uint32 index) const;

        // Get query result if available (non-blocking)
        bool TryGetResult(uint32 index, uint64& outResult) const;

    private:
        OpenGLDevice* m_device = nullptr;
        RHIQueryType m_type = RHIQueryType::Timestamp;
        uint32 m_count = 0;
        uint64 m_timestampFrequency = 0;
        GLenum m_glTarget = GL_TIMESTAMP;
        std::vector<GLuint> m_queries;
    };

} // namespace RVX
