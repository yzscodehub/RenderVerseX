#include "Core/Core.h"
#include "TestFramework/TestRunner.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace RVX::Test;

namespace
{
    std::string LoadSourceFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};

        std::ostringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

    bool Test_DX11ConstantBufferBindingsUseStaticOffsets()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_DX11" / "Private" / "DX11Pipeline.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("uint64 effectiveOffset = binding.offset") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("uint64 effectiveRange = binding.range") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("VSSetConstantBuffers1") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("PSSetConstantBuffers1") != std::string::npos);

        return true;
    }

    bool Test_DX11DynamicOffsetsAreAddedToStaticOffsets()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_DX11" / "Private" / "DX11Pipeline.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("effectiveOffset += dynamicOffsets[dynamicOffsetIndex]") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("dynamicOffsetIndex++") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("binding {} offset is outside the buffer") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("range exceeds the buffer and will be clamped") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("ID3D11DeviceContext1 is required for constant buffer offset binding; unbinding slot {}") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("ID3D11Buffer* nullCB = nullptr") != std::string::npos);

        return true;
    }
} // namespace

int main()
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("RHI DX11 Source Validation Tests");

    TestSuite suite;
    suite.AddTest("DX11ConstantBufferBindingsUseStaticOffsets", Test_DX11ConstantBufferBindingsUseStaticOffsets);
    suite.AddTest("DX11DynamicOffsetsAreAddedToStaticOffsets", Test_DX11DynamicOffsetsAreAddedToStaticOffsets);

    auto results = suite.Run();
    suite.PrintResults(results);

    RVX::Log::Shutdown();

    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }

    return 0;
}
