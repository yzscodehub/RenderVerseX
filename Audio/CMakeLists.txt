# =============================================================================
# Audio Module - Audio playback with miniaudio backend
# =============================================================================
add_library(RVX_Audio STATIC)

# Find miniaudio header (header-only library from vcpkg)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
if(NOT MINIAUDIO_INCLUDE_DIR)
    message(FATAL_ERROR "miniaudio.h not found. Please install via vcpkg: vcpkg install miniaudio")
endif()

target_sources(RVX_Audio PRIVATE
    Private/AudioEngine.cpp
    Private/AudioClip.cpp
    Private/AudioSource.cpp
    Private/AudioListener.cpp
    Private/AudioSubsystem.cpp
    Private/Mixer/VoicePool.cpp
    Private/Mixer/AudioBus.cpp
    Private/Mixer/AudioMixer.cpp
    Private/DSP/ReverbEffect.cpp
    Private/DSP/LowPassEffect.cpp
    Private/DSP/DelayEffect.cpp
    Private/DSP/CompressorEffect.cpp
    Private/DSP/EffectChain.cpp
    Private/Events/AudioEvent.cpp
    Private/Events/AudioEventManager.cpp
    Private/Music/MusicTrack.cpp
    Private/Music/BeatSynchronizer.cpp
    Private/Music/MusicPlayer.cpp
    Private/Streaming/AudioStreamer.cpp
    Private/Spatial/AudioZone.cpp
    Private/Spatial/AudioZoneManager.cpp
    Private/Spatial/RaycastOcclusion.cpp
)

target_include_directories(RVX_Audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Audio PUBLIC
    RVX::Core
)

# miniaudio include path (header-only)
target_include_directories(RVX_Audio PRIVATE
    ${MINIAUDIO_INCLUDE_DIR}
)

# Platform-specific audio libraries
if(WIN32)
    # Windows uses COM for audio (linked automatically by miniaudio)
elseif(APPLE)
    # macOS/iOS uses CoreAudio
    find_library(COREAUDIO_FRAMEWORK CoreAudio)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
    if(COREAUDIO_FRAMEWORK AND AUDIOTOOLBOX_FRAMEWORK)
        target_link_libraries(RVX_Audio PRIVATE
            ${COREAUDIO_FRAMEWORK}
            ${AUDIOTOOLBOX_FRAMEWORK}
        )
    endif()
elseif(UNIX)
    # Linux may need pthread and dl
    find_package(Threads REQUIRED)
    target_link_libraries(RVX_Audio PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
endif()

add_library(RVX::Audio ALIAS RVX_Audio)
