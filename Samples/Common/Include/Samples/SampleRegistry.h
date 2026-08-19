#pragma once

/** @file SampleRegistry.h @brief Deterministic registry for sample scenes. */

#include "Samples/Sample.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RVX
{
    using SampleFactory = std::function<std::unique_ptr<ISample>()>;

    class SampleRegistry final
    {
    public:
        bool Register(SampleInfo info,
                      SampleFactory factory,
                      std::string* outError = nullptr);

        [[nodiscard]] std::unique_ptr<ISample> Create(
            std::string_view id) const;
        [[nodiscard]] const SampleInfo* Find(std::string_view id) const;
        [[nodiscard]] std::vector<SampleInfo> List() const;

    private:
        struct Entry
        {
            SampleInfo info;
            SampleFactory factory;
        };

        std::map<std::string, Entry, std::less<>> m_entries;
    };
} // namespace RVX
