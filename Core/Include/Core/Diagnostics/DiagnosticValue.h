#pragma once

/**
 * @file DiagnosticValue.h
 * @brief Value-semantic telemetry availability contract shared by all modules.
 */

#include <optional>
#include <string>
#include <utility>

namespace RVX
{
    /**
     * @brief A diagnostic value whose absence is distinct from a measured zero.
     *
     * Producers must provide a reason when a metric cannot be sampled. This
     * type is intentionally independent of Scene, Resource, Render, and Sample
     * so every diagnostic snapshot uses one availability vocabulary.
     */
    template<typename T>
    class DiagnosticValue
    {
    public:
        DiagnosticValue() = default;

        [[nodiscard]] static DiagnosticValue Available(T value)
        {
            DiagnosticValue result;
            result.m_value = std::move(value);
            return result;
        }

        [[nodiscard]] static DiagnosticValue Unavailable(std::string reason)
        {
            DiagnosticValue result;
            result.m_reason = std::move(reason);
            return result;
        }

        [[nodiscard]] bool IsAvailable() const noexcept
        {
            return m_value.has_value();
        }

        [[nodiscard]] const std::optional<T>& GetValue() const noexcept
        {
            return m_value;
        }

        [[nodiscard]] const std::string& GetReason() const noexcept
        {
            return m_reason;
        }

    private:
        std::optional<T> m_value;
        std::string m_reason;
    };
} // namespace RVX
