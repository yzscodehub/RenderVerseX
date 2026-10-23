#pragma once

/**
 * @file ArtifactMetadata.h
 * @brief Shared metadata contract for exported diagnostics artifacts.
 */

#include "Core/Types.h"

#include <string>

namespace RVX::Diagnostics
{
    struct ArtifactMetadata
    {
        std::string id;
        std::string kind;
        std::string contentType;
        std::string schemaId;
        uint32 schemaVersion = 0;
        std::string contentHash;
        std::string relativePath;

        bool HasIdentity() const;
        bool HasSchema() const;
    };
} // namespace RVX::Diagnostics
