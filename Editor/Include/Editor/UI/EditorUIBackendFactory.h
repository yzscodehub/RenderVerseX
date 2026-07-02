/**
 * @file EditorUIBackendFactory.h
 * @brief Factory boundary for replaceable editor UI backends.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorUIBackendCatalog.h"

#include <memory>
#include <string>

namespace RVX
{
    class IRHIDevice;
}

namespace RVX::Editor
{

class IEditorUIBackend;

struct EditorUIBackendFactoryDesc
{
    EditorUIBackendType type = GetDefaultEditorUIBackendType();
    IRHIDevice* renderDevice = nullptr;
    uint32 surfaceWidth = 0;
    uint32 surfaceHeight = 0;
    float scaleFactor = 1.0f;
    std::string debugName = "RenderVerseX.EditorUI";
};

struct EditorUIBackendFactoryResult
{
    std::unique_ptr<IEditorUIBackend> backend;
    std::string error;

    explicit operator bool() const { return backend != nullptr; }
};

EditorUIBackendFactoryResult CreateEditorUIBackend(
    const EditorUIBackendFactoryDesc& desc);

} // namespace RVX::Editor
