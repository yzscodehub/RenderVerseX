#pragma once

#include "Core/Types.h"

namespace RVX
{
    struct InputState;

    class InputBackend
    {
    public:
        virtual ~InputBackend() = default;
        virtual void Poll(InputState& state) = 0;
    };
} // namespace RVX
