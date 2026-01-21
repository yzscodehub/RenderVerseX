#include "Core/RefCounted.h"
#include "RHI/RHI.h"
#include <vector>
#include <array>

namespace RVX
{
    // =============================================================================
    // Frame Resource Manager
    // Implements IDeferredDeleter for deferred resource deletion
    // =============================================================================
    class FrameResourceManager : public IDeferredDeleter
    {
    public:
        static FrameResourceManager& Get()
        {
            static FrameResourceManager instance;
            return instance;
        }

        void Initialize()
        {
            DeferredDeleterRegistry::Register(this);
        }

        void Shutdown()
        {
            DeferredDeleterRegistry::Unregister();
            
            // Delete all pending resources
            for (auto& frame : m_frames)
            {
                for (const RefCounted* obj : frame.pendingDeletes)
                {
                    delete obj;
                }
                frame.pendingDeletes.clear();
            }
        }

        void BeginFrame()
        {
            m_currentFrameIndex = (m_currentFrameIndex + 1) % RVX_MAX_FRAME_COUNT;

            // Delete resources from N frames ago (safe to delete now)
            auto& frame = m_frames[m_currentFrameIndex];
            for (const RefCounted* obj : frame.pendingDeletes)
            {
                delete obj;
            }
            frame.pendingDeletes.clear();
        }

        void EndFrame()
        {
            m_frameCounter++;
        }

        void DeferredDelete(const RefCounted* object) override
        {
            m_frames[m_currentFrameIndex].pendingDeletes.push_back(object);
        }

        uint32 GetCurrentFrameIndex() const { return m_currentFrameIndex; }
        uint64 GetFrameCounter() const { return m_frameCounter; }

    private:
        FrameResourceManager() = default;

        struct FrameResources
        {
            std::vector<const RefCounted*> pendingDeletes;
        };

        std::array<FrameResources, RVX_MAX_FRAME_COUNT> m_frames;
        uint32 m_currentFrameIndex = 0;
        uint64 m_frameCounter = 0;
    };

} // namespace RVX
