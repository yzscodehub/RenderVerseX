/**
 * @file UIClipboard.h
 * @brief Platform-neutral UI clipboard service.
 */

#pragma once

#include <functional>
#include <string>

namespace RVX::UI
{

struct UIClipboardProvider
{
    std::function<bool(std::string&)> getText;
    std::function<bool(const std::string&)> setText;
};

class UIClipboard
{
public:
    static bool SetText(const std::string& text);
    static bool GetText(std::string& text);
    static std::string GetText();
    static bool HasText();

    static void SetProvider(UIClipboardProvider provider);
    static void ResetProvider();
    static bool HasExternalProvider();
};

} // namespace RVX::UI
