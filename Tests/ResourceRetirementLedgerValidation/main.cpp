#include "Resource/ResourceRetirementLedger.h"
#include "Resource/ResourceSubsystem.h"

#include <gtest/gtest.h>

#include <deque>
#include <type_traits>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using namespace RVX;
    using namespace RVX::Resource;

    class RetirementGateway final : public IRenderResourceGateway
    {
    public:
        RenderResourceReserveResult ReserveResource(
            AssetId,
            RenderResourceKind) noexcept override
        {
            return {};
        }

        RenderUploadEnqueueResult TryEnqueueUpload(
            const ResourceUploadRequestRef&) noexcept override
        {
            return {};
        }

        RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept override
        {
            releaseRequests.push_back(handle);
            if (releaseResults.empty())
            {
                return {RenderReleaseCode::Accepted};
            }
            const RenderReleaseCode code = releaseResults.front();
            releaseResults.pop_front();
            return {code};
        }

        [[nodiscard]] RenderResourceStatus QueryResourceStatus(
            RenderResourceHandle handle) const noexcept override
        {
            const auto found = statuses.find(handle);
            return found == statuses.end() ? RenderResourceStatus{} : found->second;
        }

        void SetCurrent(RenderResourceHandle handle,
                        RenderResourcePublicState state,
                        RenderResourceFailureCode failure =
                            RenderResourceFailureCode::None)
        {
            statuses[handle] = RenderResourceStatus{
                RenderResourceStatusCode::Current,
                state,
                failure};
        }

        void SetStale(RenderResourceHandle handle)
        {
            statuses[handle] = RenderResourceStatus{
                RenderResourceStatusCode::StaleGeneration};
        }

        std::unordered_map<RenderResourceHandle,
                           RenderResourceStatus,
                           RenderResourceHandleHash>
            statuses{};
        std::deque<RenderReleaseCode> releaseResults{};
        std::vector<RenderResourceHandle> releaseRequests{};
    };

    ResourceClosureRetirementOutcome MakeOutcome(
        AssetId rootAssetId,
        uint64 generation,
        std::vector<ResourceId> removed,
        std::vector<ResourceId> shared = {})
    {
        return ResourceClosureRetirementOutcome{
            rootAssetId,
            generation,
            ResourceClosureRetirementReason::ExplicitUnload,
            std::move(removed),
            std::move(shared)};
    }
} // namespace

TEST(ResourceRetirementLedgerValidation, AcceptedReleaseCompletesOnlyAfterReleased)
{
    ResourceRetirementLedger ledger;
    RetirementGateway gateway;
    const RenderResourceHandle handle{7, 3};
    gateway.SetCurrent(handle, RenderResourcePublicState::GPUReady);

    const ResourceClosureRetirementSubmitResult begin = ledger.Begin(
        MakeOutcome(AssetId{100}, 1, {101}),
        {{101, handle}},
        gateway);
    ASSERT_TRUE(begin.IsAccepted());
    EXPECT_EQ(begin.receipt.state,
              ResourceClosureRetirementState::ReleaseAccepted);
    EXPECT_EQ(begin.receipt.releaseAcceptedCount, 1u);
    EXPECT_EQ(begin.receipt.pendingGpuResourceCount, 1u);

    gateway.SetCurrent(handle, RenderResourcePublicState::Evicting);
    const ResourceClosureRetirementPollResult waiting =
        ledger.Poll(begin.receipt.token, gateway);
    EXPECT_EQ(waiting.code, ResourceClosureRetirementPollCode::Updated);
    EXPECT_EQ(waiting.receipt.state,
              ResourceClosureRetirementState::AwaitingGpuLastUse);
    EXPECT_EQ(waiting.receipt.pendingGpuResourceCount, 1u);

    gateway.SetCurrent(handle, RenderResourcePublicState::Released);
    const ResourceClosureRetirementPollResult completed =
        ledger.Poll(begin.receipt.token, gateway);
    EXPECT_EQ(completed.receipt.state,
              ResourceClosureRetirementState::Completed);
    EXPECT_EQ(completed.receipt.pendingGpuResourceCount, 0u);
}

