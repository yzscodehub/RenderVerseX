#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::string ReadSource(const std::filesystem::path& relativePath)
    {
        const std::filesystem::path path =
            std::filesystem::path(RVX_SOURCE_DIR) / relativePath;
        std::ifstream stream(path, std::ios::binary);
        EXPECT_TRUE(stream.good()) << "Could not read " << path.string();
        std::ostringstream contents;
        contents << stream.rdbuf();
        return contents.str();
    }

    TEST(RenderCallerBoundaryValidation,
         ShowcaseSamplesUseOnlyProductionValueBoundaries)
    {
        constexpr std::array<const char*, 9> forbidden = {
            "GetSceneRenderer",
            "GetGPUResourceManager",
            "GetRenderContext",
            "renderSubsystem->GetDevice",
            "renderSubsystem->BeginFrame",
            "renderSubsystem->Render(",
            "renderSubsystem->EndFrame",
            "renderSubsystem->Present",
            "TickWithoutRender",
        };
        constexpr std::array<const char*, 2> samples = {
            "Samples/RenderVerseSamples/Scenes/ModelViewerSample.cpp",
            "Samples/RenderVerseSamples/Scenes/GPUDrivenShowcaseSample.cpp",
        };

        for (const char* sample : samples)
        {
            const std::string source = ReadSource(sample);
            for (const char* token : forbidden)
            {
                EXPECT_EQ(source.find(token), std::string::npos)
                    << sample << " crossed the production Render boundary with "
                    << token;
            }
        }
    }

    TEST(RenderCallerBoundaryValidation,
         EditorBuildRequiresTheM1RuntimeAdapter)
    {
        const std::string cmake = ReadSource("Editor/CMakeLists.txt");
        const std::string application =
            ReadSource("Editor/Private/EditorApplication.cpp");
        const std::string adapterHeader =
            ReadSource("Editor/Include/Editor/EditorRenderRuntimeAdapter.h");

        EXPECT_NE(cmake.find("Private/EditorRenderRuntimeAdapter.cpp"),
                  std::string::npos);
        EXPECT_NE(cmake.find("RVX_EDITOR_M1_RENDER_ADAPTER=1"),
                  std::string::npos);
        EXPECT_NE(application.find("m_renderRuntimeAdapter.Start"),
                  std::string::npos);
        EXPECT_EQ(application.find("m_renderContext->GetDevice"),
                  std::string::npos);
        EXPECT_NE(adapterHeader.find(
                      "UnavailableDuringM1ArchitectureCut"),
                  std::string::npos);
    }

    TEST(RenderCallerBoundaryValidation,
         EditorDoesNotUseRenderSubsystemLiveObjectGetters)
    {
        constexpr std::array<const char*, 4> forbidden = {
            "GetSceneRenderer",
            "GetGPUResourceManager",
            "GetRenderContext",
            "GetSwapChain",
        };
        const std::filesystem::path editorRoot =
            std::filesystem::path(RVX_SOURCE_DIR) / "Editor";

        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(editorRoot))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::filesystem::path extension = entry.path().extension();
            if (extension != ".cpp" && extension != ".h")
            {
                continue;
            }

            std::ifstream stream(entry.path(), std::ios::binary);
            ASSERT_TRUE(stream.good());
            std::ostringstream contents;
            contents << stream.rdbuf();
            const std::string source = contents.str();
            for (const char* token : forbidden)
            {
                EXPECT_EQ(source.find("renderSubsystem->" +
                                      std::string(token)),
                          std::string::npos)
                    << entry.path().string();
            }
        }
    }
} // namespace
