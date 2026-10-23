#include "Resource/ResourceRetirementLedger.h"

#include <algorithm>
#include <limits>

namespace RVX::Resource
{
    bool ResourceClosureRetirementOutcome::IsStructurallyValid() const noexcept
    {
        const auto hasDuplicateOrInvalid = [](const std::vector<ResourceId>& values)
        {
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (values[index] == InvalidResourceId ||
                    std::find(values.begin(), values.begin() + index,
                              values[index]) != values.begin() + index)
                {
                    return true;
                }
            }
            return false;
        };
        return rootAssetId.IsValid() && closureGeneration != 0 &&
               reason != ResourceClosureRetirementReason::Invalid &&
               !hasDuplicateOrInvalid(removedResourceIds) &&
               !hasDuplicateOrInvalid(sharedRetainedResourceIds) &&
               std::none_of(
                   removedResourceIds.begin(),
                   removedResourceIds.end(),
                   [this](ResourceId resourceId)
                   {
                       return std::find(sharedRetainedResourceIds.begin(),
                                        sharedRetainedResourceIds.end(),
                                        resourceId) !=
                              sharedRetainedResourceIds.end();
                   });
    }

    ResourceClosureRetirementSubmitResult ResourceRetirementLedger::Begin(
        const ResourceClosureRetirementOutcome& outcome,
        const std::vector<ResourceRetirementHandleCapture>& captures,
        IRenderResourceGateway* gateway) noexcept
    {
        ResourceClosureRetirementSubmitResult result;
        if (!outcome.IsStructurallyValid() ||
            !IsCaptureSetValid(outcome, captures))
        {
            return result;
        }
        if (!captures.empty() && gateway == nullptr)
        {
            result.code = ResourceClosureRetirementSubmitCode::GatewayUnavailable;
            return result;
        }

        const auto highest = m_highestClosureGeneration.find(
            outcome.rootAssetId);
        if (highest != m_highestClosureGeneration.end() &&
            outcome.closureGeneration <= highest->second)
        {
            result.code =
                ResourceClosureRetirementSubmitCode::DuplicateClosureGeneration;
            return result;
        }
        if (m_nextTokenValue == std::numeric_limits<uint64>::max())
        {
            result.code = ResourceClosureRetirementSubmitCode::TokenExhausted;
            return result;
        }
        try
        {
            Entry prepared;
            prepared.receipt.token = ResourceRetirementToken{
                m_nextTokenValue++, outcome.closureGeneration};
            prepared.receipt.rootAssetId = outcome.rootAssetId;
            prepared.receipt.closureGeneration = outcome.closureGeneration;
            prepared.receipt.reason = outcome.reason;
            prepared.receipt.state = ResourceClosureRetirementState::Requested;
            prepared.receipt.removedResourceCount =
                outcome.removedResourceIds.size();
            prepared.receipt.sharedRetainedResourceCount =
                outcome.sharedRetainedResourceIds.size();
            prepared.captures = captures;
            prepared.releaseAdmitted.assign(captures.size(), false);
            prepared.terminalObserved.assign(captures.size(), false);
            prepared.failureObserved.assign(captures.size(), false);

            // Preflight every exact generation before any release mutation. A
            // stale handle here is never evidence that this closure was admitted.
            for (const ResourceRetirementHandleCapture& capture : captures)
            {
                const RenderResourceStatus status =
                    gateway->QueryResourceStatus(capture.handle);
                if (HasDeviceLost(status))
                {
                    prepared.receipt.state =
                        ResourceClosureRetirementState::DeviceLost;
                    result.code = ResourceClosureRetirementSubmitCode::DeviceLost;
                    result.receipt = prepared.receipt;
                    break;
                }
                if (HasStaleStatus(status))
                {
                    prepared.receipt.state =
                        ResourceClosureRetirementState::Failed;
                    result.code =
                        ResourceClosureRetirementSubmitCode::StaleBeforeAdmission;
                    result.receipt = prepared.receipt;
                    break;
                }
            }

            // Persist the receipt and stale-generation tombstone before the
            // first RequestRelease. All mutation-path operations below are
            // allocation-free and the gateway contract itself is noexcept.
            const auto [entry, entryInserted] = m_entries.try_emplace(
                prepared.receipt.token.value, std::move(prepared));
            if (!entryInserted)
            {
                result.code =
                    ResourceClosureRetirementSubmitCode::AllocationFailure;
                return result;
            }

            bool insertedTombstone = false;
            try
            {
                if (highest == m_highestClosureGeneration.end())
                {
                    const auto [tombstone, tombstoneInserted] =
                        m_highestClosureGeneration.try_emplace(
                            outcome.rootAssetId,
                            outcome.closureGeneration);
                    (void)tombstone;
                    insertedTombstone = tombstoneInserted;
                    if (!tombstoneInserted)
                    {
                        m_entries.erase(entry);
                        result.code =
                            ResourceClosureRetirementSubmitCode::AllocationFailure;
                        return result;
                    }
                }
                else
                {
                    highest->second = outcome.closureGeneration;
                }
            }
            catch (...)
            {
                m_entries.erase(entry);
                if (insertedTombstone)
                {
                    m_highestClosureGeneration.erase(outcome.rootAssetId);
                }
                result.code =
                    ResourceClosureRetirementSubmitCode::AllocationFailure;
                return result;
            }

            Entry& persisted = entry->second;
            if (result.code == ResourceClosureRetirementSubmitCode::DeviceLost ||
                result.code ==
                    ResourceClosureRetirementSubmitCode::StaleBeforeAdmission)
            {
                return result;
            }

            for (size_t index = 0; index < persisted.captures.size(); ++index)
            {
                const RenderReleaseResult release =
                    gateway->RequestRelease(persisted.captures[index].handle);
                if (release.code == RenderReleaseCode::Accepted ||
                    release.code == RenderReleaseCode::AlreadyPending)
                {
                    persisted.releaseAdmitted[index] = true;
                    ++persisted.receipt.releaseAcceptedCount;
                    ++persisted.receipt.pendingGpuResourceCount;
                }
                else
                {
                    ++persisted.receipt.gatewayRejectedCount;
                }
            }

            if (persisted.receipt.pendingGpuResourceCount != 0)
            {
                persisted.receipt.state =
                    persisted.receipt.gatewayRejectedCount == 0
                        ? ResourceClosureRetirementState::ReleaseAccepted
                        : ResourceClosureRetirementState::AwaitingGpuLastUse;
            }
            else
            {
                persisted.receipt.state =
                    persisted.receipt.gatewayRejectedCount == 0
                        ? ResourceClosureRetirementState::Completed
                        : ResourceClosureRetirementState::Failed;
            }
            result.code = persisted.receipt.gatewayRejectedCount == 0
                              ? ResourceClosureRetirementSubmitCode::Accepted
                              : ResourceClosureRetirementSubmitCode::GatewayRejected;
            result.receipt = persisted.receipt;
            return result;
        }
        catch (...)
        {
            result.code = ResourceClosureRetirementSubmitCode::AllocationFailure;
            return result;
        }
    }

    ResourceClosureRetirementPollResult ResourceRetirementLedger::Poll(
        ResourceRetirementToken token,
        IRenderResourceGateway& gateway)
    {
        const auto found = m_entries.find(token.value);
        if (found == m_entries.end() || found->second.receipt.token != token)
        {
            return {};
        }
        return PollEntry(found->second, gateway);
    }

    void ResourceRetirementLedger::PollAll(IRenderResourceGateway& gateway)
    {
        for (auto& [value, entry] : m_entries)
        {
            (void)value;
            static_cast<void>(PollEntry(entry, gateway));
        }
    }

    void ResourceRetirementLedger::NotifyDeviceLost()
    {
        for (auto& [value, entry] : m_entries)
        {
            (void)value;
            if (!entry.receipt.IsTerminal())
            {
                entry.receipt.state = ResourceClosureRetirementState::DeviceLost;
                entry.receipt.pendingGpuResourceCount = 0;
                std::fill(entry.terminalObserved.begin(),
                          entry.terminalObserved.end(),
                          true);
            }
        }
    }

    std::optional<ResourceClosureRetirementReceipt>
    ResourceRetirementLedger::Query(ResourceRetirementToken token) const
    {
        const auto found = m_entries.find(token.value);
        if (found == m_entries.end() || found->second.receipt.token != token)
        {
            return std::nullopt;
        }
        return found->second.receipt;
    }

