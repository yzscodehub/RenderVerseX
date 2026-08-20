#pragma once

/** @file RenderSubmissionResourceBatch.h @brief Per-submission strong GPU ownership batch */

#include "Core/RefCounted.h"
#include "Core/Types.h"
#include "Resources/RenderSubmissionTracker.h"
#include "Runtime/RenderThreadGuard.h"

#include <vector>

namespace RVX
{
    class RenderRetirementQueue;

    /** @brief Lifetime policy assigned to every production GPU strong-reference holder. */
    enum class RenderRHILifetimePolicy : uint8
    {
        RegistryExactGeneration = 0,
        SubmissionBatch = 1,
        PoolAvailability = 2,
        OwnerSnapshot = 3,
        SurfaceGeneration = 4,
        ShutdownAfterDrain = 5,
    };

    /**
     * @brief Retains recording-time objects and transfers them to token retirement.
     *
     * A batch binds to the constructing Render Thread. Objects are deduplicated by
     * address, sealed exactly once, and never use a CPU frame index as completion
     * evidence.
     */
    class RenderSubmissionResourceBatch final : public NonMovable
    {
    public:
        RenderSubmissionResourceBatch();
        ~RenderSubmissionResourceBatch();

        /** @brief Retain one object while recording; rejects null, sealed, or wrong-thread calls. */
        [[nodiscard]] bool Retain(Ref<RefCounted> object,
                                  uint64 estimatedBytes = 0);

        /** @brief Seal with the actual submission token and transfer all refs to retirement. */
        void SealAndTransfer(const GPUCompletionToken& completion,
                             RenderRetirementQueue& retirement);

        /** @brief Seal a never-submitted batch with empty completion evidence. */
        void ReleaseUnsubmitted(RenderRetirementQueue& retirement);

        [[nodiscard]] bool IsSealed() const noexcept { return m_sealed; }
        [[nodiscard]] uint32 GetRetainedObjectCount() const noexcept;

    private:
        struct RetainedObject
        {
            Ref<RefCounted> object;
            uint64 estimatedBytes = 0;
        };

        [[nodiscard]] bool IsOnRenderThread() const noexcept;
        void Transfer(const GPUCompletionToken& completion,
                      RenderRetirementQueue& retirement);

        RenderThreadGuard m_renderThreadGuard;
        std::vector<RetainedObject> m_retained;
        bool m_sealed = false;
    };

    /** @brief Retain when a production batch is active; standalone no-submit paths are immediate. */
    [[nodiscard]] inline bool RetainRenderSubmissionResource(
        RenderSubmissionResourceBatch* batch,
        const Ref<RefCounted>& object,
        uint64 estimatedBytes = 0)
    {
        return object &&
               (batch == nullptr || batch->Retain(object, estimatedBytes));
    }
} // namespace RVX
