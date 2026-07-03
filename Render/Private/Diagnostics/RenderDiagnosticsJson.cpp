/**
 * @file RenderDiagnosticsJson.cpp
 * @brief Lightweight JSON helper implementation for render diagnostics artifacts.
 */

#include "RenderDiagnosticsJson.h"

#include <iomanip>
#include <sstream>

namespace RVX::RenderDiagnostics
{
    const char* JsonBool(bool value)
    {
        return value ? "true" : "false";
    }

    std::string JsonString(std::string_view value)
    {
        std::ostringstream ss;
        ss << '"';
        for (unsigned char ch : value)
        {
            switch (ch)
            {
                case '\\':
                    ss << "\\\\";
                    break;
                case '"':
                    ss << "\\\"";
                    break;
                case '\n':
                    ss << "\\n";
                    break;
                case '\r':
                    ss << "\\r";
                    break;
                case '\t':
                    ss << "\\t";
                    break;
                default:
                    if (ch < 0x20)
                    {
                        ss << "\\u"
                           << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                           << static_cast<uint32>(ch)
                           << std::dec << std::nouppercase << std::setfill(' ');
                    }
                    else
                    {
                        ss << static_cast<char>(ch);
                    }
                    break;
            }
        }
        ss << '"';
        return ss.str();
    }

    std::string JsonOptionalIndex(uint32 value)
    {
        if (value == RVX_INVALID_INDEX)
        {
            return "null";
        }

        return std::to_string(value);
    }
} // namespace RVX::RenderDiagnostics
