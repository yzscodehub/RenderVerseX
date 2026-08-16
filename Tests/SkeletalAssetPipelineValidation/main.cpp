#include "Core/Log.h"
#include "Core/Diagnostics/ContentHash.h"
#include "Geometry/Asset/Mesh.h"
#include "Resource/Cooked/CookedAnimationArtifact.h"
#include "Resource/Cooked/CookedModelArtifact.h"
#include "Resource/Importer/GLTFImporter.h"
#include "Resource/Loader/ModelLoader.h"
#include "Resource/Types/ModelResource.h"
#include "Tools/AssetPipeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;

enum class FixtureFault
{
    None,
    MultipleSkins,
    InverseBindCountMismatch,
    JointOutOfRange,
    ZeroWeights,
    MissingWeights,
    CubicSpline,
    OutputCountMismatch,
    DuplicateQuantizedTimes,
};

struct BufferRegion
{
    size_t offset = 0;
    size_t length = 0;
};

class ScopedGLB
{
public:
    ScopedGLB(std::string_view label, FixtureFault fault)
    {
        const auto token = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        m_path = fs::temp_directory_path() /
                 ("rvx_skeletal_" + std::string(label) + "_" +
                  std::to_string(token) + ".glb");
        const std::vector<RVX::uint8> bytes = Build(fault);
        std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file)
            throw std::runtime_error("Could not write skeletal GLB fixture.");
    }

    ~ScopedGLB()
    {
        std::error_code error;
        fs::remove(m_path, error);
    }

    const fs::path& Path() const { return m_path; }

private:
    template <typename T>
    static void AppendScalar(std::vector<RVX::uint8>& bytes, const T& value)
    {
        const size_t oldSize = bytes.size();
        bytes.resize(oldSize + sizeof(T));
        std::memcpy(bytes.data() + oldSize, &value, sizeof(T));
    }

    template <typename T>
    static BufferRegion AppendRegion(std::vector<RVX::uint8>& bytes,
                                     const std::vector<T>& values)
    {
        while ((bytes.size() & 3u) != 0u)
            bytes.push_back(0);
        BufferRegion region{bytes.size(), values.size() * sizeof(T)};
        const size_t oldSize = bytes.size();
        bytes.resize(oldSize + region.length);
        if (!values.empty())
        {
            std::memcpy(bytes.data() + oldSize,
                        values.data(),
                        region.length);
        }
        return region;
    }

    static std::string ViewJson(const BufferRegion& region)
    {
        std::ostringstream json;
        json << "{\"buffer\":0,\"byteOffset\":" << region.offset
             << ",\"byteLength\":" << region.length << "}";
        return json.str();
    }

    static std::vector<RVX::uint8> Build(FixtureFault fault)
    {
        std::vector<RVX::uint8> binary;
        const BufferRegion positions = AppendRegion<float>(
            binary,
            {0.0f, 0.0f, 0.0f,
             1.0f, 0.0f, 0.0f,
             0.0f, 1.0f, 0.0f});

        std::vector<RVX::uint8> jointValues = {
            1, 2, 0, 0,
            2, 0, 1, 0,
            0, 1, 2, 0,
        };
        if (fault == FixtureFault::JointOutOfRange)
            jointValues[0] = 3;
        const BufferRegion joints = AppendRegion(binary, jointValues);

        std::vector<RVX::uint8> weightValues = {
            128, 127, 0, 0,
            255, 0, 0, 0,
            192, 63, 0, 0,
        };
        if (fault == FixtureFault::ZeroWeights)
            std::fill_n(weightValues.begin(), 4, static_cast<RVX::uint8>(0));
        const BufferRegion weights = AppendRegion(binary, weightValues);
        const BufferRegion indices = AppendRegion<RVX::uint16>(binary, {0, 1, 2});

        // Skin joint order is [Foot, Root, Hips]. These are the corresponding
        // inverse global bind matrices in glTF column-major order.
        const BufferRegion inverseBinds = AppendRegion<float>(
            binary,
            {
                1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -2, 0, 1,
                1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1,
            });

        const BufferRegion times = AppendRegion<float>(
            binary,
            fault == FixtureFault::DuplicateQuantizedTimes
                ? std::vector<float>{0.0f, 0.0000004f}
                : std::vector<float>{0.0f, 1.0f});
        const BufferRegion translations = AppendRegion<float>(
            binary,
            {0.0f, 1.0f, 0.0f,
             0.0f, 1.0f, 1.0f});
        const float halfRoot = std::sqrt(0.5f);
        const BufferRegion rotations = AppendRegion<float>(
            binary,
            {0.0f, 0.0f, 0.0f, 1.0f,
             0.0f, halfRoot, 0.0f, halfRoot});

        std::ostringstream json;
        json << "{\"asset\":{\"version\":\"2.0\"},"
             << "\"buffers\":[{\"byteLength\":" << binary.size() << "}],"
             << "\"bufferViews\":["
             << ViewJson(positions) << ','
             << ViewJson(joints) << ','
             << ViewJson(weights) << ','
             << ViewJson(indices) << ','
             << ViewJson(inverseBinds) << ','
             << ViewJson(times) << ','
             << ViewJson(translations) << ','
             << ViewJson(rotations) << "],"
             << "\"accessors\":["
             << "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
             << "{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
             << "{\"bufferView\":2,\"componentType\":5121,\"normalized\":true,\"count\":3,\"type\":\"VEC4\"},"
             << "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
             << "{\"bufferView\":4,\"componentType\":5126,\"count\":"
             << (fault == FixtureFault::InverseBindCountMismatch ? 2 : 3)
             << ",\"type\":\"MAT4\"},"
             << "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
             << "{\"bufferView\":6,\"componentType\":5126,\"count\":"
             << (fault == FixtureFault::OutputCountMismatch ? 1 : 2)
             << ",\"type\":\"VEC3\"},"
             << "{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}],"
             << "\"meshes\":[{\"name\":\"BodyMesh\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1";
        if (fault != FixtureFault::MissingWeights)
            json << ",\"WEIGHTS_0\":2";
        json << "},\"indices\":3,\"mode\":4}]}],"
             << "\"nodes\":["
             << "{\"name\":\"Root\",\"children\":[1]},"
             << "{\"name\":\"Hips\",\"translation\":[0,1,0],\"children\":[2]},"
             << "{\"name\":\"Foot\",\"translation\":[0,1,0]},"
             << "{\"name\":\"SkinnedMesh\",\"mesh\":0,\"skin\":0}],"
             << "\"skins\":[{\"name\":\"Character\",\"inverseBindMatrices\":4,\"skeleton\":0,\"joints\":[2,0,1]}";
        if (fault == FixtureFault::MultipleSkins)
        {
            json << ", {\"name\":\"Second\",\"inverseBindMatrices\":4,\"skeleton\":0,\"joints\":[2,0,1]}";
        }
        json << "],\"animations\":["
             << "{\"name\":\"Run\",\"samplers\":[{\"input\":5,\"output\":6,\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}]},"
             << "{\"name\":\"Idle\",\"samplers\":[{\"input\":5,\"output\":7,\"interpolation\":\""
             << (fault == FixtureFault::CubicSpline ? "CUBICSPLINE" : "STEP")
             << "\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}}]},"
             << "{\"name\":\"Walk\",\"samplers\":[{\"input\":5,\"output\":6,\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}]}],"
             << "\"scenes\":[{\"nodes\":[0,3]}],\"scene\":0}";

        std::string jsonText = json.str();
        while ((jsonText.size() & 3u) != 0u)
            jsonText.push_back(' ');
        while ((binary.size() & 3u) != 0u)
            binary.push_back(0);

        constexpr RVX::uint32 glbMagic = 0x46546c67u;
        constexpr RVX::uint32 glbVersion = 2u;
        constexpr RVX::uint32 jsonChunkType = 0x4e4f534au;
        constexpr RVX::uint32 binChunkType = 0x004e4942u;
        const RVX::uint32 totalLength = static_cast<RVX::uint32>(
            12 + 8 + jsonText.size() + 8 + binary.size());

        std::vector<RVX::uint8> glb;
        glb.reserve(totalLength);
        AppendScalar(glb, glbMagic);
        AppendScalar(glb, glbVersion);
        AppendScalar(glb, totalLength);
        AppendScalar(glb, static_cast<RVX::uint32>(jsonText.size()));
        AppendScalar(glb, jsonChunkType);
        glb.insert(glb.end(), jsonText.begin(), jsonText.end());
        AppendScalar(glb, static_cast<RVX::uint32>(binary.size()));
        AppendScalar(glb, binChunkType);
        glb.insert(glb.end(), binary.begin(), binary.end());
        return glb;
    }

    fs::path m_path;
};

