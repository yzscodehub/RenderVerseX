/**
 * @file EditorLayoutSerializer.h
 * @brief Persistent native editor dock layout serialization
 */

#pragma once

#include "Editor/UI/EditorDockingModel.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace RVX::Editor
{

class EditorLayoutSerializer
{
public:
    static std::string Serialize(const EditorDockingModel& model);
    static bool Deserialize(std::string_view text,
                            EditorDockingModel& model,
                            std::string* error = nullptr);

    static bool SaveToFile(const EditorDockingModel& model,
                           const std::filesystem::path& path,
                           std::string* error = nullptr);
    static bool LoadFromFile(const std::filesystem::path& path,
                             EditorDockingModel& model,
                             std::string* error = nullptr);
};

} // namespace RVX::Editor