ResourceClosureRetirementAcknowledgeResult
ResourceRetirementLedger::Acknowledge(ResourceRetirementToken token)
    {
        ResourceClosureRetirementAcknowledgeResult result;
        const auto found = m_entries.find(token.value);
        if (found == m_entries.end() || found->second.receipt.token != token)
        {
            return result;
        }

        result.receipt = found->second.receipt;
        if (!result.receipt.IsTerminal())
        {
            result.code = ResourceClosureRetirementAcknowledgeCode::NotTerminal;
            return result;
        }

        result.code = ResourceClosureRetirementAcknowledgeCode::Acknowledged;
    m_entries.erase(found);
    return result;
}

const std::vector<ResourceRetirementHandleCapture>*
ResourceRetirementLedger::GetCaptures(ResourceRetirementToken token) const noexcept
{
    const auto found = m_entries.find(token.value);
    if (found == m_entries.end() || found->second.receipt.token != token)
    {
        return nullptr;
    }
    return &found->second.captures;
}

size_t ResourceRetirementLedger::GetOutstandingGpuRetirementCount() const noexcept
    {
        size_t count = 0;
        for (const auto& [value, entry] : m_entries)
        {
            (void)value;
            if (!entry.receipt.IsTerminal())
            {
                count += static_cast<size_t>(
                    entry.receipt.pendingGpuResourceCount);
            }
        }
        return count;
    }

    bool ResourceRetirementLedger::HasStaleStatus(
        const RenderResourceStatus& status) noexcept
    {
        return status.code == RenderResourceStatusCode::StaleGeneration ||
               status.code == RenderResourceStatusCode::InvalidHandle;
    }

    bool ResourceRetirementLedger::HasDeviceLost(
        const RenderResourceStatus& status) noexcept
    {
        return status.failure == RenderResourceFailureCode::DeviceLost;
    }

    bool ResourceRetirementLedger::IsReleasedOrStale(
        const RenderResourceStatus& status) noexcept
    {
        return status.code == RenderResourceStatusCode::StaleGeneration ||
               (status.code == RenderResourceStatusCode::Current &&
                status.state == RenderResourcePublicState::Released);
    }

    bool ResourceRetirementLedger::Contains(const std::vector<ResourceId>& values,
                                             ResourceId value) noexcept
    {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    bool ResourceRetirementLedger::IsCaptureSetValid(
        const ResourceClosureRetirementOutcome& outcome,
        const std::vector<ResourceRetirementHandleCapture>& captures) const noexcept
    {
        for (size_t index = 0; index < captures.size(); ++index)
        {
            const ResourceRetirementHandleCapture& capture = captures[index];
            if (!capture.IsValid() ||
                !Contains(outcome.removedResourceIds, capture.resourceId))
            {
                return false;
            }
            for (size_t prior = 0; prior < index; ++prior)
            {
                if (captures[prior].resourceId == capture.resourceId)
                {
                    return false;
                }
            }
        }
        return true;
    }

    ResourceClosureRetirementPollResult ResourceRetirementLedger::PollEntry(
        Entry& entry,
        IRenderResourceGateway& gateway)
    {
        ResourceClosureRetirementPollResult result;
        result.code = ResourceClosureRetirementPollCode::Updated;

        if (entry.receipt.IsTerminal())
        {
            result.receipt = entry.receipt;
            return result;
        }

        entry.receipt.state = ResourceClosureRetirementState::AwaitingGpuLastUse;
        uint64 pendingCount = 0;
        for (size_t index = 0; index < entry.captures.size(); ++index)
        {
            // Only an exact handle whose release was admitted may interpret a
            // later StaleGeneration as its own completed retirement.
            if (!entry.releaseAdmitted[index] || entry.terminalObserved[index])
            {
                continue;
            }

            const RenderResourceStatus status =
                gateway.QueryResourceStatus(entry.captures[index].handle);
            if (HasDeviceLost(status))
            {
                entry.receipt.state = ResourceClosureRetirementState::DeviceLost;
                entry.receipt.pendingGpuResourceCount = 0;
                std::fill(entry.terminalObserved.begin(),
                          entry.terminalObserved.end(),
                          true);
                result.receipt = entry.receipt;
                return result;
            }
            if (IsReleasedOrStale(status))
            {
                entry.terminalObserved[index] = true;
                continue;
            }
            if (status.code != RenderResourceStatusCode::Current ||
                status.state == RenderResourcePublicState::Failed)
            {
                if (!entry.failureObserved[index])
                {
                    entry.failureObserved[index] = true;
                    ++entry.receipt.gatewayRejectedCount;
                }
            }
            ++pendingCount;
        }

        entry.receipt.pendingGpuResourceCount = pendingCount;
        if (pendingCount == 0)
        {
            entry.receipt.state = entry.receipt.gatewayRejectedCount == 0
                                      ? ResourceClosureRetirementState::Completed
                                      : ResourceClosureRetirementState::Failed;
        }
        result.receipt = entry.receipt;
        return result;
    }
} // namespace RVX::Resource
