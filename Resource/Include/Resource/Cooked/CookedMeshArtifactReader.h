#pragma once

/**
 * @file CookedMeshArtifactReader.h
 * @brief Shared reader for standalone and model-owned cooked mesh artifacts.
 */

#include "Core/Types.h"
#include "Geometry/Asset/Mesh.h"

#include <string>
#include <vector>

namespace RVX::Resource
{
    struct CookedMeshRecord
    {
        std::vector<Mesh::Ptr> lodMeshes;
    };

    class CookedMeshArtifactReader
    {
    public:
        static bool ReadFile(const std::string& path,
                             std::vector<CookedMeshRecord>& outMeshes,
                             std::string& outError);

        static bool ReadBytes(const std::vector<uint8>& bytes,
                              std::vector<CookedMeshRecord>& outMeshes,
                              std::string& outError);
    };
} // namespace RVX::Resource
