/**
 * @file UI.h
 * @brief UI system unified header
 * 
 * Provides immediate and retained mode UI rendering.
 */

#pragma once

#include "UI/UITypes.h"
#include "UI/UICanvas.h"
#include "UI/Widget.h"
#include "UI/Widgets/Panel.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Image.h"
#include "UI/Layout/Layout.h"

namespace RVX::UI
{

/**
 * @brief UI system version info
 */
struct UISystemInfo
{
    static constexpr int kMajorVersion = 1;
    static constexpr int kMinorVersion = 0;
};

} // namespace RVX::UI
