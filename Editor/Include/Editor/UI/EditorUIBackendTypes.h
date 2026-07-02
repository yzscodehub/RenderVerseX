/**
 * @file EditorUIBackendTypes.h
 * @brief Shared data types for editor UI backend boundaries.
 */

#pragma once

#include "Core/Types.h"
#include "UI/UIContext.h"

#include <string>

namespace RVX
{
    class IRHIDevice;
}

namespace RVX::Editor
{

enum class EditorUICursorMode : uint8
{
    Normal = 0,
    Hidden,
    Locked
};

struct EditorUICursorRequest
{
    bool requestsMouseCapture = false;
    bool requestsCursorHidden = false;
    bool requestsCursorLock = false;
    EditorUICursorMode cursorMode = EditorUICursorMode::Normal;
    std::string ownerPanelId;

    bool IsActive() const
    {
        return requestsMouseCapture ||
               requestsCursorHidden ||
               requestsCursorLock ||
               cursorMode != EditorUICursorMode::Normal;
    }
};

struct EditorUIBackendHostDesc
{
    IRHIDevice* renderDevice = nullptr;
    uint32 surfaceWidth = 0;
    uint32 surfaceHeight = 0;
    float scaleFactor = 1.0f;
    float inputScaleFactor = 0.0f;
    std::string debugName = "EditorUIHost";
};

using EditorUIHostDesc = EditorUIBackendHostDesc;

struct EditorUIFrameDesc
{
    uint32 surfaceWidth = 0;
    uint32 surfaceHeight = 0;
    float deltaTime = 0.0f;
    float scaleFactor = 1.0f;
    float inputScaleFactor = 0.0f;
    UI::UIInputSnapshot input;
};

struct EditorUINativeRenderStats
{
    bool frameRecorded = false;
    bool rendererInitialized = false;
    uint32 commandCount = 0;
    uint32 rectCount = 0;
    uint32 textCount = 0;
    uint32 imageCount = 0;
    uint32 vertexCount = 0;
    uint32 indexCount = 0;
    uint32 menuCount = 0;
    uint32 toolbarCount = 0;
    uint32 statusItemCount = 0;
    uint32 panelFrameCount = 0;
    uint32 popupCount = 0;
    uint32 tooltipCount = 0;
    uint32 contextMenuItemCount = 0;
    uint32 commandPaletteItemCount = 0;
    uint32 commandPaletteExecutableItemCount = 0;
    uint32 filePickerEntryCount = 0;
    uint32 filePickerFilteredEntryCount = 0;
    uint32 modalDialogButtonCount = 0;
    uint32 modalDialogEnabledButtonCount = 0;
    uint32 executableItemCount = 0;
    bool contextMenuOpen = false;
    bool commandPaletteOpen = false;
    bool filePickerOpen = false;
    bool modalDialogOpen = false;
    bool tooltipVisible = false;
    float menuBarHeight = 0.0f;
    float toolbarHeight = 0.0f;
    float statusBarHeight = 0.0f;
    float topReservedHeight = 0.0f;
    float bottomReservedHeight = 0.0f;
};

} // namespace RVX::Editor
