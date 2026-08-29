#pragma once

/** @file ShaderArtifact.h @brief Cooked shader program artifact v2 contract */

#include "ShaderContracts/ShaderProgramProvider.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr std::array<char, 8> RVX_SHADER_ARTIFACT_MAGIC_V2 = {
        'R', 'V', 'X', 'S', 'H', 'D', '2', '\0'};
    inline constexpr uint32 RVX_SHADER_ARTIFACT_VERSION = 2;
    inline constexpr uint32 RVX_SHADER_ARTIFACT_ENDIAN_MARKER =
        0x01020304u;
    // OpenGL cooked artifacts embed the translated GLSL source. The GLSL
    // binding remap changed for the integrated froxel volume, so older
    // OpenGL artifacts must be rejected and recooked.
    inline constexpr uint64 RVX_SHADER_COMPILER_ABI_VERSION = 16;

    struct ShaderArtifactSection
    {
        uint64 offset = 0;
        uint64 size = 0;
    };

#pragma pack(push, 1)
    struct ShaderArtifactHeaderV2
    {
        std::array<char, 8> magic = RVX_SHADER_ARTIFACT_MAGIC_V2;
        uint32 version = RVX_SHADER_ARTIFACT_VERSION;
        uint32 endianMarker = RVX_SHADER_ARTIFACT_ENDIAN_MARKER;
        uint32 headerSize = 0;
        uint32 backend = 0;
        uint32 stage = 0;
        uint32 flags = 0;
        uint64 totalSize = 0;
        uint64 compilerAbi = 0;
        uint64 permutationHash = 0;
        uint64 contentHash = 0;
        uint64 payloadHash = 0;
        ShaderArtifactSection identity;
        ShaderArtifactSection permutation;
        ShaderArtifactSection code;
        ShaderArtifactSection reflection;
        std::array<uint64, 8> reserved{};
    };
#pragma pack(pop)

    static_assert(sizeof(ShaderArtifactHeaderV2) == 200);

    struct ShaderArtifact
    {
        ShaderProgramRequest request;
        std::string targetProfile;
        /** @brief Backend-final entry point; differs for translated Metal code. */
        std::string runtimeEntryPoint;
        uint64 compilerAbi = RVX_SHADER_COMPILER_ABI_VERSION;
        uint64 permutationHash = 0;
        uint64 contentHash = 0;
        uint64 payloadHash = 0;
        std::vector<uint8> code;
        ShaderReflection reflection;
    };

    /** @brief Canonicalize macro order and reject duplicate names. */
    [[nodiscard]] bool CanonicalizeShaderMacros(
        std::vector<ShaderMacro>& defines,
        std::string& outError);

    /** @brief Stable hash of the complete runtime program request. */
    [[nodiscard]] uint64 ComputeShaderPermutationHash(
        const ShaderProgramRequest& request);

    /** @brief Content-addressed cooked artifact path for one request. */
    [[nodiscard]] std::filesystem::path BuildShaderArtifactRelativePath(
        const ShaderProgramRequest& request);

    /** @brief Strict v2 writer. No v1 compatibility output is supported. */
    [[nodiscard]] bool WriteShaderArtifactV2(
        const std::filesystem::path& path,
        ShaderArtifact artifact,
        std::string& outError);

    /** @brief Strict v2 reader. No v1 compatibility input is accepted. */
    [[nodiscard]] bool ReadShaderArtifactV2(
        const std::filesystem::path& path,
        ShaderArtifact& outArtifact,
        std::string& outError);
} // namespace RVX
