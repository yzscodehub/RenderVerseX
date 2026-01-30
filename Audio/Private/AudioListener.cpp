/**
 * @file AudioListener.cpp
 * @brief AudioListener implementation
 */

#include "Audio/AudioListener.h"
#include "Audio/AudioEngine.h"

namespace RVX::Audio
{

// AudioListener stores state locally.
// The actual listener update to miniaudio happens via AudioEngine::SetListenerTransform()
// which should be called by the game/engine code when the listener moves.

// Helper function to sync listener with engine
void SyncListenerWithEngine(const AudioListener& listener)
{
    if (!listener.IsEnabled())
    {
        return;
    }

    auto& engine = GetAudioEngine();
    engine.SetListenerTransform(
        listener.GetPosition(),
        listener.GetForward(),
        listener.GetUp()
    );
    engine.SetListenerVelocity(listener.GetVelocity());
}

} // namespace RVX::Audio
