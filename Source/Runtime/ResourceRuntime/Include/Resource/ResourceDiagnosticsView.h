#pragma once

/**
 * @file ResourceDiagnosticsView.h
 * @brief Read-only, owner-thread diagnostics view for Resource consumers.
 */

#include "Core/Diagnostics/DiagnosticValue.h"
#include "Resource/ResourceDiagnostics.h"

namespace RVX::Resource
{
    /** @brief Result code for a Resource diagnostics snapshot request. */
    enum class ResourceDiagnosticsQueryCode : uint8
    {
        Available = 0,
        NotInitialized,
        WrongThread
    };

    /**
     * @brief Value-semantic result of a Resource diagnostics request.
     *
     * A missing snapshot is deliberately distinct from an all-zero snapshot.
     * Callers must preserve the code and unavailable reason in diagnostics
     * rather than interpreting unavailable metrics as measured zeroes.
     */
    struct ResourceDiagnosticsQueryResult
    {
        ResourceDiagnosticsQueryCode code =
            ResourceDiagnosticsQueryCode::NotInitialized;
        DiagnosticValue<ResourceDiagnosticsSnapshot> snapshot =
            DiagnosticValue<ResourceDiagnosticsSnapshot>::Unavailable(
                "ResourceSubsystem diagnostics are not initialized.");

        [[nodiscard]] bool IsAvailable() const noexcept
        {
            return code == ResourceDiagnosticsQueryCode::Available &&
                   snapshot.IsAvailable();
        }
    };

    /**
     * @brief Narrow, read-only diagnostics contract exposed to sample code.
     *
     * Resource diagnostics aggregate update-owned ResourceManager state and
     * ResourceSubsystem handoff queues. Consequently, callers must use this
     * view from the ResourceSubsystem update-owner thread. Calls from another
     * thread return WrongThread before inspecting any mutable Resource state.
     */
    class IResourceDiagnosticsView
    {
    public:
        virtual ~IResourceDiagnosticsView() = default;

        [[nodiscard]] virtual ResourceDiagnosticsQueryResult
            QueryResourceDiagnostics() const = 0;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::IResourceDiagnosticsView;
    using Resource::ResourceDiagnosticsQueryCode;
    using Resource::ResourceDiagnosticsQueryResult;
} // namespace RVX
