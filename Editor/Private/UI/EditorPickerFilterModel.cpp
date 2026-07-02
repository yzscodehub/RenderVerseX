/**
 * @file EditorPickerFilterModel.cpp
 * @brief Reusable native editor picker filtering and ranking model implementation
 */

#include "Editor/UI/EditorPickerFilterModel.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::vector<std::string> TokenizeFilter(const std::string& filterText)
    {
        std::vector<std::string> tokens;
        std::string current;
        for (unsigned char ch : filterText)
        {
            if (std::isspace(ch))
            {
                if (!current.empty())
                {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
                continue;
            }
            current.push_back(static_cast<char>(std::tolower(ch)));
        }

        if (!current.empty())
        {
            tokens.push_back(std::move(current));
        }
        return tokens;
    }

    int32 ScoreFuzzySubsequence(const std::string& field, const std::string& token)
    {
        size_t cursor = 0;
        size_t firstMatch = std::string::npos;
        size_t lastMatch = 0;
        for (char ch : token)
        {
            const size_t position = field.find(ch, cursor);
            if (position == std::string::npos)
            {
                return 0;
            }
            if (firstMatch == std::string::npos)
            {
                firstMatch = position;
            }
            lastMatch = position;
            cursor = position + 1u;
        }

        const size_t span = lastMatch >= firstMatch ? lastMatch - firstMatch + 1u : token.size();
        const int32 gapPenalty = static_cast<int32>(
            std::min<size_t>(span > token.size() ? span - token.size() : 0u, 100u));
        const int32 startPenalty = static_cast<int32>(std::min<size_t>(firstMatch, 100u));
        return 280 + static_cast<int32>(token.size()) * 4 - gapPenalty - startPenalty;
    }

    int32 ScoreField(const std::string& field, const std::string& token)
    {
        if (field.empty() || token.empty())
        {
            return 0;
        }

        if (field == token)
        {
            return 1000 + static_cast<int32>(token.size()) * 8;
        }
        if (field.rfind(token, 0u) == 0u)
        {
            return 800 + static_cast<int32>(token.size()) * 6;
        }

        const size_t position = field.find(token);
        if (position != std::string::npos)
        {
            const int32 positionPenalty =
                static_cast<int32>(std::min<size_t>(position, 120u));
            return 620 + static_cast<int32>(token.size()) * 5 - positionPenalty;
        }

        return ScoreFuzzySubsequence(field, token);
    }

    int32 AddWeightWhenMatched(int32 score, int32 weight)
    {
        return score > 0 ? score + weight : 0;
    }

    int32 ScoreTokenForItem(const EditorPickerFilterItemDesc& item,
                            const std::string& token)
    {
        int32 bestScore =
            AddWeightWhenMatched(ScoreField(ToLowerAscii(item.text), token), 40);
        bestScore = std::max(
            bestScore,
            AddWeightWhenMatched(ScoreField(ToLowerAscii(item.id), token), 30));
        bestScore = std::max(
            bestScore,
            AddWeightWhenMatched(ScoreField(ToLowerAscii(item.category), token), 20));
        for (const std::string& keyword : item.keywords)
        {
            bestScore = std::max(
                bestScore,
                AddWeightWhenMatched(ScoreField(ToLowerAscii(keyword), token), 25));
        }
        return bestScore;
    }

    int32 ScoreItem(const EditorPickerFilterItemDesc& item,
                    const std::vector<std::string>& tokens)
    {
        if (tokens.empty())
        {
            return 0;
        }

        int32 totalScore = 0;
        for (const std::string& token : tokens)
        {
            const int32 tokenScore = ScoreTokenForItem(item, token);
            if (tokenScore <= 0)
            {
                return 0;
            }
            totalScore += tokenScore;
        }
        return totalScore;
    }

    bool ItemSortLess(const EditorPickerFilterItemDesc& lhsItem,
                      const EditorPickerFilterItemDesc& rhsItem,
                      const EditorPickerFilterResult& lhs,
                      const EditorPickerFilterResult& rhs,
                      const EditorPickerFilterOptions& options,
                      bool filterEmpty)
    {
        if (options.groupByCategory && lhsItem.category != rhsItem.category)
        {
            return lhsItem.category < rhsItem.category;
        }

        if (!filterEmpty && options.sortMatchesByScore && lhs.score != rhs.score)
        {
            return lhs.score > rhs.score;
        }

        if (!options.groupByCategory && lhsItem.category != rhsItem.category)
        {
            return lhsItem.category < rhsItem.category;
        }
        if (lhsItem.text != rhsItem.text)
        {
            return lhsItem.text < rhsItem.text;
        }
        return lhsItem.id < rhsItem.id;
    }
}

void EditorPickerFilterModel::SetItems(std::vector<EditorPickerFilterItemDesc> items)
{
    m_items = std::move(items);
}

void EditorPickerFilterModel::SetFilterText(std::string filterText)
{
    m_filterText = std::move(filterText);
}

void EditorPickerFilterModel::Rebuild()
{
    m_results.clear();

    const std::vector<std::string> tokens = TokenizeFilter(m_filterText);
    const bool filterEmpty = tokens.empty();
    m_results.reserve(m_items.size());
    for (uint32 index = 0; index < static_cast<uint32>(m_items.size()); ++index)
    {
        const int32 score = filterEmpty ? 0 : ScoreItem(m_items[index], tokens);
        if (filterEmpty || score > 0)
        {
            m_results.push_back({index, score});
        }
    }

    std::stable_sort(
        m_results.begin(),
        m_results.end(),
        [this, filterEmpty](const EditorPickerFilterResult& lhs,
                            const EditorPickerFilterResult& rhs) {
            const EditorPickerFilterItemDesc& lhsItem = m_items[lhs.sourceIndex];
            const EditorPickerFilterItemDesc& rhsItem = m_items[rhs.sourceIndex];
            return ItemSortLess(lhsItem, rhsItem, lhs, rhs, m_options, filterEmpty);
        });

    m_lastBuildStats.sourceItemCount = static_cast<uint32>(m_items.size());
    m_lastBuildStats.resultCount = static_cast<uint32>(m_results.size());
    m_lastBuildStats.filterEmpty = filterEmpty;
}

} // namespace RVX::Editor
