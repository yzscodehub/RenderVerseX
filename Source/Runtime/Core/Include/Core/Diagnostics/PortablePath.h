#pragma once

/**
 * @file PortablePath.h
 * @brief Portable path formatting helpers for diagnostics artifacts.
 */

#include <filesystem>
#include <string>

namespace RVX::Diagnostics
{
    std::string ToPortablePath(const std::filesystem::path& path);
    std::string GetPortableFilename(const std::filesystem::path& path);
} // namespace RVX::Diagnostics
