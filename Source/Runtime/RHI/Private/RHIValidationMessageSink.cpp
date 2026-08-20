/**
 * @file RHIValidationMessageSink.cpp
 * @brief Thread-safe normalized native-validation message collection.
 */

#include "RHI/RHIValidationMessageSink.h"

#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr bool IsConcreteBackend(RHIBackendType backend)
        {
            switch (backend)
            {
                case RHIBackendType::DX11:
                case RHIBackendType::DX12:
                case RHIBackendType::Vulkan:
                case RHIBackendType::Metal:
                case RHIBackendType::OpenGL:
                    return true;
                case RHIBackendType::None:
                case RHIBackendType::Auto:
                default:
                    return false;
            }
        }

        constexpr bool IsLeapYear(uint32 year)
        {
            return (year % 4 == 0 && year % 100 != 0) ||
                   year % 400 == 0;
        }

        bool ParseIsoDate(const std::string& value)
        {
            if (value.size() != 10 || value[4] != '-' || value[7] != '-')
            {
                return false;
            }
            for (size_t i = 0; i < value.size(); ++i)
            {
                if (i == 4 || i == 7)
                {
                    continue;
                }
                if (value[i] < '0' || value[i] > '9')
                {
                    return false;
                }
            }

            const uint32 year =
                static_cast<uint32>(std::stoul(value.substr(0, 4)));
            const uint32 month =
                static_cast<uint32>(std::stoul(value.substr(5, 2)));
            const uint32 day =
                static_cast<uint32>(std::stoul(value.substr(8, 2)));
            if (year == 0 || month == 0 || month > 12 || day == 0)
            {
                return false;
            }

            constexpr std::array<uint32, 12> kDaysPerMonth = {
                31, 28, 31, 30, 31, 30,
                31, 31, 30, 31, 30, 31,
            };
            uint32 daysInMonth = kDaysPerMonth[month - 1];
            if (month == 2 && IsLeapYear(year))
            {
                ++daysInMonth;
            }
            return day <= daysInMonth;
        }

        bool ValidationMessageLess(
            const RHIConformanceValidationMessage& left,
            const RHIConformanceValidationMessage& right)
        {
            return std::tie(left.severity,
                            left.category,
                            left.nativeId,
                            left.text,
                            left.textTruncated,
                            left.allowlisted,
                            left.allowlistEntryId) <
                   std::tie(right.severity,
                            right.category,
                            right.nativeId,
                            right.text,
                            right.textTruncated,
                            right.allowlisted,
                            right.allowlistEntryId);
        }
    } // namespace

    RHIConformanceValidationMessageSink::
        RHIConformanceValidationMessageSink(
            RHIBackendType backend,
            std::string evaluationDate,
            std::vector<RHIConformanceValidationAllowlistEntry> allowlist)
        : m_backend(backend),
          m_evaluationDate(std::move(evaluationDate)),
          m_allowlist(std::move(allowlist))
    {
        if (!IsConcreteBackend(m_backend))
        {
            AddConfigurationDiagnostic(
                "Validation message sinks require a concrete backend.");
        }
        if (!ParseIsoDate(m_evaluationDate))
        {
            AddConfigurationDiagnostic(
                "Validation allowlist evaluation date must use YYYY-MM-DD.");
        }

        std::set<std::string> ids;
        std::set<std::pair<uint8, std::string>> nativeKeys;
        for (const RHIConformanceValidationAllowlistEntry& entry :
             m_allowlist)
        {
            if (entry.id.empty())
            {
                AddConfigurationDiagnostic(
                    "Validation allowlist entries require a stable ID.");
            }
            else if (!ids.emplace(entry.id).second)
            {
                AddConfigurationDiagnostic(
                    "Validation allowlist entry IDs must be unique.");
            }
            if (!IsConcreteBackend(entry.backend))
            {
                AddConfigurationDiagnostic(
                    "Validation allowlist entries require a concrete backend.");
            }
            if (entry.nativeId.empty())
            {
                AddConfigurationDiagnostic(
                    "Validation allowlist entries require an exact native ID.");
            }
            else
            {
                const auto nativeKey = std::make_pair(
                    static_cast<uint8>(entry.backend),
                    entry.nativeId);
                if (!nativeKeys.emplace(nativeKey).second)
                {
                    AddConfigurationDiagnostic(
                        "Validation allowlist backend/native-ID pairs must be unique.");
                }
            }
            if (entry.justification.empty())
            {
                AddConfigurationDiagnostic(
                    "Validation allowlist entries require a justification.");
            }
            if (entry.reviewOwner.empty())
            {
                AddConfigurationDiagnostic(
                    "Validation allowlist entries require a review owner.");
            }
            if (!ParseIsoDate(entry.expiresOn))
            {
                AddConfigurationDiagnostic(
                    "Validation allowlist expiry must use YYYY-MM-DD.");
            }
        }
    }

    void RHIConformanceValidationMessageSink::Record(
        RHIConformanceValidationSeverity severity,
        std::string category,
        std::string nativeId,
        std::string text)
    {
        RHIConformanceValidationMessage message;
        message.severity = severity;
        message.category = std::move(category);
        message.nativeId = std::move(nativeId);
        message.text = std::move(text);
        Record(message);
    }

    void RHIConformanceValidationMessageSink::Record(
        const RHIConformanceValidationMessage& message)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        RHIConformanceValidationMessage normalized = message;
        normalized.allowlisted = false;
        normalized.allowlistEntryId.clear();
        normalized.textTruncated = false;
        if (normalized.category.empty())
        {
            AddConfigurationDiagnostic(
                "Validation messages require a normalized category.");
        }
        if (normalized.severity !=
                RHIConformanceValidationSeverity::Info &&
            normalized.nativeId.empty())
        {
            AddConfigurationDiagnostic(
                "Validation warnings/errors require an exact native ID.");
        }
        if (normalized.text.empty())
        {
            AddConfigurationDiagnostic(
                "Validation messages require bounded diagnostic text.");
        }
        if (normalized.text.size() >
            RVX_RHI_CONFORMANCE_MAX_VALIDATION_TEXT_BYTES)
        {
            normalized.text.resize(
                RVX_RHI_CONFORMANCE_MAX_VALIDATION_TEXT_BYTES);
            normalized.textTruncated = true;
        }

        switch (normalized.severity)
        {
            case RHIConformanceValidationSeverity::Info:
                ++m_infoCount;
                break;
            case RHIConformanceValidationSeverity::Warning:
            {
                ++m_warningCount;
                const RHIConformanceValidationAllowlistEntry* entry =
                    FindAllowlistEntry(normalized);
                if (entry)
                {
                    normalized.allowlisted = true;
                    normalized.allowlistEntryId = entry->id;
                }
                else
                {
                    ++m_unexpectedWarningCount;
                }
                break;
            }
            case RHIConformanceValidationSeverity::Error:
                ++m_errorCount;
                ++m_unexpectedErrorCount;
                break;
            default:
                AddConfigurationDiagnostic(
                    "Validation messages require a known severity.");
                break;
        }
        ++m_totalMessageCount;

        const auto insertionPoint = std::lower_bound(
            m_messages.begin(),
            m_messages.end(),
            normalized,
            ValidationMessageLess);
        m_messages.insert(insertionPoint, std::move(normalized));
        if (m_messages.size() >
            RVX_RHI_CONFORMANCE_MAX_VALIDATION_MESSAGES)
        {
            m_messages.pop_back();
        }
    }

    RHIConformanceValidationSnapshot
    RHIConformanceValidationMessageSink::GetSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        RHIConformanceValidationSnapshot snapshot;
        snapshot.configurationValid = m_configurationValid;
        snapshot.configurationDiagnostics = m_configurationDiagnostics;
        std::sort(snapshot.configurationDiagnostics.begin(),
                  snapshot.configurationDiagnostics.end());
        snapshot.evaluationDate = m_evaluationDate;
        snapshot.messages = m_messages;
        snapshot.infoCount = m_infoCount;
        snapshot.warningCount = m_warningCount;
        snapshot.errorCount = m_errorCount;
        snapshot.unexpectedWarningCount = m_unexpectedWarningCount;
        snapshot.unexpectedErrorCount = m_unexpectedErrorCount;
        snapshot.droppedMessageCount =
            m_totalMessageCount -
            static_cast<uint32>(m_messages.size());
        return snapshot;
    }

    const RHIConformanceValidationAllowlistEntry*
    RHIConformanceValidationMessageSink::FindAllowlistEntry(
        const RHIConformanceValidationMessage& message) const
    {
        if (!m_configurationValid ||
            message.severity != RHIConformanceValidationSeverity::Warning)
        {
            return nullptr;
        }

        const auto it = std::find_if(
            m_allowlist.begin(),
            m_allowlist.end(),
            [this, &message](
                const RHIConformanceValidationAllowlistEntry& entry)
            {
                return entry.backend == m_backend &&
                       entry.nativeId == message.nativeId &&
                       entry.expiresOn >= m_evaluationDate;
            });
        return it != m_allowlist.end() ? &(*it) : nullptr;
    }

    void RHIConformanceValidationMessageSink::
        AddConfigurationDiagnostic(std::string diagnostic)
    {
        m_configurationValid = false;
        m_configurationDiagnostics.push_back(std::move(diagnostic));
    }

    const char* GetRHIConformanceValidationSeverityName(
        RHIConformanceValidationSeverity severity)
    {
        switch (severity)
        {
            case RHIConformanceValidationSeverity::Info:
                return "Info";
            case RHIConformanceValidationSeverity::Warning:
                return "Warning";
            case RHIConformanceValidationSeverity::Error:
                return "Error";
            default:
                return "Unknown";
        }
    }
} // namespace RVX
