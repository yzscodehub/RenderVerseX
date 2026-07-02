/**
 * @file EditorUIBackendFactory.cpp
 * @brief Factory boundary for replaceable editor UI backends.
 */

#include "Editor/UI/EditorUIBackendFactory.h"

#include "Editor/UI/NativeEditorUIBackend.h"

#include <memory>
#include <utility>

namespace RVX::Editor
{

EditorUIBackendFactoryResult CreateEditorUIBackend(
    const EditorUIBackendFactoryDesc& desc)
{
    EditorUIBackendFactoryResult result;

    switch (desc.type)
    {
    case EditorUIBackendType::Native:
    {
        auto backend = std::make_unique<NativeEditorUIBackend>();
        NativeEditorUIBackendDesc nativeDesc;
        nativeDesc.host.renderDevice = desc.renderDevice;
        nativeDesc.host.surfaceWidth = desc.surfaceWidth;
        nativeDesc.host.surfaceHeight = desc.surfaceHeight;
        nativeDesc.host.scaleFactor = desc.scaleFactor;
        nativeDesc.host.debugName = desc.debugName;

        if (!backend->Initialize(nativeDesc))
        {
            result.error = "Failed to initialize Native editor UI backend";
            return result;
        }

        result.backend = std::move(backend);
        return result;
    }
    default:
        result.error = "Unsupported editor UI backend type: ";
        result.error += ToString(desc.type);
        return result;
    }
}

} // namespace RVX::Editor