class TemporaryDirectory final
{
public:
    explicit TemporaryDirectory(std::string_view label)
    {
        const auto token = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        m_path = fs::temp_directory_path() /
                 ("rvx_skeletal_cook_" + std::string(label) + "_" +
                  std::to_string(token));
        fs::create_directories(m_path);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    const fs::path& Path() const { return m_path; }

private:
    fs::path m_path;
};

std::vector<RVX::uint8> ReadBytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

std::vector<RVX::uint8> RewriteV2Payload(
    const std::vector<RVX::uint8>& bytes,
    const std::function<void(std::vector<std::string>&)>& rewrite)
{
    constexpr std::string_view payloadBegin = "RVX_MODEL_PAYLOAD_BEGIN\n";
    constexpr std::string_view payloadEnd = "RVX_MODEL_PAYLOAD_END\n";
    const std::string serialized(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const size_t payloadStart = serialized.find(payloadBegin);
    const size_t payloadFinish = serialized.find(payloadEnd, payloadStart);
    if (payloadStart == std::string::npos || payloadFinish == std::string::npos)
        throw std::runtime_error("Cooked model fixture does not contain a payload.");

    const size_t payloadOffset = payloadStart + payloadBegin.size();
    std::istringstream source(serialized.substr(
        payloadOffset, payloadFinish - payloadOffset));
    std::vector<std::string> lines;
    std::string payload;
    std::string line;
    while (std::getline(source, line))
        lines.push_back(std::move(line));
    rewrite(lines);
    for (const std::string& rewrittenLine : lines)
        payload += rewrittenLine + '\n';

    RVX::uint64 hash = RVX::Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
    for (const char value : payload)
    {
        hash ^= static_cast<RVX::uint8>(value);
        hash *= RVX::Diagnostics::RVX_DIAGNOSTICS_FNV1A64_PRIME;
    }
    std::ostringstream rebuilt;
    rebuilt << RVX::Resource::RVX_MODEL_PREBAKE_MAGIC << '\n'
            << "schemaVersion=" << RVX::Resource::RVX_MODEL_PREBAKE_SCHEMA_VERSION
            << '\n'
            << "buildFingerprint="
            << RVX::Resource::RVX_MODEL_PREBAKE_BUILD_FINGERPRINT << '\n'
            << "contentHash=" << RVX::Diagnostics::FormatContentHash(hash) << '\n'
            << "payloadSize=" << payload.size() << '\n'
            << payloadBegin << payload << payloadEnd;
    const std::string text = rebuilt.str();
    return {text.begin(), text.end()};
}

std::vector<RVX::uint8> OmitV2PrimitiveMeshIndices(
    const std::vector<RVX::uint8>& bytes)
{
    return RewriteV2Payload(
        bytes,
        [](std::vector<std::string>& lines)
        {
            std::erase_if(
                lines,
                [](const std::string& line)
                {
                    return line.find(".meshIndexCount=") != std::string::npos ||
                           line.find(".meshIndex.") != std::string::npos;
                });
        });
}

bool ContainsBytes(const std::vector<RVX::uint8>& bytes,
                   const std::string& value)
{
    return !value.empty() &&
           std::search(bytes.begin(), bytes.end(),
                       value.begin(), value.end()) != bytes.end();
}

fs::path FindRepositoryAsset(const fs::path& relativePath)
{
    const auto searchFrom = [&relativePath](fs::path directory)
    {
        while (!directory.empty())
        {
            const fs::path candidate = directory / relativePath;
            if (fs::is_regular_file(candidate))
                return candidate;
            const fs::path parent = directory.parent_path();
            if (parent == directory)
                break;
            directory = parent;
        }
        return fs::path{};
    };

    if (const fs::path found = searchFrom(fs::current_path()); !found.empty())
        return found;
    return searchFrom(fs::absolute(fs::path(__FILE__)).parent_path());
}

class SkeletalAssetPipelineValidation : public testing::Test
{
protected:
    static void SetUpTestSuite() { RVX::Log::Initialize(); }
    static void TearDownTestSuite() { RVX::Log::Shutdown(); }
};

void ExpectCanonicalImport(const RVX::Resource::GLTFImportResult& result)
{
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(result.observedContentIdentity.IsValid());
    EXPECT_EQ(result.observedContentIdentity.fileCount, 1u);
    EXPECT_EQ(result.observedContentIdentity.scope,
              RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact);

    ASSERT_TRUE(result.skeleton);
    ASSERT_EQ(result.skeleton->GetBoneCount(), 3u);
    EXPECT_EQ(result.skeleton->bones[0].name, "Root");
    EXPECT_EQ(result.skeleton->bones[0].parentIndex, -1);
    EXPECT_EQ(result.skeleton->bones[1].name, "Root/Hips");
    EXPECT_EQ(result.skeleton->bones[1].parentIndex, 0);
    EXPECT_EQ(result.skeleton->bones[2].name, "Root/Hips/Foot");
    EXPECT_EQ(result.skeleton->bones[2].parentIndex, 1);

    ASSERT_EQ(result.meshes.size(), 1u);
    const RVX::VertexAttribute* jointAttribute =
        result.meshes[0]->GetAttribute(RVX::VertexBufferNames::BoneIndices);
    const RVX::VertexAttribute* weightAttribute =
        result.meshes[0]->GetAttribute(RVX::VertexBufferNames::BoneWeights);
    ASSERT_NE(jointAttribute, nullptr);
    ASSERT_NE(weightAttribute, nullptr);
    EXPECT_EQ(jointAttribute->GetType(), RVX::AttributeType::Int);
    EXPECT_EQ(weightAttribute->GetType(), RVX::AttributeType::Float);
    EXPECT_EQ(jointAttribute->GetVector<RVX::IVec4>(0), RVX::IVec4(0, 1, 2, 2));
    const RVX::Vec4 weights = weightAttribute->GetVector<RVX::Vec4>(0);
    EXPECT_NEAR(weights.x, 128.0f / 255.0f, 1.0e-6f);
    EXPECT_NEAR(weights.y, 127.0f / 255.0f, 1.0e-6f);
    EXPECT_NEAR(weights.x + weights.y + weights.z + weights.w, 1.0f, 1.0e-6f);

    ASSERT_TRUE(result.model);
    const RVX::Node::Ptr meshNode = result.model->GetNodeByName("SkinnedMesh");
    ASSERT_TRUE(meshNode);
    EXPECT_TRUE(meshNode->HasSkin());
    EXPECT_EQ(meshNode->GetSkinIndex(), 0);
    EXPECT_EQ(meshNode->GetMeshIndices(), (std::vector<int>{0}));

    ASSERT_EQ(result.animationClips.size(), 3u);
    auto clip = result.animationClips.begin();
    EXPECT_EQ((clip++)->first, "Idle");
    EXPECT_EQ((clip++)->first, "Run");
    EXPECT_EQ(clip->first, "Walk");

    const auto walk = result.animationClips.at("Walk");
    ASSERT_TRUE(walk);
    EXPECT_EQ(walk->duration, 1'000'000);
    const RVX::Animation::TransformTrack* hips =
        walk->FindTransformTrack("Root/Hips");
    ASSERT_NE(hips, nullptr);
    EXPECT_EQ(hips->targetType, RVX::Animation::TrackTargetType::Bone);
    ASSERT_EQ(hips->translationKeyframes.size(), 2u);
    EXPECT_EQ(hips->translationKeyframes.back().value, RVX::Vec3(0.0f, 1.0f, 1.0f));
    ASSERT_EQ(hips->rotationKeyframes.size(), 1u);
    ASSERT_EQ(hips->scaleKeyframes.size(), 1u);
}
} // namespace

TEST_F(SkeletalAssetPipelineValidation,
       SelfContainedGLBImportsCanonicalSkeletonWeightsAndNamedClipsDeterministically)
{
    ScopedGLB fixture("valid", FixtureFault::None);
    RVX::Resource::GLTFImporter importer;
    const auto first = importer.Import(fixture.Path().string());
    const auto second = importer.Import(fixture.Path().string());

    ExpectCanonicalImport(first);
    ExpectCanonicalImport(second);
    EXPECT_EQ(first.observedContentIdentity.digest,
              second.observedContentIdentity.digest);
    ASSERT_TRUE(first.skeleton && second.skeleton);
    for (size_t index = 0; index < first.skeleton->bones.size(); ++index)
    {
        EXPECT_EQ(first.skeleton->bones[index].name,
                  second.skeleton->bones[index].name);
        EXPECT_EQ(first.skeleton->bones[index].parentIndex,
                  second.skeleton->bones[index].parentIndex);
    }
}

TEST_F(SkeletalAssetPipelineValidation,
       VendoredCasualFemaleImportsPinnedSkinAndRequiredClips)
{
    const fs::path sourcePath = FindRepositoryAsset(
        "Samples/RenderVerseSamples/Assets/models/casual-female/Casual_Female.gltf");
    ASSERT_FALSE(sourcePath.empty());

    RVX::Resource::GLTFImporter importer;
    const RVX::Resource::GLTFImportResult result =
        importer.Import(sourcePath.string());
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(result.observedContentIdentity.IsValid());
    EXPECT_EQ(result.observedContentIdentity.scope,
              RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact);
    EXPECT_EQ(result.observedContentIdentity.digest,
              "87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a");
    EXPECT_EQ(result.observedContentIdentity.byteCount, 2'058'883u);
    EXPECT_EQ(result.observedContentIdentity.fileCount, 1u);

    ASSERT_TRUE(result.skeleton);
    EXPECT_EQ(result.skeleton->GetBoneCount(), 23u);
    EXPECT_GE(result.skeleton->FindBoneIndex("CharacterArmature/Bone/Body/Hips"), 0);
    ASSERT_EQ(result.animationClips.size(), 17u);
    for (const char* requiredClip : {"Idle", "Walk", "Run"})
    {
        const auto found = result.animationClips.find(requiredClip);
        ASSERT_NE(found, result.animationClips.end()) << requiredClip;
        ASSERT_TRUE(found->second) << requiredClip;
    }
    EXPECT_EQ(result.animationClips.at("Walk")->duration, 1'250'000);

    // The source has one glTF mesh with six material primitives; the importer
    // intentionally publishes one engine Mesh per primitive.
    ASSERT_EQ(result.meshes.size(), 6u);
    for (const RVX::Mesh::Ptr& mesh : result.meshes)
    {
        ASSERT_TRUE(mesh);
        const RVX::VertexAttribute* jointAttribute =
            mesh->GetAttribute(RVX::VertexBufferNames::BoneIndices);
        const RVX::VertexAttribute* weightAttribute =
            mesh->GetAttribute(RVX::VertexBufferNames::BoneWeights);
        ASSERT_NE(jointAttribute, nullptr);
        ASSERT_NE(weightAttribute, nullptr);
        EXPECT_EQ(jointAttribute->GetVertexCount(), mesh->GetVertexCount());
        EXPECT_EQ(weightAttribute->GetVertexCount(), mesh->GetVertexCount());
    }
    ASSERT_TRUE(result.model);
    std::vector<RVX::Node*> skinnedNodes;
    result.model->GetRootNode()->TraverseDepthFirst(
        [&skinnedNodes](RVX::Node* node)
        {
            if (node && node->HasSkin())
                skinnedNodes.push_back(node);
        });
    ASSERT_EQ(skinnedNodes.size(), 1u);
    EXPECT_EQ(skinnedNodes.front()->GetMeshIndices(),
              (std::vector<int>{0, 1, 2, 3, 4, 5}));
}

TEST_F(SkeletalAssetPipelineValidation,
       VendoredCasualFemaleCookedModelReloadsPinnedSkeletonAndClipLibrary)
{
    const fs::path cookedModel = FindRepositoryAsset(
        "Samples/RenderVerseSamples/Assets/cooked/casual-female/"
        "models/casual-female/Casual_Female.rva");
    ASSERT_FALSE(cookedModel.empty());

    RVX::Resource::ModelLoader loader(nullptr);
    std::unique_ptr<RVX::Resource::ModelResource> model(
        static_cast<RVX::Resource::ModelResource*>(
            loader.Load(cookedModel.string())));
    ASSERT_TRUE(model);
    ASSERT_TRUE(model->HasSkeleton());
    EXPECT_EQ(model->GetSkeleton()->GetBoneCount(), 23u);
    EXPECT_GE(model->GetSkeleton()->FindBoneIndex(
                  "CharacterArmature/Bone/Body/Hips"),
              0);
    ASSERT_TRUE(model->GetAnimationResource());
    EXPECT_TRUE(model->GetAnimationResource()->IsLoaded());
    EXPECT_EQ(model->GetAnimationClips().size(), 17u);
    for (const char* requiredClip : {"Idle", "Walk", "Run"})
    {
        const auto clip = model->GetAnimationClip(requiredClip);
        ASSERT_TRUE(clip) << requiredClip;
    }
    EXPECT_EQ(model->GetAnimationClip("Walk")->duration, 1'250'000);
    ASSERT_TRUE(model->GetRootNode());
    std::vector<RVX::Node*> skinnedNodes;
    model->GetRootNode()->TraverseDepthFirst(
        [&skinnedNodes](RVX::Node* node)
        {
            if (node && node->HasSkin())
                skinnedNodes.push_back(node);
        });
    ASSERT_EQ(skinnedNodes.size(), 1u);
    EXPECT_EQ(skinnedNodes.front()->GetMeshIndices(),
              (std::vector<int>{0, 1, 2, 3, 4, 5}));
    for (const int meshIndex : skinnedNodes.front()->GetMeshIndices())
    {
        const RVX::Resource::ResourceHandle<RVX::Resource::MeshResource> mesh =
            model->GetMesh(static_cast<size_t>(meshIndex));
        ASSERT_TRUE(mesh) << meshIndex;
        ASSERT_GT(mesh->GetLODCount(), 0u) << meshIndex;
        for (size_t lod = 0; lod < mesh->GetLODCount(); ++lod)
        {
            const RVX::Mesh::Ptr lodMesh = mesh->GetLODMesh(lod);
            ASSERT_TRUE(lodMesh) << meshIndex;
            EXPECT_NE(lodMesh->GetAttribute(RVX::VertexBufferNames::BoneIndices),
                      nullptr) << meshIndex;
            EXPECT_NE(lodMesh->GetAttribute(RVX::VertexBufferNames::BoneWeights),
                      nullptr) << meshIndex;
        }
    }
}

TEST_F(SkeletalAssetPipelineValidation,
       InvalidSkinTopologyIndicesAndWeightsFailWithoutPartialPublication)
{
    const std::array faults = {
        FixtureFault::MultipleSkins,
        FixtureFault::InverseBindCountMismatch,
        FixtureFault::JointOutOfRange,
        FixtureFault::ZeroWeights,
        FixtureFault::MissingWeights,
    };
    for (size_t index = 0; index < faults.size(); ++index)
    {
        ScopedGLB fixture("invalid_skin_" + std::to_string(index), faults[index]);
        RVX::Resource::GLTFImporter importer;
        const auto result = importer.Import(fixture.Path().string());
        EXPECT_FALSE(result.success) << "fault index " << index;
        EXPECT_FALSE(result.errorMessage.empty()) << "fault index " << index;
        EXPECT_FALSE(result.skeleton && !result.meshes.empty())
            << "A failed import must not expose both canonical skeleton and mesh payloads.";
    }
}

TEST_F(SkeletalAssetPipelineValidation,
       UnsupportedOrAmbiguousAnimationSamplingFailsClosed)
{
    const std::array faults = {
        FixtureFault::CubicSpline,
        FixtureFault::OutputCountMismatch,
        FixtureFault::DuplicateQuantizedTimes,
    };
    for (size_t index = 0; index < faults.size(); ++index)
    {
        ScopedGLB fixture("invalid_animation_" + std::to_string(index), faults[index]);
        RVX::Resource::GLTFImporter importer;
        const auto result = importer.Import(fixture.Path().string());
        EXPECT_FALSE(result.success) << "fault index " << index;
        EXPECT_FALSE(result.errorMessage.empty()) << "fault index " << index;
        EXPECT_TRUE(result.animationClips.empty())
            << "A failed animation import must not publish a partial clip library.";
    }
}

TEST_F(SkeletalAssetPipelineValidation,
       ModelLoaderPublishesImportedSkeletonAndClipLibrary)
{
    ScopedGLB fixture("model_loader", FixtureFault::None);
    RVX::Resource::ModelLoader loader(nullptr);
    std::unique_ptr<RVX::Resource::ModelResource> model(
        static_cast<RVX::Resource::ModelResource*>(
            loader.Load(fixture.Path().string())));
    ASSERT_TRUE(model);
    ASSERT_TRUE(model->HasSkeleton());
    EXPECT_EQ(model->GetSkeleton()->GetBoneCount(), 3u);
    EXPECT_TRUE(model->HasAnimationClips());
    EXPECT_TRUE(model->GetAnimationClip("Idle"));
    EXPECT_TRUE(model->GetAnimationClip("Walk"));
    EXPECT_TRUE(model->GetAnimationClip("Run"));
    EXPECT_FALSE(model->GetAnimationClip("Missing"));
}

TEST_F(SkeletalAssetPipelineValidation,
       CookedV2PublishesContainedAnimationAndReloadsCanonicalDependency)
{
    ScopedGLB fixture("cooked_v2", FixtureFault::None);
    TemporaryDirectory temporary("cooked_v2");
    const fs::path output = temporary.Path() / "Character.rva";

    RVX::Tools::ModelImporter importer;
    const RVX::Tools::ImportResult first = importer.Import(fixture.Path(), output);
    ASSERT_TRUE(first.success) << first.error;
    ASSERT_EQ(first.outputPaths.size(), 3u);

    const std::vector<RVX::uint8> firstModelBytes = ReadBytes(output);
    ASSERT_FALSE(firstModelBytes.empty());
    const std::string modelPrefix(
        reinterpret_cast<const char*>(firstModelBytes.data()),
        std::min<size_t>(firstModelBytes.size(), 64u));
    EXPECT_TRUE(modelPrefix.starts_with(RVX::Resource::RVX_MODEL_PREBAKE_MAGIC));

    RVX::Resource::CookedModelArtifact modelArtifact;
    std::string error;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        firstModelBytes, modelArtifact, error)) << error;
    ASSERT_FALSE(modelArtifact.animationArtifactPath.empty());
    ASSERT_TRUE(modelArtifact.rootNode);
    RVX::Node* skinnedNode = nullptr;
    modelArtifact.rootNode->TraverseDepthFirst(
        [&](RVX::Node* node)
        {
            if (node && node->HasSkin())
                skinnedNode = node;
        });
    ASSERT_NE(skinnedNode, nullptr);
    EXPECT_EQ(skinnedNode->GetSkinIndex(), 0);
    EXPECT_EQ(skinnedNode->GetMeshIndices(), (std::vector<int>{0}));

    // V2 predates explicit primitive mappings.  The optional fields may be
    // absent only when the compatibility primary mesh index can supply the
    // unambiguous one-mesh fallback.
    RVX::Resource::CookedModelArtifact legacyV2Artifact;
    const std::vector<RVX::uint8> legacyV2Bytes =
        OmitV2PrimitiveMeshIndices(firstModelBytes);
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        legacyV2Bytes, legacyV2Artifact, error)) << error;
    ASSERT_TRUE(legacyV2Artifact.rootNode);
    RVX::Node* legacySkinnedNode = nullptr;
    legacyV2Artifact.rootNode->TraverseDepthFirst(
        [&legacySkinnedNode](RVX::Node* node)
        {
            if (node && node->HasSkin())
                legacySkinnedNode = node;
        });
    ASSERT_NE(legacySkinnedNode, nullptr);
    EXPECT_FALSE(legacySkinnedNode->HasExplicitMeshIndices());
    EXPECT_TRUE(legacySkinnedNode->GetMeshIndices().empty());
    EXPECT_EQ(legacySkinnedNode->GetMeshIndex(), 0);

    const auto expectRejectedPrimitiveMapping =
        [](const std::vector<RVX::uint8>& malformedBytes)
        {
            RVX::Resource::CookedModelArtifact rejected;
            std::string rejectedError;
            EXPECT_FALSE(RVX::Resource::DeserializeCookedModelArtifact(
                malformedBytes, rejected, rejectedError));
            EXPECT_FALSE(rejectedError.empty());
        };
    expectRejectedPrimitiveMapping(RewriteV2Payload(
        firstModelBytes,
        [](std::vector<std::string>& lines)
        {
            for (std::string& line : lines)
            {
                if (line.ends_with(".meshIndexCount=1"))
                {
                    line = line.substr(0, line.find('=')) + "=4294967295";
                    return;
                }
            }
        }));
    expectRejectedPrimitiveMapping(RewriteV2Payload(
        firstModelBytes,
        [](std::vector<std::string>& lines)
        {
            for (std::string& line : lines)
            {
                if (line.ends_with(".meshIndexCount=1"))
                {
                    line = line.substr(0, line.find('=')) + "=2";
                    return;
                }
            }
        }));
    expectRejectedPrimitiveMapping(RewriteV2Payload(
        firstModelBytes,
        [](std::vector<std::string>& lines)
        {
            std::string nodePrefix;
            for (std::string& line : lines)
            {
                if (line.ends_with(".meshIndexCount=1"))
                {
                    const size_t equals = line.find('=');
                    nodePrefix = line.substr(0, equals -
                                             std::string_view(".meshIndexCount").size());
                    line = line.substr(0, equals) + "=2";
                    break;
                }
            }
            for (std::string& line : lines)
            {
                if (line == nodePrefix + ".materialCount=1")
                {
                    line = nodePrefix + ".materialCount=2";
                    break;
                }
            }
            lines.push_back(nodePrefix + ".meshIndex.1=0");
            lines.push_back(nodePrefix + ".material.1=0");
        }));

    const std::vector<int> originalMeshIndices = skinnedNode->GetMeshIndices();
    const std::vector<int> originalMaterialIndices =
        skinnedNode->GetMaterialIndices();
    std::vector<RVX::uint8> rejectedSerialization;
    skinnedNode->SetMeshIndices({0, 1});
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        modelArtifact, rejectedSerialization, error));
    skinnedNode->SetMeshIndices({0, 0});
    skinnedNode->SetMaterialIndices({0, 0});
    EXPECT_FALSE(RVX::Resource::SerializeCookedModelArtifact(
        modelArtifact, rejectedSerialization, error));
    skinnedNode->SetMeshIndices(originalMeshIndices);
    skinnedNode->SetMaterialIndices(originalMaterialIndices);

    const fs::path animationPath =
        output.parent_path() / modelArtifact.animationArtifactPath;
    const fs::path meshPath =
        output.parent_path() / modelArtifact.meshArtifactPath;
    ASSERT_TRUE(fs::is_regular_file(animationPath));
    ASSERT_TRUE(fs::is_regular_file(meshPath));
    const fs::path canonicalAnimationPath = fs::weakly_canonical(animationPath);
    EXPECT_TRUE(std::any_of(
        first.outputPaths.begin(),
        first.outputPaths.end(),
        [&](const std::string& product)
        {
            std::error_code productError;
            const fs::path canonicalProduct =
                fs::weakly_canonical(fs::path(product), productError);
            return !productError && canonicalProduct == canonicalAnimationPath;
        }));

    const std::vector<RVX::uint8> firstAnimationBytes = ReadBytes(animationPath);
    const std::vector<RVX::uint8> firstMeshBytes = ReadBytes(meshPath);
    RVX::Resource::CookedAnimationArtifact animationArtifact;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedAnimationArtifact(
        firstAnimationBytes, animationArtifact, error)) << error;
    ASSERT_TRUE(animationArtifact.skeleton);
    EXPECT_EQ(animationArtifact.skeleton->GetBoneCount(), 3u);
    EXPECT_EQ(animationArtifact.clips.size(), 3u);
    EXPECT_TRUE(animationArtifact.clips.contains("Idle"));
    EXPECT_TRUE(animationArtifact.clips.contains("Walk"));
    EXPECT_TRUE(animationArtifact.clips.contains("Run"));

    const RVX::Tools::ImportResult second = importer.Import(fixture.Path(), output);
    ASSERT_TRUE(second.success) << second.error;
    EXPECT_EQ(ReadBytes(output), firstModelBytes);
    EXPECT_EQ(ReadBytes(animationPath), firstAnimationBytes);
    EXPECT_EQ(ReadBytes(meshPath), firstMeshBytes);

    RVX::Resource::ModelLoader loader(nullptr);
    std::unique_ptr<RVX::Resource::ModelResource> model(
        static_cast<RVX::Resource::ModelResource*>(loader.Load(output.string())));
    ASSERT_TRUE(model);
    const RVX::Resource::AnimationHandle animation =
        model->GetAnimationResource();
    ASSERT_TRUE(animation);
    EXPECT_TRUE(animation->IsLoaded());
    EXPECT_EQ(model->GetSkeleton(), animation->GetSkeleton());
    EXPECT_EQ(model->GetAnimationClips().size(), 3u);
    EXPECT_TRUE(model->GetAnimationClip("Walk"));

    std::vector<RVX::uint8> corruptAnimation = firstAnimationBytes;
    corruptAnimation.back() ^= 0x1u;
    std::ofstream corruptFile(animationPath,
                              std::ios::binary | std::ios::trunc);
    corruptFile.write(
        reinterpret_cast<const char*>(corruptAnimation.data()),
        static_cast<std::streamsize>(corruptAnimation.size()));
    corruptFile.close();
    EXPECT_EQ(loader.Load(output.string()), nullptr);
}

