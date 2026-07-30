#pragma once

/**
 * @file RHIValidationMessageSink.h
 * @brief Thread-safe normalized native-validation message collection.
 */

#include "RHI/RHIDefinitions.h"

#include <mutex>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RHI_CONFORMANCE_MAX_VALIDATION_MESSAGES = 64;
    inline constexpr uint32 RVX_RHI_CONFORMANCE_MAX_VALIDATION_TEXT_BYTES =
        2048;

    enum class RHIConformanceValidationSeverity : uint8
    {
        Info = 0,
        Warning = 1,
        Error = 2,
    };

    struct RHIConformanceValidationMessage
    {
        RHIConformanceValidationSeverity severity =
            RHIConformanceValidationSeverity::Info;
        std::string category;
        std::string nativeId;
        std::string text;
        bool textTruncated = false;
        bool allowlisted = false;
        std::string allowlistEntryId;
    };

    struct RHIConformanceValidationAllowlistEntry
    {
        std::string id;
        RHIBackendType backend = RHIBackendType::None;
        std::string nativeId;
        std::string justification;
        std::string reviewOwner;
        std::string expiresOn;
    };

    struct RHIConformanceValidationSnapshot
    {
        bool configurationValid = true;
        std::vector<std::string> configurationDiagnostics;
        std::string evaluationDate;
        std::vector<RHIConformanceValidationMessage> messages;
        uint32 infoCount = 0;
        uint32 warningCount = 0;
        uint32 errorCount = 0;
        uint32 unexpectedWarningCount = 0;
        uint32 unexpectedErrorCount = 0;
        uint32 droppedMessageCount = 0;
    };

    /**
     * @brief Collect normalized native diagnostics without suppressing them.
     *
     * Warning allowlists match only a concrete backend and exact native ID.
     * Message text is retained for evidence but never participates in matching.
     */
    class RHIConformanceValidationMessageSink final
    {
    public:
        RHIConformanceValidationMessageSink(
            RHIBackendType backend,
            std::string evaluationDate,
            std::vector<RHIConformanceValidationAllowlistEntry> allowlist = {});

        RHIConformanceValidationMessageSink(
            const RHIConformanceValidationMessageSink&) = delete;
        RHIConformanceValidationMessageSink& operator=(
            const RHIConformanceValidationMessageSink&) = delete;
        RHIConformanceValidationMessageSink(
            RHIConformanceValidationMessageSink&&) = delete;
        RHIConformanceValidationMessageSink& operator=(
            RHIConformanceValidationMessageSink&&) = delete;

        void Record(RHIConformanceValidationSeverity severity,
                    std::string category,
                    std::string nativeId,
                    std::string text);
        void Record(const RHIConformanceValidationMessage& message);

        [[nodiscard]] RHIConformanceValidationSnapshot GetSnapshot() const;

    private:
        const RHIConformanceValidationAllowlistEntry* FindAllowlistEntry(
            const RHIConformanceValidationMessage& message) const;
        void AddConfigurationDiagnostic(std::string diagnostic);

        RHIBackendType m_backend = RHIBackendType::None;
        std::string m_evaluationDate;
        std::vector<RHIConformanceValidationAllowlistEntry> m_allowlist;
        mutable std::mutex m_mutex;
        bool m_configurationValid = true;
        std::vector<std::string> m_configurationDiagnostics;
        std::vector<RHIConformanceValidationMessage> m_messages;
        uint32 m_infoCount = 0;
        uint32 m_warningCount = 0;
        uint32 m_errorCount = 0;
        uint32 m_unexpectedWarningCount = 0;
        uint32 m_unexpectedErrorCount = 0;
        uint32 m_totalMessageCount = 0;
    };

    const char* GetRHIConformanceValidationSeverityName(
        RHIConformanceValidationSeverity severity);
} // namespace RVX
