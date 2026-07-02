/**
 * @file EditorPickerFilterModel.h
 * @brief Reusable native editor picker filtering and ranking model
 */

#pragma once

#include "Core/Types.h"

#include <string>
#include <vector>

namespace RVX::Editor
{

struct EditorPickerFilterItemDesc
{
    std::string id;
    std::string text;
    std::string category;
    std::vector<std::string> keywords;
    bool enabled = true;
};

struct EditorPickerFilterOptions
{
    bool groupByCategory = true;
    bool sortMatchesByScore = true;
};

struct EditorPickerFilterResult
{
    uint32 sourceIndex = 0;
    int32 score = 0;
};

struct EditorPickerFilterStats
{
    uint32 sourceItemCount = 0;
    uint32 resultCount = 0;
    bool filterEmpty = true;
};

class EditorPickerFilterModel
{
public:
    // =========================================================================
    // Data
    // =========================================================================
    void SetItems(std::vector<EditorPickerFilterItemDesc> items);
    const std::vector<EditorPickerFilterItemDesc>& GetItems() const { return m_items; }

    // =========================================================================
    // Filter
    // =========================================================================
    void SetFilterText(std::string filterText);
    const std::string& GetFilterText() const { return m_filterText; }

    void SetOptions(EditorPickerFilterOptions options) { m_options = options; }
    const EditorPickerFilterOptions& GetOptions() const { return m_options; }

    void Rebuild();
    const std::vector<EditorPickerFilterResult>& GetResults() const
    {
        return m_results;
    }

    const EditorPickerFilterStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    std::vector<EditorPickerFilterItemDesc> m_items;
    std::vector<EditorPickerFilterResult> m_results;
    std::string m_filterText;
    EditorPickerFilterOptions m_options;
    EditorPickerFilterStats m_lastBuildStats;
};

} // namespace RVX::Editor