TEST_F(SkeletalAssetPipelineValidation,
       CookManifestCapturesWholeSkeletalProductAndTamperPreservesPublishedPackage)
{
    ScopedGLB fixture("cook_manifest", FixtureFault::None);
    TemporaryDirectory temporary("cook_manifest");
    const fs::path sourceRoot = temporary.Path() / "Source";
    const fs::path outputRoot = temporary.Path() / "Cooked";
    const fs::path sourcePath = sourceRoot / "Models" / "Character.glb";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(sourcePath.parent_path());
    fs::copy_file(fixture.Path(), sourcePath);

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(
        std::make_unique<RVX::Tools::ModelImporter>());
    const RVX::Tools::CookManifest first = pipeline.CookDirectory(
        sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(first.manifestWritten) << first.manifestError;
    ASSERT_EQ(first.entries.size(), 1u);
    const RVX::Tools::CookManifestEntry& entry = first.entries.front();
    ASSERT_TRUE(entry.success) << entry.error;
    EXPECT_EQ(entry.sourcePath, "Models/Character.glb");
    EXPECT_EQ(entry.sourceContent.byteCount, fs::file_size(sourcePath));
    EXPECT_EQ(entry.sourceContent.sha256.size(), 64u);
    ASSERT_EQ(entry.dependencies.size(), 2u);
    EXPECT_EQ(entry.dependencies[0].relativePath,
              "Models/Character.rvdeps/animation.rvxanim");
    EXPECT_EQ(entry.dependencies[1].relativePath,
              "Models/Character.rvdeps/meshes.rva");
    ASSERT_EQ(entry.artifacts.size(), 3u);
    EXPECT_EQ(entry.artifacts[0].relativePath, "Models/Character.rva");
    EXPECT_EQ(entry.artifacts[1].relativePath,
              "Models/Character.rvdeps/animation.rvxanim");
    EXPECT_EQ(entry.artifacts[2].relativePath,
              "Models/Character.rvdeps/meshes.rva");
    for (const RVX::Tools::CookContentIdentity& identity : entry.artifacts)
    {
        EXPECT_GT(identity.byteCount, 0u);
        EXPECT_EQ(identity.sha256.size(), 64u);
    }

    const fs::path publishedModel = outputRoot / "Models" / "Character.rva";
    const fs::path publishedAnimation =
        outputRoot / "Models" / "Character.rvdeps" / "animation.rvxanim";
    const std::vector<RVX::uint8> modelBefore = ReadBytes(publishedModel);
    const std::vector<RVX::uint8> animationBefore = ReadBytes(publishedAnimation);
    const std::vector<RVX::uint8> manifestBefore = ReadBytes(manifestPath);

    const RVX::Tools::CookManifest rejected = pipeline.CookDirectory(
        sourceRoot,
        outputRoot,
        true,
        manifestPath,
        nullptr,
        nullptr,
        [](RVX::Tools::CookManifest&,
           const fs::path& stagingRoot,
           std::string& outError)
        {
            const fs::path stagedAnimation =
                stagingRoot / "Models" / "Character.rvdeps" /
                "animation.rvxanim";
            std::fstream file(stagedAnimation,
                              std::ios::binary | std::ios::in | std::ios::out);
            if (!file.is_open())
            {
                outError = "Could not open the staged animation dependency.";
                return false;
            }
            file.seekg(-1, std::ios::end);
            char value = 0;
            file.read(&value, 1);
            value ^= 0x1;
            file.seekp(-1, std::ios::end);
            file.write(&value, 1);
            return file.good();
        });
    EXPECT_FALSE(rejected.manifestWritten);
    EXPECT_FALSE(rejected.manifestError.empty());
    EXPECT_EQ(ReadBytes(publishedModel), modelBefore);
    EXPECT_EQ(ReadBytes(publishedAnimation), animationBefore);
    EXPECT_EQ(ReadBytes(manifestPath), manifestBefore);
}

TEST_F(SkeletalAssetPipelineValidation,
       SkeletalCookProductsAreRootIndependentAndDoNotEmbedTemporaryPaths)
{
    ScopedGLB fixture("root_independent", FixtureFault::None);
    TemporaryDirectory temporary("root_independent");
    const fs::path firstSourceRoot = temporary.Path() / "FirstSource";
    const fs::path secondSourceRoot = temporary.Path() / "SecondSource";
    const fs::path relativeSource = "models/Character.glb";
    const fs::path firstSource = firstSourceRoot / relativeSource;
    const fs::path secondSource = secondSourceRoot / relativeSource;
    fs::create_directories(firstSource.parent_path());
    fs::create_directories(secondSource.parent_path());
    fs::copy_file(fixture.Path(), firstSource);
    fs::copy_file(fixture.Path(), secondSource);

    const fs::path firstOutputRoot = temporary.Path() / "FirstCooked";
    const fs::path secondOutputRoot = temporary.Path() / "SecondCooked";
    RVX::Tools::AssetPipeline firstPipeline;
    firstPipeline.RegisterImporter(std::make_unique<RVX::Tools::ModelImporter>());
    const RVX::Tools::CookManifest first = firstPipeline.CookDirectory(
        firstSourceRoot,
        firstOutputRoot,
        true,
        firstOutputRoot / "CookManifest.rvxmanifest");
    ASSERT_TRUE(first.manifestWritten) << first.manifestError;

    RVX::Tools::AssetPipeline secondPipeline;
    secondPipeline.RegisterImporter(std::make_unique<RVX::Tools::ModelImporter>());
    const RVX::Tools::CookManifest second = secondPipeline.CookDirectory(
        secondSourceRoot,
        secondOutputRoot,
        true,
        secondOutputRoot / "CookManifest.rvxmanifest");
    ASSERT_TRUE(second.manifestWritten) << second.manifestError;

    const std::array artifactPaths = {
        fs::path("models/Character.rva"),
        fs::path("models/Character.rvdeps/meshes.rva"),
        fs::path("models/Character.rvdeps/animation.rvxanim"),
    };
    for (const fs::path& relativeArtifact : artifactPaths)
    {
        const std::vector<RVX::uint8> firstBytes =
            ReadBytes(firstOutputRoot / relativeArtifact);
        const std::vector<RVX::uint8> secondBytes =
            ReadBytes(secondOutputRoot / relativeArtifact);
        ASSERT_FALSE(firstBytes.empty()) << relativeArtifact.string();
        EXPECT_EQ(firstBytes, secondBytes) << relativeArtifact.string();
        EXPECT_FALSE(ContainsBytes(firstBytes, firstSourceRoot.generic_string()));
        EXPECT_FALSE(ContainsBytes(firstBytes, secondSourceRoot.generic_string()));
        EXPECT_FALSE(ContainsBytes(firstBytes, firstSourceRoot.string()));
        EXPECT_FALSE(ContainsBytes(firstBytes, secondSourceRoot.string()));
    }
}

TEST_F(SkeletalAssetPipelineValidation,
       NestedModelTransactionsFitQualificationWorktreePathBudget)
{
    ScopedGLB fixture("path_budget", FixtureFault::None);
    TemporaryDirectory temporary("path_budget");
    const fs::path sourceRoot = temporary.Path() / "Source";
    const fs::path sourcePath =
        sourceRoot / "models" / "casual-female" / "Character.glb";
    fs::create_directories(sourcePath.parent_path());
    fs::copy_file(fixture.Path(), sourcePath);

    fs::path outputParent = temporary.Path();
    constexpr size_t QualificationOutputParentLength = 112u;
    const size_t absoluteLength =
        fs::absolute(outputParent).generic_string().size();
    if (absoluteLength + 1u < QualificationOutputParentLength)
    {
        outputParent /= std::string(
            QualificationOutputParentLength - absoluteLength - 1u,
            'q');
    }
    fs::create_directories(outputParent);
    const fs::path outputRoot = outputParent / "casual-female";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::ModelImporter>());
    const RVX::Tools::CookManifest manifest = pipeline.CookDirectory(
        sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(manifest.manifestWritten) << manifest.manifestError;
    ASSERT_EQ(manifest.entries.size(), 1u);
    ASSERT_TRUE(manifest.entries.front().success)
        << manifest.entries.front().error;

    const fs::path animationPath =
        outputRoot / "models" / "casual-female" /
        "Character.rvdeps" / "animation.rvxanim";
    ASSERT_TRUE(fs::is_regular_file(animationPath));
    RVX::Resource::CookedAnimationArtifact animation;
    std::string error;
    EXPECT_TRUE(RVX::Resource::DeserializeCookedAnimationArtifact(
        ReadBytes(animationPath), animation, error)) << error;
    ASSERT_TRUE(animation.skeleton);
    EXPECT_EQ(animation.skeleton->GetBoneCount(), 3u);
    EXPECT_TRUE(animation.clips.contains("Walk"));
}
