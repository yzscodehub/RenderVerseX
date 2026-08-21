#include "Resource/CookManifest.h"
#include "Resource/Cooked/CookedModelArtifact.h"
#include "Resource/Loader/CookedModelLoader.h"
#include "Resource/Types/ModelResource.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class TemporaryDirectory
    {
    public:
        TemporaryDirectory()
        {
            static std::atomic<uint64_t> sequence{0};
            m_path = fs::temp_directory_path() /
                ("rvx-fbx-cook-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()) +
                 "-" + std::to_string(++sequence));
            fs::create_directories(m_path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        const fs::path& Get() const { return m_path; }

    private:
        fs::path m_path;
    };

    std::vector<RVX::uint8> ReadBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
    }

    std::map<std::string, std::vector<RVX::uint8>> SnapshotTree(
        const fs::path& root)
    {
        std::map<std::string, std::vector<RVX::uint8>> snapshot;
        for (const fs::directory_entry& entry :
             fs::recursive_directory_iterator(root))
        {
            if (entry.is_regular_file())
            {
                snapshot.emplace(entry.path().lexically_relative(root).generic_string(),
                                 ReadBytes(entry.path()));
            }
        }
        return snapshot;
    }

    std::string Quote(const fs::path& path)
    {
        std::string value = path.string();
        std::string quoted = "\"";
        for (char character : value)
        {
            if (character == '"') quoted += '\\';
            quoted += character;
        }
        quoted += '"';
        return quoted;
    }

    std::string WrapSystemCommand(std::string command)
    {
#if defined(_WIN32)
        return "\"" + command + "\"";
#else
        return command;
#endif
    }

    int RunCook(const fs::path& sourceRoot, const fs::path& outputRoot)
    {
        const fs::path manifest = outputRoot / "CookManifest.rvxmanifest";
        const std::string command = WrapSystemCommand(
            Quote(fs::path(RVX_COOK_EXECUTABLE_PATH)) +
            " --source " + Quote(sourceRoot) +
            " --output " + Quote(outputRoot) +
            " --manifest " + Quote(manifest) +
            " --model-only --fail-on-errors");
        return std::system(command.c_str());
    }
} // namespace

TEST(FBXCookPipelineValidation, RVXCookProducesDeterministicV2AndRuntimeLoadsIt)
{
    TemporaryDirectory temporary;
    const fs::path sourceRoot = temporary.Get() / "Source";
    fs::create_directories(sourceRoot / "Models");
    fs::copy_file(fs::path(RVX_SOURCE_DIR) /
                      "Tests/Fixtures/FBX/static-binary.fbx",
                  sourceRoot / "Models/Static.fbx");

    const fs::path firstOutput = temporary.Get() / "CookedA";
    const fs::path secondOutput = temporary.Get() / "CookedB";
    ASSERT_EQ(RunCook(sourceRoot, firstOutput), 0);
    ASSERT_EQ(RunCook(sourceRoot, secondOutput), 0);
    EXPECT_EQ(SnapshotTree(firstOutput), SnapshotTree(secondOutput));

    const fs::path modelPath = firstOutput / "Models/Static.rva";
    const fs::path manifestPath = firstOutput / "CookManifest.rvxmanifest";
    ASSERT_TRUE(fs::is_regular_file(modelPath));
    ASSERT_TRUE(fs::is_regular_file(manifestPath));

    const std::vector<RVX::uint8> modelBytes = ReadBytes(modelPath);
    ASSERT_FALSE(modelBytes.empty());
    const std::string prefix(reinterpret_cast<const char*>(modelBytes.data()),
                             std::min<size_t>(modelBytes.size(), 64u));
    EXPECT_TRUE(prefix.starts_with(RVX::Resource::RVX_MODEL_ARTIFACT_MAGIC));

    RVX::Resource::CookedModelArtifact artifact;
    std::string error;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedModelArtifact(
        modelBytes, artifact, error)) << error;
    EXPECT_EQ(artifact.logicalAssetId, "Models/Static.fbx");
    EXPECT_FALSE(artifact.animationArtifactPath.has_value());
    EXPECT_TRUE(artifact.bounds.IsValid());

    RVX::Resource::CookManifest manifest;
    ASSERT_TRUE(RVX::Resource::LoadCookManifest(manifestPath, manifest, error))
        << error;
    ASSERT_EQ(manifest.entries.size(), 1u);
    EXPECT_TRUE(manifest.entries.front().success);

    RVX::Resource::ResourceLoadPreparationContext context;
    context.requestedPath = modelPath.string();
    context.resolvedPath = modelPath.string();
    context.assetKey = RVX::Resource::MakeAssetKey(
        context.requestedPath, RVX::Resource::ResourceType::Model);
    context.resourceIdentityPath = context.assetKey.canonicalPath;
    context.rootResourceId = RVX::Resource::GenerateResourceId(
        context.resourceIdentityPath);

    RVX::Resource::CookedModelLoader loader;
    RVX::Resource::PreparedResourceBundle bundle;
    RVX::Resource::ResourceLoadError loadError;
    ASSERT_TRUE(loader.Prepare(context, bundle, loadError))
        << loadError.message;
    const auto* model = dynamic_cast<const RVX::Resource::ModelResource*>(
        bundle.GetRoot().Get());
    ASSERT_NE(model, nullptr);
    EXPECT_GT(model->GetMeshCount(), 0u);
    EXPECT_GT(model->GetMaterialCount(), 0u);
    EXPECT_FALSE(model->HasSkeleton());
    EXPECT_FALSE(model->HasAnimationClips());
}
