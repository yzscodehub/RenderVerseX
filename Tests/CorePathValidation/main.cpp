#include "Core/PathUtils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace
{
    std::filesystem::path MakeUniqueTempRoot()
    {
        const std::filesystem::path base =
            std::filesystem::temp_directory_path();
        for (int index = 0; index < 100; ++index)
        {
            std::filesystem::path candidate =
                base / ("rvx_path_validation_" + std::to_string(index));
            std::error_code ec;
            if (!std::filesystem::exists(candidate, ec))
            {
                std::filesystem::create_directories(candidate, ec);
                if (!ec)
                {
                    return candidate;
                }
            }
        }
        return {};
    }
} // namespace

TEST(CorePathValidation, ExecutablePathIsDiscoverable)
{
    const std::filesystem::path executablePath = RVX::GetExecutablePath();
    EXPECT_FALSE(executablePath.empty());
    EXPECT_TRUE(executablePath.is_absolute());
}

TEST(CorePathValidation, FindAncestorContainingWalksUpward)
{
    const std::filesystem::path root = MakeUniqueTempRoot();
    ASSERT_FALSE(root.empty());

    const std::filesystem::path markerDir = root / "marker";
    const std::filesystem::path markerFile = markerDir / "root.txt";
    const std::filesystem::path nested = root / "a" / "b" / "c";

    std::filesystem::create_directories(markerDir);
    std::filesystem::create_directories(nested);
    {
        std::ofstream marker(markerFile, std::ios::binary);
        marker << "marker";
    }

    const std::filesystem::path found =
        RVX::FindAncestorContaining(nested,
                                    std::filesystem::path("marker") /
                                        "root.txt");
    EXPECT_EQ(found, std::filesystem::weakly_canonical(root));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(CorePathValidation, ResolveWorkspaceRelativePathReturnsAbsolutePath)
{
    const std::filesystem::path resolved =
        RVX::ResolveWorkspaceRelativePath(
            std::filesystem::path("Render") / "Shaders");
    EXPECT_FALSE(resolved.empty());
    EXPECT_TRUE(resolved.is_absolute());
    EXPECT_EQ(resolved.filename(), std::filesystem::path("Shaders"));
}
