/**
 * @file UIClipboard.cpp
 * @brief Platform-neutral UI clipboard service implementation.
 */

#include "UI/UIClipboard.h"

#include <utility>

namespace RVX::UI
{
namespace
{
    std::string s_memoryClipboard;
    UIClipboardProvider s_provider;
    bool s_hasExternalProvider = false;
}

bool UIClipboard::SetText(const std::string& text)
{
    s_memoryClipboard = text;
    if (s_hasExternalProvider && s_provider.setText)
    {
        return s_provider.setText(text);
    }
    return true;
}

bool UIClipboard::GetText(std::string& text)
{
    if (s_hasExternalProvider && s_provider.getText)
    {
        std::string externalText;
        if (s_provider.getText(externalText))
        {
            s_memoryClipboard = externalText;
            text = std::move(externalText);
            return true;
        }
    }

    text = s_memoryClipboard;
    return !text.empty();
}

std::string UIClipboard::GetText()
{
    std::string text;
    GetText(text);
    return text;
}

bool UIClipboard::HasText()
{
    std::string text;
    return GetText(text) && !text.empty();
}

void UIClipboard::SetProvider(UIClipboardProvider provider)
{
    s_provider = std::move(provider);
    s_hasExternalProvider =
        static_cast<bool>(s_provider.getText) ||
        static_cast<bool>(s_provider.setText);
}

void UIClipboard::ResetProvider()
{
    s_provider = {};
    s_hasExternalProvider = false;
}

bool UIClipboard::HasExternalProvider()
{
    return s_hasExternalProvider;
}

} // namespace RVX::UI