TEST(ResourceRetirementLedgerValidation,
     CpuOnlyClosureCompletesWithoutRenderGateway)
{
    ResourceRetirementLedger ledger;
    const ResourceClosureRetirementSubmitResult begin = ledger.Begin(
        MakeOutcome(AssetId{150}, 1, {151}),
        {},
        static_cast<IRenderResourceGateway*>(nullptr));

    ASSERT_TRUE(begin.IsAccepted());
    EXPECT_TRUE(begin.receipt.IsValid());
    EXPECT_EQ(begin.receipt.state, ResourceClosureRetirementState::Completed);
    EXPECT_EQ(begin.receipt.removedResourceCount, 1u);
    EXPECT_EQ(begin.receipt.releaseAcceptedCount, 0u);
    EXPECT_EQ(begin.receipt.pendingGpuResourceCount, 0u);
    const std::vector<ResourceRetirementHandleCapture>* captures =
        ledger.GetCaptures(begin.receipt.token);
    ASSERT_NE(captures, nullptr);
    EXPECT_TRUE(captures->empty());
    EXPECT_EQ(ledger.GetOutstandingGpuRetirementCount(), 0u);
    EXPECT_EQ(ledger.Acknowledge(begin.receipt.token).code,
              ResourceClosureRetirementAcknowledgeCode::Acknowledged);
}

TEST(ResourceRetirementLedgerValidation,
     CapturedClosureRequiresRenderGateway)
{
    ResourceRetirementLedger ledger;
    const RenderResourceHandle handle{19, 1};
    const ResourceClosureRetirementSubmitResult begin = ledger.Begin(
        MakeOutcome(AssetId{160}, 1, {161}),
        {{161, handle}},
        static_cast<IRenderResourceGateway*>(nullptr));

    EXPECT_EQ(begin.code,
              ResourceClosureRetirementSubmitCode::GatewayUnavailable);
    EXPECT_FALSE(begin.receipt.IsValid());

    RetirementGateway gateway;
    gateway.SetCurrent(handle, RenderResourcePublicState::GPUReady);
    const ResourceClosureRetirementSubmitResult retry = ledger.Begin(
        MakeOutcome(AssetId{160}, 1, {161}), {{161, handle}}, gateway);
    ASSERT_TRUE(retry.IsAccepted());
    EXPECT_EQ(retry.receipt.token.value, 1u);
}

static_assert(
    noexcept(std::declval<ResourceSubsystem&>().BeginSceneAssetClosureRelease(
        std::declval<AssetResidencyLease&&>())),
    "Scene closure begin is a service boundary and must report failure by value.");

TEST(ResourceRetirementLedgerValidation, StaleBeforeAdmissionRejectsWithoutRelease)
{
    ResourceRetirementLedger ledger;
    RetirementGateway gateway;
    const RenderResourceHandle handle{8, 2};
    gateway.SetStale(handle);

    const ResourceClosureRetirementSubmitResult result = ledger.Begin(
        MakeOutcome(AssetId{200}, 1, {201}),
        {{201, handle}},
        gateway);

    EXPECT_EQ(result.code,
              ResourceClosureRetirementSubmitCode::StaleBeforeAdmission);
    EXPECT_EQ(result.receipt.state, ResourceClosureRetirementState::Failed);
    EXPECT_TRUE(result.receipt.IsValid());
    EXPECT_EQ(ledger.Acknowledge(result.receipt.token).code,
              ResourceClosureRetirementAcknowledgeCode::Acknowledged);
    EXPECT_TRUE(gateway.releaseRequests.empty());
}

TEST(ResourceRetirementLedgerValidation, AcceptedReleaseMayCompleteAfterStaleGeneration)
{
    ResourceRetirementLedger ledger;
    RetirementGateway gateway;
    const RenderResourceHandle handle{9, 5};
    gateway.SetCurrent(handle, RenderResourcePublicState::GPUReady);

    const ResourceClosureRetirementSubmitResult begin = ledger.Begin(
        MakeOutcome(AssetId{300}, 1, {301}),
        {{301, handle}},
        gateway);
    ASSERT_TRUE(begin.IsAccepted());

    gateway.SetStale(handle);
    const ResourceClosureRetirementPollResult completed =
        ledger.Poll(begin.receipt.token, gateway);
    EXPECT_EQ(completed.receipt.state,
              ResourceClosureRetirementState::Completed);
}

