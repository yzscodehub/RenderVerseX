/**
 * @file PortablePath.cpp
 * @brief Portable path formatting helpers for diagnostics artifacts.
 */

#include "Core/Diagnostics/PortablePath.h"

namespace RVX::Diagnostics
{
    std::string ToPortablePath(const std::filesystem::path& path)
    {
        return path.generic_string();
    }

    std::string GetPortableFilename(const std::filesystem::path& path)
    {
        return path.filename().generic_string();
    }
} // namespace RVX::Diagnostics
