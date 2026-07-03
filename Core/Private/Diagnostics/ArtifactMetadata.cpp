/**
 * @file ArtifactMetadata.cpp
 * @brief Shared metadata contract for exported diagnostics artifacts.
 */

#include "Core/Diagnostics/ArtifactMetadata.h"

namespace RVX::Diagnostics
{
    bool ArtifactMetadata::HasIdentity() const
    {
        return !id.empty() || !kind.empty() || !contentType.empty();
    }

    bool ArtifactMetadata::HasSchema() const
    {
        return !schemaId.empty() || schemaVersion != 0;
    }
} // namespace RVX::Diagnostics