TEST(ResourceRetirementLedgerValidation, SharedRetainedResourcesNeverRequestRelease)
{
    ResourceRetirementLedger ledger;
    RetirementGateway gateway;
    const RenderResourceHandle removedHandle{10, 1};
    const RenderResourceHandle sharedHandle{11, 1};
    gateway.SetCurrent(removedHandle, RenderResourcePublicState::GPUReady);
    gateway.SetCurrent(sharedHandle, RenderResourcePublicState::GPUReady);

    const ResourceClosureRetirementSubmitResult begin = ledger.Begin(
        MakeOutcome(AssetId{400}, 1, {401}, {402}),
        {{401, removedHandle}},
        gateway);
    ASSERT_TRUE(begin.IsAccepted());
    EXPECT_EQ(begin.receipt.removedResourceCount, 1u);
    EXPECT_EQ(begin.receipt.sharedRetainedResourceCount, 1u);
    ASSERT_EQ(gateway.releaseRequests.size(), 1u);
    EXPECT_EQ(gateway.releaseRequests.front(), removedHandle);
    EXPECT_NE(gateway.releaseRequests.front(), sharedHandle);
}

TEST(ResourceRetirementLedgerValidation, ReusedRenderSlotRequiresNewClosureGeneration)
{
    ResourceRetirementLedger ledger;
    RetirementGateway gateway;
    const AssetId root{500};
    const RenderResourceHandle first{12, 1};
    const RenderResourceHandle reused{12, 2};

    gateway.SetCurrent(first, RenderResourcePublicState::GPUReady);
    const ResourceClosureRetirementSubmitResult firstBegin = ledger.Begin(
        MakeOutcome(root, 1, {501}), {{501, first}}, gateway);
    ASSERT_TRUE(firstBegin.IsAccepted());
    gateway.SetStale(first);
    ASSERT_EQ(ledger.Poll(firstBegin.receipt.token, gateway).receipt.state,
              ResourceClosureRetirementState::Completed);
    EXPECT_EQ(ledger.Acknowledge(firstBegin.receipt.token).code,
              ResourceClosureRetirementAcknowledgeCode::Acknowledged);

    gateway.SetCurrent(reused, RenderResourcePublicState::GPUReady);
    const ResourceClosureRetirementSubmitResult secondBegin = ledger.Begin(
        MakeOutcome(root, 2, {501}), {{501, reused}}, gateway);
    ASSERT_TRUE(secondBegin.IsAccepted());
    EXPECT_NE(secondBegin.receipt.token, firstBegin.receipt.token);
    ASSERT_EQ(gateway.releaseRequests.size(), 2u);
    EXPECT_EQ(gateway.releaseRequests[0], first);
    EXPECT_EQ(gateway.releaseRequests[1], reused);

    const ResourceClosureRetirementSubmitResult staleGeneration = ledger.Begin(
        MakeOutcome(root, 1, {501}), {{501, first}}, gateway);
    EXPECT_EQ(staleGeneration.code,
              ResourceClosureRetirementSubmitCode::DuplicateClosureGeneration);
}

TEST(ResourceRetirementLedgerValidation, PartialGatewayRejectionPreservesEvidence)
{
    ResourceRetirementLedger ledger;
    RetirementGateway gateway;
    const RenderResourceHandle first{13, 1};
    const RenderResourceHandle second{14, 1};
    gateway.SetCurrent(first, RenderResourcePublicState::GPUReady);
    gateway.SetCurrent(second, RenderResourcePublicState::GPUReady);
    gateway.releaseResults = {RenderReleaseCode::Accepted,
                              RenderReleaseCode::ShuttingDown};

    const ResourceClosureRetirementSubmitResult result = ledger.Begin(
        MakeOutcome(AssetId{600}, 1, {601, 602}),
        {{601, first}, {602, second}},
        gateway);

    EXPECT_EQ(result.code, ResourceClosureRetirementSubmitCode::GatewayRejected);
    EXPECT_EQ(result.receipt.state,
              ResourceClosureRetirementState::AwaitingGpuLastUse);
    EXPECT_EQ(result.receipt.releaseAcceptedCount, 1u);
    EXPECT_EQ(result.receipt.gatewayRejectedCount, 1u);
    EXPECT_EQ(result.receipt.pendingGpuResourceCount, 1u);
    EXPECT_EQ(gateway.releaseRequests.size(), 2u);
    EXPECT_EQ(ledger.GetOutstandingGpuRetirementCount(), 1u);
    EXPECT_EQ(ledger.Acknowledge(result.receipt.token).code,
              ResourceClosureRetirementAcknowledgeCode::NotTerminal);

    gateway.SetStale(first);
    const ResourceClosureRetirementPollResult completed =
        ledger.Poll(result.receipt.token, gateway);
    EXPECT_EQ(completed.receipt.state, ResourceClosureRetirementState::Failed);
    EXPECT_EQ(completed.receipt.pendingGpuResourceCount, 0u);
    EXPECT_EQ(ledger.GetOutstandingGpuRetirementCount(), 0u);
    EXPECT_EQ(ledger.Acknowledge(result.receipt.token).code,
              ResourceClosureRetirementAcknowledgeCode::Acknowledged);
}

