#include "RenderContracts/ResourceUploadRequest.h"
#include "Runtime/RenderResourceStatusTable.h"

#include <gtest/gtest.h>

#include <memory>
#include <variant>

namespace RVX
{
namespace
{
    ResourceUploadRequestCreateInfo MakeTextureRequest(
        std::vector<uint8> bytes)
    {
        TextureUploadPayload payload;
        payload.createInfo.width = 1;
        payload.createInfo.height = 1;
        payload.createInfo.depth = 1;
        payload.createInfo.mipLevels = 1;
        payload.createInfo.arrayLayers = 1;
        payload.createInfo.format = TextureUploadFormat::RGBA8;
        payload.bytes = std::move(bytes);
        payload.subresources.push_back(TextureUploadSubresource{
            UploadByteRange{0, 4, 0}, 0, 0, 4, 4});

        ResourceUploadRequestCreateInfo info;
        info.sequence = 1;
        info.assetId = AssetId{1};
        info.handle = RenderResourceHandle{1, 1};
        info.kind = RenderResourceKind::Texture;
        info.operation = RenderResourceContentOperation::Replace;
        info.sourceRevision = 1;
        info.payload = std::move(payload);
        info.declaredPayloadBytes =
            4U + static_cast<uint64>(sizeof(TextureUploadSubresource));
        return info;
    }

    TEST(RenderResourceReplacementValidation,
         PayloadBytesAreRetainedAsValueOwnedStorage)
    {
        const std::vector<uint8> sourceBytes = {1, 2, 3, 4};
        ResourceUploadRequestCreateResult result =
            ResourceUploadRequest::Create(MakeTextureRequest(sourceBytes));

        ASSERT_EQ(result.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_NE(result.request, nullptr);
        EXPECT_EQ(result.request->GetOperation(),
                  RenderResourceContentOperation::Replace);
        EXPECT_EQ(result.request->GetSourceRevision(), 1U);
        const auto& payload = std::get<TextureUploadPayload>(
            result.request->GetPayload());
        EXPECT_EQ(payload.bytes, sourceBytes);
        const std::span<const uint8> payloadBytes =
            GetUploadPayloadBytes(payload);
        EXPECT_NE(payloadBytes.data(), sourceBytes.data());
        EXPECT_EQ(payloadBytes.size(), sourceBytes.size());
    }

    TEST(RenderResourceReplacementValidation,
         RejectsMissingReplacementRevision)
    {
        ResourceUploadRequestCreateInfo missingRevision =
            MakeTextureRequest({1, 2, 3, 4});
        missingRevision.sourceRevision = 0;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(missingRevision)).code,
                  ResourceUploadRequestCreateCode::InvalidSourceRevision);

        ResourceUploadRequestCreateInfo implicitReplacement =
            MakeTextureRequest({1, 2, 3, 4});
        implicitReplacement.sourceRevision = 0;
        implicitReplacement.provenance.sourceRevision = 7;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(implicitReplacement)).code,
                  ResourceUploadRequestCreateCode::InvalidSourceRevision);

        ResourceUploadRequestCreateInfo conflictingRevision =
            MakeTextureRequest({1, 2, 3, 4});
        conflictingRevision.sourceRevision = 7;
        conflictingRevision.provenance.sourceRevision = 8;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(conflictingRevision)).code,
                  ResourceUploadRequestCreateCode::ConflictingSourceRevision);
    }

    TEST(RenderResourceReplacementValidation,
         ReplacementStatusKeepsTheGenerationAndReturnsToUsableContent)
    {
        RenderResourceStatusTable table(1024);
        const RenderResourceHandle unassigned{1, 0};
        const RenderResourceHandle handle{1, 1};
        EXPECT_TRUE(table.CompareExchange(
            unassigned,
            PackedRenderResourceStatus{0,
                                       RenderResourcePublicState::Released,
                                       RenderResourceFailureCode::None},
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::Reserved,
                                       RenderResourceFailureCode::None},
            RenderStatusWriter::Update));
        EXPECT_TRUE(table.CompareExchange(
            handle,
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::Reserved,
                                       RenderResourceFailureCode::None},
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::UploadQueued,
                                       RenderResourceFailureCode::None},
            RenderStatusWriter::Update));
        EXPECT_TRUE(table.CompareExchange(
            handle,
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::UploadQueued,
                                       RenderResourceFailureCode::None},
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::Uploading,
                                       RenderResourceFailureCode::None},
            RenderStatusWriter::Render));
        EXPECT_TRUE(table.CompareExchange(
            handle,
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::Uploading,
                                       RenderResourceFailureCode::None},
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::GPUReady,
                                       RenderResourceFailureCode::None},
            RenderStatusWriter::Render));
        EXPECT_TRUE(table.CompareExchange(
            handle,
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::GPUReady,
                                       RenderResourceFailureCode::None},
            PackedRenderResourceStatus{
                1,
                RenderResourcePublicState::ReplacementQueued,
                RenderResourceFailureCode::None},
            RenderStatusWriter::Update));
        EXPECT_TRUE(table.CompareExchange(
            handle,
            PackedRenderResourceStatus{
                1,
                RenderResourcePublicState::ReplacementQueued,
                RenderResourceFailureCode::None},
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::Replacing,
                                       RenderResourceFailureCode::None},
            RenderStatusWriter::Render));
        EXPECT_TRUE(table.CompareExchange(
            handle,
            PackedRenderResourceStatus{1,
                                       RenderResourcePublicState::Replacing,
                                       RenderResourceFailureCode::None},
            PackedRenderResourceStatus{
                1,
                RenderResourcePublicState::GPUReady,
                RenderResourceFailureCode::ResourceCreationFailed},
            RenderStatusWriter::Render));

        const RenderResourceStatus status = table.Query(handle);
        EXPECT_EQ(status.code, RenderResourceStatusCode::Current);
        EXPECT_EQ(status.state, RenderResourcePublicState::GPUReady);
        EXPECT_EQ(status.failure,
                  RenderResourceFailureCode::ResourceCreationFailed);
    }
} // namespace
} // namespace RVX
