#pragma once

/** @file RenderSubmissionTracker.h @brief Render-internal per-domain GPU completion tracking */

#include "RHI/RHIQueueTopology.h"
#include "RHI/RHISynchronization.h"

#include <array>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;

    struct GPUCompletionToken
    {
        std::array<GPUCompletionPoint, 3> points{};
        uint8 count = 0;
    };

    enum class GPUCompletionStatus : uint8
    {
        Completed = 0,
        Pending,
        Lost,
        CompatibilityWaitIdle,
    };

    bool InsertGPUCompletionPoint(GPUCompletionToken& token,
                                  GPUCompletionPoint point);
    bool MergeGPUCompletionToken(GPUCompletionToken& destination,
                                 const GPUCompletionToken& source);

    class RenderSubmissionTracker
    {
    public:
        RenderSubmissionTracker() = default;
        ~RenderSubmissionTracker();

        RenderSubmissionTracker(const RenderSubmissionTracker&) = delete;
        RenderSubmissionTracker& operator=(const RenderSubmissionTracker&) = delete;

        bool Initialize(IRHIDevice* device);
        void Shutdown();
        /** @brief Preserve timeline values while making every active domain terminally lost. */
        void MarkDeviceLost() noexcept;

        GPUCompletionPoint Submit(RHICommandContext* context);

        GPUCompletionStatus Query(GPUCompletionPoint point) const;
        GPUCompletionStatus Query(const GPUCompletionToken& token) const;
        GPUCompletionStatus Wait(GPUCompletionPoint point);
        GPUCompletionStatus Wait(const GPUCompletionToken& token);

        uint64 GetLastSubmittedValue(GPUQueueDomain domain) const;
        uint64 GetLastCompletedValue(GPUQueueDomain domain) const;
        /** @brief Capture exact non-zero last-submitted points for every active domain. */
        [[nodiscard]] GPUCompletionToken CaptureLastSubmittedToken() const;
        const RHIQueueTopology& GetTopology() const { return m_topology; }

    private:
        struct DomainState
        {
            RHIFenceRef fence;
            uint64 lastSubmittedValue = 0;
            mutable uint64 lastCompletedValue = 0;
            bool active = false;
            mutable bool lost = false;
        };

        DomainState* GetDomainState(GPUQueueDomain domain);
        const DomainState* GetDomainState(GPUQueueDomain domain) const;
        GPUCompletionStatus QueryCompatibilityPoint(GPUCompletionPoint point) const;

        IRHIDevice* m_device = nullptr;
        RHIQueueTopology m_topology;
        std::array<DomainState, 3> m_domains;
    };

} // namespace RVX
