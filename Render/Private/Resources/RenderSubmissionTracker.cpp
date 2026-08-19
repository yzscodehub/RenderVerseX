#include "Resources/RenderSubmissionTracker.h"

#include "Core/Assert.h"
#include "RHI/RHICapabilities.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHISynchronization.h"

#include <algorithm>
#include <limits>

namespace RVX
{
    namespace
    {
        constexpr uint8 DomainIndex(GPUQueueDomain domain)
        {
            return static_cast<uint8>(domain);
        }

        constexpr bool IsConcreteBackendType(RHIBackendType backendType)
        {
            switch (backendType)
            {
                case RHIBackendType::DX11:
                case RHIBackendType::DX12:
                case RHIBackendType::Vulkan:
                case RHIBackendType::Metal:
                case RHIBackendType::OpenGL:
                    return true;
                case RHIBackendType::None:
                case RHIBackendType::Auto:
                default:
                    return false;
            }
        }

        bool IsCompletionSatisfied(GPUCompletionStatus status)
        {
            return status == GPUCompletionStatus::Completed ||
                   status == GPUCompletionStatus::CompatibilityWaitIdle;
        }

        bool IsValidCompletionToken(const GPUCompletionToken& token)
        {
            if (token.count > token.points.size())
            {
                return false;
            }

            for (uint8 i = 0; i < token.count; ++i)
            {
                const GPUCompletionPoint point = token.points[i];
                if (!IsDeclaredGPUQueueDomain(point.domain) || point.value == 0)
                {
                    return false;
                }
                if (i > 0 && DomainIndex(token.points[i - 1].domain) >= DomainIndex(point.domain))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool InsertGPUCompletionPoint(GPUCompletionToken& token,
                                  GPUCompletionPoint point)
    {
        if (!IsDeclaredGPUQueueDomain(point.domain) || point.value == 0 ||
            !IsValidCompletionToken(token))
        {
            return false;
        }

        for (uint8 i = 0; i < token.count; ++i)
        {
            if (token.points[i].domain == point.domain)
            {
                token.points[i].value = std::max(token.points[i].value, point.value);
                return true;
            }
        }

        if (token.count == 3)
        {
            return false;
        }

        token.points[token.count++] = point;
        std::sort(token.points.begin(), token.points.begin() + token.count,
                  [](const GPUCompletionPoint& left, const GPUCompletionPoint& right)
                  {
                      return DomainIndex(left.domain) < DomainIndex(right.domain);
                  });
        return true;
    }

    bool MergeGPUCompletionToken(GPUCompletionToken& destination,
                                 const GPUCompletionToken& source)
    {
        if (!IsValidCompletionToken(destination) || !IsValidCompletionToken(source))
        {
            return false;
        }

        GPUCompletionToken merged = destination;
        for (uint8 i = 0; i < source.count; ++i)
        {
            if (!InsertGPUCompletionPoint(merged, source.points[i]))
            {
                return false;
            }
        }

        destination = merged;
        return true;
    }

    RenderSubmissionTracker::~RenderSubmissionTracker()
    {
        Shutdown();
    }

    bool RenderSubmissionTracker::Initialize(IRHIDevice* device)
    {
        Shutdown();
        if (!device)
        {
            return false;
        }

        const RHICapabilities& capabilities = device->GetCapabilities();
        const RHIBackendType deviceBackend = device->GetBackendType();
        if (!IsConcreteBackendType(deviceBackend) ||
            !IsConcreteBackendType(capabilities.backendType) ||
            deviceBackend != capabilities.backendType)
        {
            return false;
        }
        if (!ValidateRHICapabilities(capabilities))
        {
            return false;
        }

        m_device = device;
        m_topology = capabilities.queueTopology;
        for (GPUQueueDomain domain : m_topology.logicalQueueDomains)
        {
            m_domains[DomainIndex(domain)].active = true;
        }

        if (m_topology.completionMode == RHIQueueCompletionMode::NativeTimeline)
        {
            for (DomainState& state : m_domains)
            {
                if (!state.active)
                {
                    continue;
                }

                state.fence = m_device->CreateFence(0);
                if (!state.fence)
                {
                    Shutdown();
                    return false;
                }
            }
        }

        return true;
    }

    void RenderSubmissionTracker::Shutdown()
    {
        for (DomainState& state : m_domains)
        {
            state.fence.Reset();
            state.lastSubmittedValue = 0;
            state.lastCompletedValue = 0;
            state.active = false;
            state.lost = false;
        }
        m_topology = {};
        m_device = nullptr;
    }

    void RenderSubmissionTracker::MarkDeviceLost() noexcept
    {
        for (DomainState& state : m_domains)
        {
            if (state.active)
            {
                state.lost = true;
            }
        }
    }

    GPUCompletionPoint RenderSubmissionTracker::Submit(RHICommandContext* context)
    {
        if (!m_device || !context)
        {
            return {};
        }
        if (m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
        {
            MarkDeviceLost();
            return {};
        }

        const RHICommandQueueType queueType = context->GetQueueType();
        GPUQueueDomain domain = GPUQueueDomain::Graphics;
        if (!TryGetGPUQueueDomain(m_topology, queueType, domain))
        {
            return {};
        }

        DomainState* state = GetDomainState(domain);
        if (!state || state->lost)
        {
            return {};
        }

        if (state->lastSubmittedValue == std::numeric_limits<uint64>::max())
        {
            state->lost = true;
            return {};
        }

        if (m_topology.completionMode == RHIQueueCompletionMode::CompatibilityWaitIdle)
        {
            m_device->SubmitCommandContext(context, nullptr);
            ++state->lastSubmittedValue;
            return {domain, state->lastSubmittedValue};
        }

        if (m_topology.completionMode != RHIQueueCompletionMode::NativeTimeline || !state->fence)
        {
            state->lost = true;
            return {};
        }

        const uint64 submittedValue = m_device->SubmitCommandContext(context, state->fence.Get());
        if (submittedValue == 0 || submittedValue <= state->lastSubmittedValue)
        {
            state->lost = true;
            return {};
        }

        state->lastSubmittedValue = submittedValue;
        return {domain, submittedValue};
    }

    GPUCompletionPoint RenderSubmissionTracker::Submit(
        std::span<RHICommandContext* const> contexts,
        GPUQueueDomain terminalDomain)
    {
        if (!m_device || contexts.empty() ||
            !IsDeclaredGPUQueueDomain(terminalDomain) ||
            m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
        {
            if (m_device &&
                m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
            {
                MarkDeviceLost();
            }
            return {};
        }

        bool containsTerminalDomain = false;
        for (RHICommandContext* context : contexts)
        {
            GPUQueueDomain contextDomain = GPUQueueDomain::Graphics;
            if (!context ||
                !TryGetGPUQueueDomain(
                    m_topology, context->GetQueueType(), contextDomain))
            {
                return {};
            }
            containsTerminalDomain |= contextDomain == terminalDomain;
        }
        if (!containsTerminalDomain)
        {
            return {};
        }

        DomainState* state = GetDomainState(terminalDomain);
        if (!state || state->lost ||
            state->lastSubmittedValue == std::numeric_limits<uint64>::max())
        {
            if (state)
            {
                state->lost = true;
            }
            return {};
        }

        if (m_topology.completionMode ==
            RHIQueueCompletionMode::CompatibilityWaitIdle)
        {
            m_device->SubmitCommandContexts(contexts, nullptr);
            ++state->lastSubmittedValue;
            return {terminalDomain, state->lastSubmittedValue};
        }
        if (m_topology.completionMode != RHIQueueCompletionMode::NativeTimeline ||
            !m_device->GetCapabilities().supportsMultiQueueBatchSubmit ||
            !state->fence)
        {
            state->lost = true;
            return {};
        }

        const uint64 submittedValue =
            m_device->SubmitCommandContexts(contexts, state->fence.Get());
        if (submittedValue == 0 || submittedValue <= state->lastSubmittedValue)
        {
            state->lost = true;
            return {};
        }
        state->lastSubmittedValue = submittedValue;
        return {terminalDomain, submittedValue};
    }

    GPUCompletionPoint RenderSubmissionTracker::Submit(
        const RHIQueueSubmissionPlan& plan)
    {
        constexpr GPUQueueDomain terminalDomain = GPUQueueDomain::Graphics;
        if (!m_device ||
            m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready ||
            !ValidateRHIQueueSubmissionPlan(plan))
        {
            if (m_device &&
                m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
            {
                MarkDeviceLost();
            }
            return {};
        }

        DomainState* state = GetDomainState(terminalDomain);
        if (!state || state->lost || !state->fence ||
            state->lastSubmittedValue == std::numeric_limits<uint64>::max() ||
            m_topology.completionMode != RHIQueueCompletionMode::NativeTimeline ||
            !m_device->GetCapabilities().supportsQueueSubmissionPlan)
        {
            if (state)
            {
                state->lost = true;
            }
            return {};
        }

        const uint64 submittedValue =
            m_device->SubmitQueuePlan(plan, state->fence.Get());
        if (submittedValue == 0 || submittedValue <= state->lastSubmittedValue)
        {
            state->lost = true;
            return {};
        }
        state->lastSubmittedValue = submittedValue;
        return {terminalDomain, submittedValue};
    }

    GPUCompletionStatus RenderSubmissionTracker::Query(GPUCompletionPoint point) const
    {
        if (!IsDeclaredGPUQueueDomain(point.domain) || point.value == 0)
        {
            return GPUCompletionStatus::Lost;
        }

        if (m_device == nullptr ||
            m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
        {
            for (const DomainState& domainState : m_domains)
            {
                if (domainState.active)
                {
                    domainState.lost = true;
                }
            }
            return GPUCompletionStatus::Lost;
        }

        const DomainState* state = GetDomainState(point.domain);
        if (!state || state->lost)
        {
            return GPUCompletionStatus::Lost;
        }

        if (m_topology.completionMode == RHIQueueCompletionMode::CompatibilityWaitIdle)
        {
            return QueryCompatibilityPoint(point);
        }
        if (m_topology.completionMode != RHIQueueCompletionMode::NativeTimeline || !state->fence)
        {
            return GPUCompletionStatus::Lost;
        }

        const uint64 completedValue = state->fence->GetCompletedValue();
        if (completedValue == std::numeric_limits<uint64>::max())
        {
            state->lost = true;
            return GPUCompletionStatus::Lost;
        }
        state->lastCompletedValue = std::max(state->lastCompletedValue, completedValue);
        return state->lastCompletedValue >= point.value
            ? GPUCompletionStatus::Completed
            : GPUCompletionStatus::Pending;
    }

    GPUCompletionStatus RenderSubmissionTracker::Query(const GPUCompletionToken& token) const
    {
        if (!IsValidCompletionToken(token))
        {
            return GPUCompletionStatus::Lost;
        }

        bool usedCompatibility = false;
        bool hasPending = false;
        for (uint8 i = 0; i < token.count; ++i)
        {
            const GPUCompletionStatus status = Query(token.points[i]);
            if (status == GPUCompletionStatus::Lost)
            {
                return status;
            }
            if (status == GPUCompletionStatus::Pending)
            {
                hasPending = true;
            }
            usedCompatibility |= status == GPUCompletionStatus::CompatibilityWaitIdle;
        }

        if (hasPending)
        {
            return GPUCompletionStatus::Pending;
        }
        return usedCompatibility
            ? GPUCompletionStatus::CompatibilityWaitIdle
            : GPUCompletionStatus::Completed;
    }

    GPUCompletionStatus RenderSubmissionTracker::Wait(GPUCompletionPoint point)
    {
        const GPUCompletionStatus status = Query(point);
        if (status != GPUCompletionStatus::Pending)
        {
            return status;
        }

        DomainState* state = GetDomainState(point.domain);
        if (!state)
        {
            return GPUCompletionStatus::Lost;
        }

        if (m_topology.completionMode == RHIQueueCompletionMode::CompatibilityWaitIdle)
        {
            m_device->WaitIdle();
            if (m_device->QueryRuntimeStatus() !=
                RHIDeviceRuntimeStatus::Ready)
            {
                MarkDeviceLost();
                return GPUCompletionStatus::Lost;
            }
            for (DomainState& domainState : m_domains)
            {
                if (domainState.active && !domainState.lost)
                {
                    domainState.lastCompletedValue = domainState.lastSubmittedValue;
                }
            }
            return QueryCompatibilityPoint(point);
        }

        if (!state->fence)
        {
            state->lost = true;
            return GPUCompletionStatus::Lost;
        }

        state->fence->Wait(point.value);
        if (m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
        {
            MarkDeviceLost();
            return GPUCompletionStatus::Lost;
        }
        return Query(point);
    }

    GPUCompletionStatus RenderSubmissionTracker::Wait(const GPUCompletionToken& token)
    {
        const GPUCompletionStatus initialStatus = Query(token);
        if (initialStatus != GPUCompletionStatus::Pending)
        {
            return initialStatus;
        }

        bool usedCompatibility = false;
        for (uint8 i = 0; i < token.count; ++i)
        {
            const GPUCompletionStatus status = Wait(token.points[i]);
            if (!IsCompletionSatisfied(status))
            {
                return status;
            }
            usedCompatibility |= status == GPUCompletionStatus::CompatibilityWaitIdle;
        }

        return usedCompatibility
            ? GPUCompletionStatus::CompatibilityWaitIdle
            : GPUCompletionStatus::Completed;
    }

    uint64 RenderSubmissionTracker::GetLastSubmittedValue(GPUQueueDomain domain) const
    {
        const DomainState* state = GetDomainState(domain);
        return state ? state->lastSubmittedValue : 0;
    }

    uint64 RenderSubmissionTracker::GetLastCompletedValue(GPUQueueDomain domain) const
    {
        const DomainState* state = GetDomainState(domain);
        if (!state)
        {
            return 0;
        }
        if (state->lost)
        {
            return state->lastCompletedValue;
        }
        if (m_topology.completionMode == RHIQueueCompletionMode::NativeTimeline && state->fence)
        {
            const uint64 completedValue = state->fence->GetCompletedValue();
            if (completedValue == std::numeric_limits<uint64>::max())
            {
                state->lost = true;
                return state->lastCompletedValue;
            }
            state->lastCompletedValue = std::max(state->lastCompletedValue, completedValue);
        }
        return state->lastCompletedValue;
    }

    GPUCompletionToken RenderSubmissionTracker::CaptureLastSubmittedToken() const
    {
        GPUCompletionToken token;
        for (uint8 index = 0; index < m_domains.size(); ++index)
        {
            const DomainState& state = m_domains[index];
            if (!state.active || state.lastSubmittedValue == 0)
            {
                continue;
            }
            const bool inserted = InsertGPUCompletionPoint(
                token,
                GPUCompletionPoint{
                    static_cast<GPUQueueDomain>(index),
                    state.lastSubmittedValue,
                });
            RVX_ASSERT_MSG(inserted,
                           "Active submission domain produced an invalid snapshot");
        }
        return token;
    }

    RenderSubmissionTracker::DomainState* RenderSubmissionTracker::GetDomainState(
        GPUQueueDomain domain)
    {
        if (!IsDeclaredGPUQueueDomain(domain))
        {
            return nullptr;
        }
        DomainState& state = m_domains[DomainIndex(domain)];
        return state.active ? &state : nullptr;
    }

    const RenderSubmissionTracker::DomainState* RenderSubmissionTracker::GetDomainState(
        GPUQueueDomain domain) const
    {
        if (!IsDeclaredGPUQueueDomain(domain))
        {
            return nullptr;
        }
        const DomainState& state = m_domains[DomainIndex(domain)];
        return state.active ? &state : nullptr;
    }

    GPUCompletionStatus RenderSubmissionTracker::QueryCompatibilityPoint(
        GPUCompletionPoint point) const
    {
        const DomainState* state = GetDomainState(point.domain);
        if (!state || state->lost)
        {
            return GPUCompletionStatus::Lost;
        }
        return state->lastCompletedValue >= point.value
            ? GPUCompletionStatus::CompatibilityWaitIdle
            : GPUCompletionStatus::Pending;
    }

} // namespace RVX