TEST(ResourceRetirementLedgerValidation, DeviceLostFailsExplicitly)
{
    ResourceRetirementLedger ledger;
    RetirementGateway gateway;
    const RenderResourceHandle handle{15, 1};
    gateway.SetCurrent(handle, RenderResourcePublicState::GPUReady);

    const ResourceClosureRetirementSubmitResult begin = ledger.Begin(
        MakeOutcome(AssetId{700}, 1, {701}), {{701, handle}}, gateway);
    ASSERT_TRUE(begin.IsAccepted());
    EXPECT_EQ(ledger.GetOutstandingGpuRetirementCount(), 1u);

    gateway.SetCurrent(handle,
                       RenderResourcePublicState::Evicting,
                       RenderResourceFailureCode::DeviceLost);
    const ResourceClosureRetirementPollResult result =
        ledger.Poll(begin.receipt.token, gateway);
    EXPECT_EQ(result.receipt.state, ResourceClosureRetirementState::DeviceLost);
    EXPECT_EQ(result.receipt.pendingGpuResourceCount, 0u);
    EXPECT_EQ(ledger.GetOutstandingGpuRetirementCount(), 0u);
}

TEST(ResourceRetirementLedgerValidation,
     NotifyDeviceLostClearsPartialRetirementOutstandingCount)
{
    ResourceRetirementLedger ledger;
    RetirementGateway gateway;
    const RenderResourceHandle accepted{17, 1};
    const RenderResourceHandle rejected{18, 1};
    gateway.SetCurrent(accepted, RenderResourcePublicState::GPUReady);
    gateway.SetCurrent(rejected, RenderResourcePublicState::GPUReady);
    gateway.releaseResults = {RenderReleaseCode::Accepted,
                              RenderReleaseCode::ShuttingDown};

    const ResourceClosureRetirementSubmitResult begin = ledger.Begin(
        MakeOutcome(AssetId{900}, 1, {901, 902}),
        {{901, accepted}, {902, rejected}},
        gateway);
    ASSERT_EQ(begin.code, ResourceClosureRetirementSubmitCode::GatewayRejected);
    EXPECT_EQ(begin.receipt.state,
              ResourceClosureRetirementState::AwaitingGpuLastUse);
    EXPECT_EQ(ledger.GetOutstandingGpuRetirementCount(), 1u);

    ledger.NotifyDeviceLost();
    const std::optional<ResourceClosureRetirementReceipt> receipt =
        ledger.Query(begin.receipt.token);
    ASSERT_TRUE(receipt.has_value());
    EXPECT_EQ(receipt->state, ResourceClosureRetirementState::DeviceLost);
    EXPECT_EQ(receipt->pendingGpuResourceCount, 0u);
    EXPECT_EQ(ledger.GetOutstandingGpuRetirementCount(), 0u);
    EXPECT_EQ(ledger.Acknowledge(begin.receipt.token).code,
              ResourceClosureRetirementAcknowledgeCode::Acknowledged);
}

TEST(ResourceRetirementLedgerValidation, StaleAndDoubleAcknowledgementReject)
{
    ResourceRetirementLedger ledger;
    RetirementGateway gateway;
    const RenderResourceHandle handle{16, 1};
    gateway.SetCurrent(handle, RenderResourcePublicState::GPUReady);

    const ResourceClosureRetirementSubmitResult begin = ledger.Begin(
        MakeOutcome(AssetId{800}, 1, {801}), {{801, handle}}, gateway);
    ASSERT_TRUE(begin.IsAccepted());
    gateway.SetStale(handle);
    ASSERT_EQ(ledger.Poll(begin.receipt.token, gateway).receipt.state,
              ResourceClosureRetirementState::Completed);

    EXPECT_EQ(ledger.Acknowledge(begin.receipt.token).code,
              ResourceClosureRetirementAcknowledgeCode::Acknowledged);
    EXPECT_EQ(ledger.Acknowledge(begin.receipt.token).code,
              ResourceClosureRetirementAcknowledgeCode::StaleToken);
    EXPECT_EQ(ledger.Poll(begin.receipt.token, gateway).code,
              ResourceClosureRetirementPollCode::StaleToken);
}
