#include "Runtime/RenderFrameMailbox.h"

namespace RVX
{
    template class BasicRenderFrameMailbox<
        RenderFramePacket,
        CompleteRenderFramePacketValidator<RenderFramePacket>>;
} // namespace RVX
