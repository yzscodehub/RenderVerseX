#include "Runtime/RenderDiagnosticsPublisher.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace RVX
{
    namespace
    {
        std::string QuoteJsonString(const std::string& value)
        {
            std::ostringstream escaped;
            escaped << '"';
            for (const unsigned char character : value)
            {
                switch (character)
                {
                case '"':
                    escaped << "\\\"";
                    break;
                case '\\':
                    escaped << "\\\\";
                    break;
                case '\b':
                    escaped << "\\b";
                    break;
                case '\f':
                    escaped << "\\f";
                    break;
                case '\n':
                    escaped << "\\n";
                    break;
                case '\r':
                    escaped << "\\r";
                    break;
                case '\t':
                    escaped << "\\t";
                    break;
                default:
                    if (character < 0x20U)
                    {
                        escaped << "\\u00"
                                << std::hex << std::setw(2)
                                << std::setfill('0')
                                << static_cast<uint32>(character)
                                << std::dec;
                    }
                    else
                    {
                        escaped << static_cast<char>(character);
                    }
                    break;
                }
            }
            escaped << '"';
            return escaped.str();
        }
    } // namespace

    RenderDiagnosticsPublisher::RenderDiagnosticsPublisher()
        : m_snapshot(std::make_shared<const RenderDiagnosticsSnapshot>())
    {
    }

    void RenderDiagnosticsPublisher::Publish(RenderDiagnosticsSnapshot snapshot)
    {
        auto publication =
            std::make_shared<const RenderDiagnosticsSnapshot>(std::move(snapshot));
        m_snapshot.store(std::move(publication), std::memory_order_release);
    }

    std::shared_ptr<const RenderDiagnosticsSnapshot>
        RenderDiagnosticsPublisher::AcquireShared() const noexcept
    {
        return m_snapshot.load(std::memory_order_acquire);
    }

    RenderDiagnosticsSnapshot RenderDiagnosticsPublisher::GetSnapshot() const
    {
        const auto snapshot = AcquireShared();
        return snapshot != nullptr ? *snapshot : RenderDiagnosticsSnapshot{};
    }

    bool RenderDiagnosticsPublisher::SaveArtifact(
        const RenderDiagnosticsSnapshot& snapshot,
        const std::string& path) noexcept
    {
        try
        {
            std::ofstream file(path, std::ios::out | std::ios::trunc);
            if (!file)
            {
                return false;
            }

            file << "{\n"
                 << "  \"schema\": \"RenderVerseX.RenderRuntimeFatalDiagnostics\",\n"
                 << "  \"schemaVersion\": 1,\n"
                 << "  \"publicationSequence\": "
                 << snapshot.publicationSequence << ",\n"
                 << "  \"lifecycle\": "
                 << static_cast<uint32>(snapshot.lifecycle) << ",\n"
                 << "  \"executor\": "
                 << static_cast<uint32>(snapshot.executor) << ",\n"
                 << "  \"backend\": "
                 << static_cast<uint32>(snapshot.backend) << ",\n"
                 << "  \"renderThreadIdentityHash\": "
                 << snapshot.renderThreadIdentityHash << ",\n"
                 << "  \"surfaceGeneration\": "
                 << snapshot.surfaceGeneration << ",\n"
                 << "  \"lastSubmittedFrameSequence\": "
                 << snapshot.lastSubmittedFrameSequence << ",\n"
                 << "  \"failureAvailable\": "
                 << (snapshot.lastFailure.available ? "true" : "false")
                 << ",\n"
                 << "  \"runtimeCode\": "
                 << static_cast<uint32>(snapshot.lastFailure.runtime.code)
                 << ",\n"
                 << "  \"runtimeNativeError\": "
                 << snapshot.lastFailure.runtime.nativeError << ",\n"
                 << "  \"shutdownCode\": "
                 << static_cast<uint32>(snapshot.lastFailure.shutdown.code)
                 << ",\n"
                 << "  \"context\": "
                 << QuoteJsonString(snapshot.lastFailure.context) << "\n"
                 << "}\n";
            return static_cast<bool>(file);
        }
        catch (...)
        {
            return false;
        }
    }
} // namespace RVX
