/**
 * @file AudioEngine.cpp
 * @brief AudioEngine implementation with miniaudio backend
 */

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "Audio/AudioEngine.h"
#include "Audio/AudioClip.h"
#include "Audio/AudioSource.h"
#include "Audio/DSP/IAudioEffect.h"
#include "Audio/DSP/ReverbEffect.h"
#include "Audio/Mixer/AudioBus.h"
#include "Core/Log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace RVX::Audio
{

// =============================================================================
// Internal Data Structures
// =============================================================================

/**
 * @brief Internal sound instance tracking
 */
struct SoundInstance
{
    ma_sound sound;
    uint64 handleId = 0;
    uint32 busId = 0;
    bool is3D = false;
    bool isActive = true;
    bool isPlaying = false;
    bool backendSoundInitialized = false;
    Vec3 position{0.0f};
    Vec3 velocity{0.0f};
    float volume = 1.0f;
    float lowPassCutoff = 20000.0f;
    ma_lpf_node lowPassNode{};
    bool lowPassNodeInitialized = false;
    ma_node* outputTargetNode = nullptr;
    uint32 outputTargetInputBusIndex = 0;
    AudioClip::Ptr clip;
};

struct ReverbNode
{
    ma_node_base baseNode;
    ReverbEffect effect;
    ma_uint32 channels = 2;
};

struct EffectChainNode
{
    ma_node_base baseNode;
    std::vector<std::shared_ptr<IAudioEffect>> effects;
    ma_uint32 channels = 2;
};

struct BusSendNode
{
    static constexpr size_t kMaxSends = 4;

    ma_node_base baseNode;
    std::array<float, kMaxSends> amounts{};
    std::array<ma_uint32, kMaxSends + 1> outputChannels{};
    ma_uint32 inputChannels = 2;
    size_t sendCount = 0;
};

struct BusEffectState
{
    float lowPassCutoff = 20000.0f;
    ReverbSettings reverbSettings;
    bool reverbEnabled = false;
    std::vector<std::shared_ptr<IAudioEffect>> genericEffects;
    std::vector<AudioBusSend> sends;
    ma_lpf_node lowPassNode{};
    bool lowPassNodeInitialized = false;
    ReverbNode reverbNode;
    bool reverbNodeInitialized = false;
    EffectChainNode genericNode;
    bool genericNodeInitialized = false;
    BusSendNode sendNode;
    bool sendNodeInitialized = false;
    uint32 droppedSendCount = 0;
    ma_node* outputTargetNode = nullptr;
    uint32 outputTargetInputBusIndex = 0;
};

/**
 * @brief Miniaudio backend implementation data
 */
struct AudioEngineBackend
{
    ma_engine engine;
    ma_resource_manager resourceManager;
    bool engineInitialized = false;
    bool resourceManagerInitialized = false;

    std::unordered_map<uint64, std::unique_ptr<SoundInstance>> sounds;
    std::mutex soundsMutex;

    // Bus groups (implemented as ma_sound_group)
    std::unordered_map<uint32, ma_sound_group> busGroups;
    std::unordered_map<uint32, BusEffectState> busEffects;

    // Files registered with the resource manager stay decoded between plays.
    std::unordered_set<std::string> registeredClipFiles;
};

namespace
{
    constexpr uint32 RVX_FIRST_CUSTOM_AUDIO_BUS_ID = BusId::UI + 1;
    constexpr float RVX_AUDIO_BYPASS_LOWPASS_CUTOFF = 19999.0f;
    constexpr ma_uint32 RVX_AUDIO_LOWPASS_ORDER = 2;

    void ReverbNodeProcess(
        ma_node* node,
        const float** framesIn,
        ma_uint32* frameCountIn,
        float** framesOut,
        ma_uint32* frameCountOut)
    {
        (void)frameCountIn;

        auto* reverbNode = static_cast<ReverbNode*>(node);
        const ma_uint32 frameCount = *frameCountOut;
        if (frameCount == 0)
        {
            return;
        }

        if (framesOut[0] != framesIn[0])
        {
            std::memcpy(
                framesOut[0],
                framesIn[0],
                static_cast<size_t>(frameCount) * reverbNode->channels * sizeof(float));
        }

        reverbNode->effect.Process(framesOut[0], frameCount, reverbNode->channels);
    }

    ma_node_vtable g_reverbNodeVTable =
    {
        ReverbNodeProcess,
        nullptr,
        1,
        1,
        0
    };

    void EffectChainNodeProcess(
        ma_node* node,
        const float** framesIn,
        ma_uint32* frameCountIn,
        float** framesOut,
        ma_uint32* frameCountOut)
    {
        (void)frameCountIn;

        auto* effectNode = static_cast<EffectChainNode*>(node);
        const ma_uint32 frameCount = *frameCountOut;
        if (frameCount == 0)
        {
            return;
        }

        if (framesOut[0] != framesIn[0])
        {
            std::memcpy(
                framesOut[0],
                framesIn[0],
                static_cast<size_t>(frameCount) * effectNode->channels * sizeof(float));
        }

        for (const std::shared_ptr<IAudioEffect>& effect : effectNode->effects)
        {
            if (effect && effect->IsEnabled())
            {
                effect->Process(framesOut[0], frameCount, effectNode->channels);
            }
        }
    }

    ma_node_vtable g_effectChainNodeVTable =
    {
        EffectChainNodeProcess,
        nullptr,
        1,
        1,
        0
    };

    void CopyScaledFrames(float* output, const float* input, ma_uint32 frameCount, ma_uint32 channels, float gain)
    {
        if (!output)
        {
            return;
        }

        const size_t sampleCount = static_cast<size_t>(frameCount) * channels;
        if (!input || gain == 0.0f)
        {
            std::memset(output, 0, sampleCount * sizeof(float));
            return;
        }

        if (gain == 1.0f && output != input)
        {
            std::memcpy(output, input, sampleCount * sizeof(float));
            return;
        }

        if (output == input && gain == 1.0f)
        {
            return;
        }

        for (size_t i = 0; i < sampleCount; ++i)
        {
            output[i] = input[i] * gain;
        }
    }

    void BusSendNodeProcess(
        ma_node* node,
        const float** framesIn,
        ma_uint32* frameCountIn,
        float** framesOut,
        ma_uint32* frameCountOut)
    {
        (void)frameCountIn;

        auto* sendNode = static_cast<BusSendNode*>(node);
        const ma_uint32 frameCount = *frameCountOut;
        if (frameCount == 0)
        {
            return;
        }

        const float* input = framesIn ? framesIn[0] : nullptr;
        CopyScaledFrames(framesOut[0], input, frameCount, sendNode->inputChannels, 1.0f);

        for (size_t i = 0; i < BusSendNode::kMaxSends; ++i)
        {
            const float amount = i < sendNode->sendCount ? sendNode->amounts[i] : 0.0f;
            CopyScaledFrames(framesOut[i + 1u], input, frameCount, sendNode->inputChannels, amount);
        }
    }

    ma_node_vtable g_busSendNodeVTable =
    {
        BusSendNodeProcess,
        nullptr,
        1,
        static_cast<ma_uint8>(BusSendNode::kMaxSends + 1u),
        0
    };

    bool TryGetReservedBusId(const std::string& name, uint32& outBusId)
    {
        if (name == "Master")
        {
            outBusId = BusId::Master;
            return true;
        }
        if (name == "Music")
        {
            outBusId = BusId::Music;
            return true;
        }
        if (name == "SFX")
        {
            outBusId = BusId::SFX;
            return true;
        }
        if (name == "Voice")
        {
            outBusId = BusId::Voice;
            return true;
        }
        if (name == "Ambient")
        {
            outBusId = BusId::Ambient;
            return true;
        }
        if (name == "UI")
        {
            outBusId = BusId::UI;
            return true;
        }

        return false;
    }

    uint32 AllocateCustomBusId(const std::vector<AudioBus>& buses)
    {
        uint32 nextBusId = RVX_FIRST_CUSTOM_AUDIO_BUS_ID;
        for (const AudioBus& bus : buses)
        {
            if (bus.id >= nextBusId)
            {
                nextBusId = bus.id + 1;
            }
        }

        return nextBusId;
    }

    ma_uint32 GetSoundFlags(const AudioClip& clip)
    {
        return clip.IsStreaming() ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
    }

    ma_uint32 GetResourceManagerFlags(const AudioClip& clip)
    {
        return clip.IsStreaming() ? MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM
                                  : MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE;
    }

    ma_sound_group* GetBusGroup(AudioEngineBackend* backend, uint32 busId)
    {
        if (!backend || busId == 0)
        {
            return nullptr;
        }

        auto it = backend->busGroups.find(busId);
        return it != backend->busGroups.end() ? &it->second : nullptr;
    }

    ma_node* GetSoundOutputTarget(AudioEngineBackend* backend, uint32 busId)
    {
        if (!backend || !backend->engineInitialized)
        {
            return nullptr;
        }

        if (ma_sound_group* group = GetBusGroup(backend, busId))
        {
            return group;
        }

        return ma_engine_get_endpoint(&backend->engine);
    }

    bool GetBusEffectOutputTarget(AudioEngineBackend* backend,
                                  const std::vector<AudioBus>& buses,
                                  uint32 busId,
                                  ma_node*& outNode,
                                  uint32& outInputBusIndex)
    {
        if (!backend || !backend->engineInitialized)
        {
            return false;
        }

        auto busIt = std::find_if(buses.begin(), buses.end(),
            [busId](const AudioBus& bus) { return bus.id == busId; });
        if (busIt == buses.end())
        {
            return false;
        }

        outNode = ma_engine_get_endpoint(&backend->engine);
        outInputBusIndex = 0;
        if (busIt->parentBus != BusId::Master)
        {
            if (auto parentIt = backend->busGroups.find(busIt->parentBus); parentIt != backend->busGroups.end())
            {
                outNode = &parentIt->second;
            }
        }

        return true;
    }

    bool HasRuntimeSendPath(AudioEngineBackend* backend,
                            uint32 currentBusId,
                            uint32 targetBusId,
                            uint32 ignoredSourceBusId,
                            std::vector<uint32>& visited)
    {
        if (currentBusId == targetBusId)
        {
            return true;
        }
        if (currentBusId == ignoredSourceBusId)
        {
            return false;
        }
        if (std::find(visited.begin(), visited.end(), currentBusId) != visited.end())
        {
            return false;
        }

        visited.push_back(currentBusId);

        auto effectIt = backend->busEffects.find(currentBusId);
        if (effectIt == backend->busEffects.end())
        {
            return false;
        }

        for (const AudioBusSend& send : effectIt->second.sends)
        {
            if (HasRuntimeSendPath(backend, send.targetBusId, targetBusId, ignoredSourceBusId, visited))
            {
                return true;
            }
        }

        return false;
    }

    bool WouldCreateRuntimeSendCycle(AudioEngineBackend* backend, uint32 sourceBusId, uint32 targetBusId)
    {
        if (!backend || sourceBusId == targetBusId)
        {
            return true;
        }

        std::vector<uint32> visited;
        return HasRuntimeSendPath(backend, targetBusId, sourceBusId, sourceBusId, visited);
    }

    uint32 ResolveSoundBus(AudioEngineBackend* backend, uint32 requestedBusId)
    {
        if (requestedBusId == 0)
        {
            return 0;
        }

        return GetBusGroup(backend, requestedBusId) ? requestedBusId : 0;
    }

    void RegisterClipWithResourceManager(AudioEngineBackend* backend,
                                         const AudioClip& clip,
                                         bool registerBackendFile)
    {
        if (!backend || !backend->resourceManagerInitialized || clip.GetPath().empty() || clip.IsStreaming())
        {
            return;
        }

        const std::string& path = clip.GetPath();
        if (backend->registeredClipFiles.find(path) != backend->registeredClipFiles.end())
        {
            return;
        }

        if (!registerBackendFile)
        {
            backend->registeredClipFiles.insert(path);
            return;
        }

        const ma_result result = ma_resource_manager_register_file(
            &backend->resourceManager,
            path.c_str(),
            GetResourceManagerFlags(clip));
        if (result == MA_SUCCESS)
        {
            backend->registeredClipFiles.insert(path);
        }
        else
        {
            RVX_CORE_WARN("Failed to register audio clip '{}' with resource manager: {}",
                path, static_cast<int>(result));
        }
    }

    float NormalizeLowPassCutoff(float cutoffFrequency)
    {
        if (!std::isfinite(cutoffFrequency))
        {
            return 20000.0f;
        }

        return std::clamp(cutoffFrequency, 20.0f, 20000.0f);
    }

    ma_uint32 GetBackendChannelCount(AudioEngineBackend* backend, const AudioEngineConfig& config)
    {
        if (backend && backend->engineInitialized)
        {
            const ma_uint32 channels = ma_engine_get_channels(&backend->engine);
            if (channels > 0)
            {
                return channels;
            }
        }

        return config.channels > 0 ? config.channels : 2;
    }

    ma_uint32 GetBackendSampleRate(AudioEngineBackend* backend, const AudioEngineConfig& config)
    {
        if (backend && backend->engineInitialized)
        {
            const ma_uint32 sampleRate = ma_engine_get_sample_rate(&backend->engine);
            if (sampleRate > 0)
            {
                return sampleRate;
            }
        }

        return config.sampleRate > 0 ? config.sampleRate : 48000;
    }

    void DestroyLowPassNode(SoundInstance& instance)
    {
        if (instance.lowPassNodeInitialized)
        {
            ma_lpf_node_uninit(&instance.lowPassNode, nullptr);
            instance.lowPassNodeInitialized = false;
        }
    }

    bool AttachSoundToStoredTarget(SoundInstance& instance)
    {
        if (!instance.outputTargetNode)
        {
            return false;
        }

        return ma_node_attach_output_bus(
            &instance.sound,
            0,
            instance.outputTargetNode,
            instance.outputTargetInputBusIndex) == MA_SUCCESS;
    }

    bool EnsureLowPassNode(AudioEngineBackend* backend,
                           const AudioEngineConfig& config,
                           SoundInstance& instance,
                           float cutoffFrequency)
    {
        if (!backend || !backend->engineInitialized || !instance.backendSoundInitialized)
        {
            return false;
        }

        const ma_uint32 channels = GetBackendChannelCount(backend, config);
        const ma_uint32 sampleRate = GetBackendSampleRate(backend, config);

        if (!instance.outputTargetNode)
        {
            instance.outputTargetNode = GetSoundOutputTarget(backend, instance.busId);
            instance.outputTargetInputBusIndex = 0;
        }

        if (!instance.outputTargetNode)
        {
            return false;
        }

        if (!instance.lowPassNodeInitialized)
        {
            ma_lpf_node_config nodeConfig = ma_lpf_node_config_init(
                channels,
                sampleRate,
                cutoffFrequency,
                RVX_AUDIO_LOWPASS_ORDER);

            ma_result result = ma_lpf_node_init(
                ma_engine_get_node_graph(&backend->engine),
                &nodeConfig,
                nullptr,
                &instance.lowPassNode);
            if (result != MA_SUCCESS)
            {
                RVX_CORE_WARN("Failed to initialize audio low-pass node: {}", static_cast<int>(result));
                return false;
            }

            instance.lowPassNodeInitialized = true;

            result = ma_node_attach_output_bus(
                &instance.lowPassNode,
                0,
                instance.outputTargetNode,
                instance.outputTargetInputBusIndex);
            if (result != MA_SUCCESS)
            {
                RVX_CORE_WARN("Failed to attach audio low-pass node: {}", static_cast<int>(result));
                DestroyLowPassNode(instance);
                return false;
            }

            result = ma_node_attach_output_bus(&instance.sound, 0, &instance.lowPassNode, 0);
            if (result != MA_SUCCESS)
            {
                RVX_CORE_WARN("Failed to route sound through low-pass node: {}", static_cast<int>(result));
                DestroyLowPassNode(instance);
                return false;
            }
        }

        ma_lpf_config filterConfig = ma_lpf_config_init(
            ma_format_f32,
            channels,
            sampleRate,
            cutoffFrequency,
            RVX_AUDIO_LOWPASS_ORDER);
        const ma_result result = ma_lpf_node_reinit(&filterConfig, &instance.lowPassNode);
        if (result != MA_SUCCESS)
        {
            RVX_CORE_WARN("Failed to update audio low-pass cutoff: {}", static_cast<int>(result));
            return false;
        }

        return true;
    }

    void ApplyLowPassCutoff(AudioEngineBackend* backend,
                            const AudioEngineConfig& config,
                            SoundInstance& instance)
    {
        if (!backend || !backend->engineInitialized || !instance.backendSoundInitialized)
        {
            return;
        }

        if (instance.lowPassCutoff >= RVX_AUDIO_BYPASS_LOWPASS_CUTOFF)
        {
            if (instance.lowPassNodeInitialized)
            {
                if (AttachSoundToStoredTarget(instance))
                {
                    DestroyLowPassNode(instance);
                }
            }
            return;
        }

        EnsureLowPassNode(backend, config, instance, instance.lowPassCutoff);
    }

    void DestroyBusLowPassNode(BusEffectState& effect)
    {
        if (effect.lowPassNodeInitialized)
        {
            ma_lpf_node_uninit(&effect.lowPassNode, nullptr);
            effect.lowPassNodeInitialized = false;
        }
    }

    void DestroyBusReverbNode(BusEffectState& effect)
    {
        if (effect.reverbNodeInitialized)
        {
            ma_node_uninit(&effect.reverbNode, nullptr);
            effect.reverbNodeInitialized = false;
        }
    }

    void DestroyBusGenericEffectNode(BusEffectState& effect)
    {
        if (effect.genericNodeInitialized)
        {
            ma_node_uninit(&effect.genericNode, nullptr);
            effect.genericNodeInitialized = false;
        }
        effect.genericNode.effects.clear();
    }

    void DestroyBusSendNode(BusEffectState& effect)
    {
        if (effect.sendNodeInitialized)
        {
            ma_node_uninit(&effect.sendNode, nullptr);
            effect.sendNodeInitialized = false;
        }
        effect.sendNode.sendCount = 0;
        effect.sendNode.amounts.fill(0.0f);
    }

    void ApplyReverbSettings(ReverbNode& node, const ReverbSettings& settings)
    {
        node.effect.SetParameter("roomSize", settings.roomSize);
        node.effect.SetParameter("damping", settings.damping);
        node.effect.SetParameter("wetLevel", settings.wetLevel);
        node.effect.SetParameter("dryLevel", settings.dryLevel);
        node.effect.SetParameter("width", settings.width);
        node.effect.SetEnabled(true);
    }

    bool EnsureBusLowPassNode(AudioEngineBackend* backend,
                              const AudioEngineConfig& config,
                              BusEffectState& effect)
    {
        if (!backend || !backend->engineInitialized)
        {
            return false;
        }

        const ma_uint32 channels = GetBackendChannelCount(backend, config);
        const ma_uint32 sampleRate = GetBackendSampleRate(backend, config);

        if (!effect.lowPassNodeInitialized)
        {
            ma_lpf_node_config nodeConfig = ma_lpf_node_config_init(
                channels,
                sampleRate,
                effect.lowPassCutoff,
                RVX_AUDIO_LOWPASS_ORDER);

            ma_result result = ma_lpf_node_init(
                ma_engine_get_node_graph(&backend->engine),
                &nodeConfig,
                nullptr,
                &effect.lowPassNode);
            if (result != MA_SUCCESS)
            {
                RVX_CORE_WARN("Failed to initialize audio bus low-pass node: {}", static_cast<int>(result));
                return false;
            }

            effect.lowPassNodeInitialized = true;
        }

        ma_lpf_config filterConfig = ma_lpf_config_init(
            ma_format_f32,
            channels,
            sampleRate,
            effect.lowPassCutoff,
            RVX_AUDIO_LOWPASS_ORDER);
        const ma_result result = ma_lpf_node_reinit(&filterConfig, &effect.lowPassNode);
        if (result != MA_SUCCESS)
        {
            RVX_CORE_WARN("Failed to update audio bus low-pass cutoff: {}", static_cast<int>(result));
            return false;
        }

        return true;
    }

    bool EnsureBusReverbNode(AudioEngineBackend* backend,
                             const AudioEngineConfig& config,
                             BusEffectState& effect)
    {
        if (!backend || !backend->engineInitialized)
        {
            return false;
        }

        const ma_uint32 channels = GetBackendChannelCount(backend, config);
        if (channels < 2)
        {
            return false;
        }

        if (!effect.reverbNodeInitialized)
        {
            effect.reverbNode.channels = channels;
            ApplyReverbSettings(effect.reverbNode, effect.reverbSettings);

            ma_node_config nodeConfig = ma_node_config_init();
            nodeConfig.vtable = &g_reverbNodeVTable;
            nodeConfig.pInputChannels = &effect.reverbNode.channels;
            nodeConfig.pOutputChannels = &effect.reverbNode.channels;

            const ma_result result = ma_node_init(
                ma_engine_get_node_graph(&backend->engine),
                &nodeConfig,
                nullptr,
                &effect.reverbNode);
            if (result != MA_SUCCESS)
            {
                RVX_CORE_WARN("Failed to initialize audio bus reverb node: {}", static_cast<int>(result));
                return false;
            }

            effect.reverbNodeInitialized = true;
        }

        effect.reverbNode.channels = channels;
        ApplyReverbSettings(effect.reverbNode, effect.reverbSettings);
        return true;
    }

    bool EnsureBusGenericEffectNode(AudioEngineBackend* backend,
                                    const AudioEngineConfig& config,
                                    BusEffectState& effect)
    {
        if (!backend || !backend->engineInitialized || effect.genericEffects.empty())
        {
            return false;
        }

        const ma_uint32 channels = GetBackendChannelCount(backend, config);
        if (channels == 0)
        {
            return false;
        }

        if (!effect.genericNodeInitialized)
        {
            effect.genericNode.channels = channels;

            ma_node_config nodeConfig = ma_node_config_init();
            nodeConfig.vtable = &g_effectChainNodeVTable;
            nodeConfig.pInputChannels = &effect.genericNode.channels;
            nodeConfig.pOutputChannels = &effect.genericNode.channels;

            const ma_result result = ma_node_init(
                ma_engine_get_node_graph(&backend->engine),
                &nodeConfig,
                nullptr,
                &effect.genericNode);
            if (result != MA_SUCCESS)
            {
                RVX_CORE_WARN("Failed to initialize audio bus generic effect node: {}", static_cast<int>(result));
                return false;
            }

            effect.genericNodeInitialized = true;
        }

        effect.genericNode.channels = channels;
        effect.genericNode.effects = effect.genericEffects;
        return true;
    }

    bool EnsureBusSendNode(AudioEngineBackend* backend,
                           const AudioEngineConfig& config,
                           BusEffectState& effect)
    {
        if (!backend || !backend->engineInitialized || effect.sends.empty())
        {
            return false;
        }

        const ma_uint32 channels = GetBackendChannelCount(backend, config);
        if (channels == 0)
        {
            return false;
        }

        effect.sendNode.inputChannels = channels;
        effect.sendNode.outputChannels.fill(channels);

        if (!effect.sendNodeInitialized)
        {
            ma_node_config nodeConfig = ma_node_config_init();
            nodeConfig.vtable = &g_busSendNodeVTable;
            nodeConfig.pInputChannels = &effect.sendNode.inputChannels;
            nodeConfig.pOutputChannels = effect.sendNode.outputChannels.data();

            const ma_result result = ma_node_init(
                ma_engine_get_node_graph(&backend->engine),
                &nodeConfig,
                nullptr,
                &effect.sendNode);
            if (result != MA_SUCCESS)
            {
                RVX_CORE_WARN("Failed to initialize audio bus send node: {}", static_cast<int>(result));
                return false;
            }

            effect.sendNodeInitialized = true;
        }

        effect.sendNode.amounts.fill(0.0f);
        effect.sendNode.sendCount = std::min(effect.sends.size(), BusSendNode::kMaxSends);
        for (size_t i = 0; i < effect.sendNode.sendCount; ++i)
        {
            effect.sendNode.amounts[i] = effect.sends[i].amount;
        }

        return true;
    }

    void RebuildBusEffectChain(AudioEngineBackend* backend,
                               const AudioEngineConfig& config,
                               ma_sound_group& group,
                               uint32 sourceBusId,
                               BusEffectState& effect,
                               ma_node* outputTargetNode,
                               uint32 outputTargetInputBusIndex)
    {
        if (!backend || !backend->engineInitialized || !outputTargetNode)
        {
            return;
        }

        effect.outputTargetNode = outputTargetNode;
        effect.outputTargetInputBusIndex = outputTargetInputBusIndex;

        const bool lowPassActive = effect.lowPassCutoff < RVX_AUDIO_BYPASS_LOWPASS_CUTOFF;
        const bool reverbActive = effect.reverbEnabled && effect.reverbSettings.wetLevel > 0.0f;
        const bool genericActive = !effect.genericEffects.empty();
        const bool sendActive = !effect.sends.empty();

        const bool lowPassReady = lowPassActive && EnsureBusLowPassNode(backend, config, effect);
        const bool reverbReady = reverbActive && EnsureBusReverbNode(backend, config, effect);
        const bool genericReady = genericActive && EnsureBusGenericEffectNode(backend, config, effect);
        const bool sendReady = sendActive && EnsureBusSendNode(backend, config, effect);

        ma_node* nextNode = outputTargetNode;
        uint32 nextInputBus = outputTargetInputBusIndex;
        if (sendReady)
        {
            ma_node_attach_output_bus(&effect.sendNode, 0, outputTargetNode, outputTargetInputBusIndex);

            size_t outputBusIndex = 1;
            for (const AudioBusSend& send : effect.sends)
            {
                if (outputBusIndex > BusSendNode::kMaxSends)
                {
                    break;
                }
                if (send.targetBusId == sourceBusId)
                {
                    continue;
                }

                ma_node* sendTargetNode = ma_engine_get_endpoint(&backend->engine);
                if (send.targetBusId != BusId::Master)
                {
                    auto targetIt = backend->busGroups.find(send.targetBusId);
                    if (targetIt == backend->busGroups.end())
                    {
                        continue;
                    }
                    sendTargetNode = &targetIt->second;
                }

                ma_node_attach_output_bus(&effect.sendNode, static_cast<ma_uint32>(outputBusIndex), sendTargetNode, 0);
                ++outputBusIndex;
            }

            nextNode = &effect.sendNode;
            nextInputBus = 0;
        }

        if (reverbReady)
        {
            ma_node_attach_output_bus(&effect.reverbNode, 0, nextNode, nextInputBus);
            nextNode = &effect.reverbNode;
            nextInputBus = 0;
        }

        if (lowPassReady)
        {
            ma_node_attach_output_bus(&effect.lowPassNode, 0, nextNode, nextInputBus);
            nextNode = &effect.lowPassNode;
            nextInputBus = 0;
        }

        if (genericReady)
        {
            ma_node_attach_output_bus(&effect.genericNode, 0, nextNode, nextInputBus);
            nextNode = &effect.genericNode;
            nextInputBus = 0;
        }

        ma_node_attach_output_bus(&group, 0, nextNode, nextInputBus);

        if (!lowPassReady)
        {
            DestroyBusLowPassNode(effect);
        }
        if (!reverbReady)
        {
            DestroyBusReverbNode(effect);
        }
        if (!genericReady)
        {
            DestroyBusGenericEffectNode(effect);
        }
        if (!sendReady)
        {
            DestroyBusSendNode(effect);
        }
    }
} // namespace
// =============================================================================
// AudioEngine Implementation
// =============================================================================

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    Shutdown();
}

bool AudioEngine::Initialize(const AudioEngineConfig& config)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("AudioEngine already initialized");
        return true;
    }

    m_config = config;

    // Create backend data
    auto backend = std::make_unique<AudioEngineBackend>();

    // Configure and initialize the shared resource manager before the engine so
    // sound instances use the same decoded data cache.
    ma_resource_manager_config resourceConfig = ma_resource_manager_config_init();
    resourceConfig.decodedFormat = ma_format_f32;
    resourceConfig.decodedChannels = 0;
    resourceConfig.decodedSampleRate = config.sampleRate;
    resourceConfig.jobThreadCount = std::min<ma_uint32>(
        config.resourceManagerJobThreadCount,
        MA_RESOURCE_MANAGER_MAX_JOB_THREAD_COUNT);
    if (!config.enableDevice || config.resourceManagerJobThreadCount == 0)
    {
        resourceConfig.jobThreadCount = 0;
        resourceConfig.flags |= MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
    }

    ma_result result = ma_resource_manager_init(&resourceConfig, &backend->resourceManager);
    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to initialize miniaudio resource manager: {}", static_cast<int>(result));
        return false;
    }
    backend->resourceManagerInitialized = true;

    // Configure and initialize miniaudio engine
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.pResourceManager = &backend->resourceManager;
    engineConfig.channels = config.channels;
    engineConfig.sampleRate = config.sampleRate;
    engineConfig.periodSizeInFrames = config.bufferSizeFrames;
    engineConfig.listenerCount = 1;
    engineConfig.noDevice = config.enableDevice ? MA_FALSE : MA_TRUE;

    result = ma_engine_init(&engineConfig, &backend->engine);
    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to initialize miniaudio engine: {}", static_cast<int>(result));
        ma_resource_manager_uninit(&backend->resourceManager);
        backend->resourceManagerInitialized = false;
        return false;
    }
    backend->engineInitialized = true;

    // Set initial listener position
    ma_engine_listener_set_position(&backend->engine, 0,
        m_listenerPosition.x, m_listenerPosition.y, m_listenerPosition.z);
    ma_engine_listener_set_direction(&backend->engine, 0,
        m_listenerForward.x, m_listenerForward.y, m_listenerForward.z);
    ma_engine_listener_set_world_up(&backend->engine, 0,
        m_listenerUp.x, m_listenerUp.y, m_listenerUp.z);

    m_backendData = backend.release();
    m_initialized = true;

    if (config.enableDevice)
    {
        RVX_CORE_INFO("AudioEngine initialized (sample rate: {}, channels: {})",
            config.sampleRate, config.channels);
    }
    else
    {
        RVX_CORE_INFO("AudioEngine initialized with no hardware device (sample rate: {}, channels: {})",
            config.sampleRate, config.channels);
    }

    return true;
}

void AudioEngine::Shutdown()
{
    if (!m_initialized) return;

    StopAll();

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (backend)
    {
        // Clean up all sounds
        {
            std::lock_guard<std::mutex> lock(backend->soundsMutex);
            for (auto& pair : backend->sounds)
            {
                if (pair.second->backendSoundInitialized)
                {
                    if (pair.second->lowPassNodeInitialized)
                    {
                        AttachSoundToStoredTarget(*pair.second);
                        DestroyLowPassNode(*pair.second);
                    }
                    ma_sound_uninit(&pair.second->sound);
                }
            }
            backend->sounds.clear();
        }
        // Clean up bus groups
        for (auto& pair : backend->busEffects)
        {
            DestroyBusLowPassNode(pair.second);
            DestroyBusReverbNode(pair.second);
            DestroyBusGenericEffectNode(pair.second);
            DestroyBusSendNode(pair.second);
        }
        backend->busEffects.clear();

        for (auto& pair : backend->busGroups)
        {
            ma_sound_group_uninit(&pair.second);
        }
        backend->busGroups.clear();

        // Uninitialize engine
        if (backend->engineInitialized)
        {
            ma_engine_uninit(&backend->engine);
        }

        if (backend->resourceManagerInitialized)
        {
            if (m_config.enableDevice)
            {
                for (const std::string& path : backend->registeredClipFiles)
                {
                    ma_resource_manager_unregister_file(&backend->resourceManager, path.c_str());
                }
            }
            backend->registeredClipFiles.clear();
            ma_resource_manager_uninit(&backend->resourceManager);
            backend->resourceManagerInitialized = false;
        }

        delete backend;
        m_backendData = nullptr;
    }

    m_sources.clear();
    m_buses.clear();
    m_initialized = false;

    RVX_CORE_INFO("AudioEngine shutdown");
}

void AudioEngine::Update(float deltaTime)
{
    (void)deltaTime;
    if (!m_initialized) return;
    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;
    if (backend->engineInitialized)
    {
        // Update listener (in case it changed)
        ma_engine_listener_set_position(&backend->engine, 0,
            m_listenerPosition.x, m_listenerPosition.y, m_listenerPosition.z);
        ma_engine_listener_set_direction(&backend->engine, 0,
            m_listenerForward.x, m_listenerForward.y, m_listenerForward.z);
        ma_engine_listener_set_world_up(&backend->engine, 0,
            m_listenerUp.x, m_listenerUp.y, m_listenerUp.z);
        ma_engine_listener_set_velocity(&backend->engine, 0,
            m_listenerVelocity.x, m_listenerVelocity.y, m_listenerVelocity.z);
    }

    // Clean up finished sounds
    {
        std::lock_guard<std::mutex> lock(backend->soundsMutex);
        for (auto it = backend->sounds.begin(); it != backend->sounds.end();)
        {
            if (!it->second->backendSoundInitialized)
            {
                if (!it->second->isActive)
                {
                    it = backend->sounds.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            else if (!ma_sound_is_playing(&it->second->sound) && !it->second->isActive)
            {
                if (it->second->lowPassNodeInitialized)
                {
                    AttachSoundToStoredTarget(*it->second);
                    DestroyLowPassNode(*it->second);
                }
                ma_sound_uninit(&it->second->sound);
                it = backend->sounds.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

AudioClip::Ptr AudioEngine::LoadClip(const std::string& path)
{
    auto clip = AudioClip::Create();
    if (clip->LoadFromFile(path))
    {
        if (m_initialized)
        {
            auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
            RegisterClipWithResourceManager(backend, *clip, m_config.enableDevice);
        }
        return clip;
    }
    return nullptr;
}

uint64 AudioEngine::ReadMixedFrames(float* output, uint64 frameCount)
{
    if (!output || frameCount == 0 || !m_initialized)
    {
        return 0;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend || !backend->engineInitialized)
    {
        return 0;
    }

    ma_uint64 framesRead = 0;
    const ma_result result = ma_engine_read_pcm_frames(&backend->engine, output, frameCount, &framesRead);
    return result == MA_SUCCESS ? static_cast<uint64>(framesRead) : 0;
}

void AudioEngine::UnloadClip(AudioClip::Ptr clip)
{
    if (clip)
    {
        clip->Unload();
    }
}

AudioHandle AudioEngine::Play(AudioClip::Ptr clip, const AudioPlaySettings& settings)
{
    if (!clip || !clip->IsLoaded() || !m_initialized)
    {
        return AudioHandle();
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return AudioHandle();

    RegisterClipWithResourceManager(backend, *clip, m_config.enableDevice);

    uint64 id = m_nextHandleId++;
    auto instance = std::make_unique<SoundInstance>();
    instance->handleId = id;
    instance->busId = ResolveSoundBus(backend, settings.busId);
    instance->is3D = false;
    instance->volume = settings.volume;
    instance->clip = clip;
    if (settings.busId != 0 && instance->busId == 0)
    {
        RVX_CORE_WARN("Audio bus {} was requested but does not exist; routing sound to master", settings.busId);
    }

    // Initialize sound from file
    ma_sound_group* group = GetBusGroup(backend, instance->busId);
    instance->outputTargetNode = group ? static_cast<ma_node*>(group) : ma_engine_get_endpoint(&backend->engine);
    instance->outputTargetInputBusIndex = 0;
    ma_result result = ma_sound_init_from_file(&backend->engine,
        clip->GetPath().c_str(),
        GetSoundFlags(*clip),
        group, nullptr,
        &instance->sound);

    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to create sound from clip '{}': {}",
            clip->GetPath(), static_cast<int>(result));
        return AudioHandle();
    }
    instance->backendSoundInitialized = true;

    // Apply settings
    ma_sound_set_volume(&instance->sound, settings.volume * (m_muted ? 0.0f : m_masterVolume));
    ma_sound_set_pitch(&instance->sound, settings.pitch);
    ma_sound_set_pan(&instance->sound, settings.pan);
    ma_sound_set_looping(&instance->sound, settings.loop);

    // Disable spatialization for 2D sounds
    ma_sound_set_spatialization_enabled(&instance->sound, MA_FALSE);

    // Handle fade in
    if (settings.fadeInTime > 0.0f)
    {
        ma_sound_set_fade_in_milliseconds(&instance->sound, 0.0f, settings.volume,
            static_cast<ma_uint64>(settings.fadeInTime * 1000.0f));
    }

    // Start playback (unless starting paused)
    if (!settings.startPaused)
    {
        ma_sound_start(&instance->sound);
    }
    instance->isPlaying = !settings.startPaused;

    {
        std::lock_guard<std::mutex> lock(backend->soundsMutex);
        backend->sounds[id] = std::move(instance);
    }

    return AudioHandle(id);
}

AudioHandle AudioEngine::Play3D(AudioClip::Ptr clip, const Audio3DSettings& settings3D,
                                const AudioPlaySettings& playSettings)
{
    if (!clip || !clip->IsLoaded() || !m_initialized)
    {
        return AudioHandle();
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return AudioHandle();

    RegisterClipWithResourceManager(backend, *clip, m_config.enableDevice);

    uint64 id = m_nextHandleId++;
    auto instance = std::make_unique<SoundInstance>();
    instance->handleId = id;
    instance->busId = ResolveSoundBus(backend, playSettings.busId);
    instance->is3D = true;
    instance->position = settings3D.position;
    instance->velocity = settings3D.velocity;
    instance->volume = playSettings.volume;
    instance->clip = clip;
    if (playSettings.busId != 0 && instance->busId == 0)
    {
        RVX_CORE_WARN("Audio bus {} was requested but does not exist; routing 3D sound to master",
            playSettings.busId);
    }

    // Initialize sound from file
    ma_sound_group* group = GetBusGroup(backend, instance->busId);
    instance->outputTargetNode = group ? static_cast<ma_node*>(group) : ma_engine_get_endpoint(&backend->engine);
    instance->outputTargetInputBusIndex = 0;
    ma_result result = ma_sound_init_from_file(&backend->engine,
        clip->GetPath().c_str(),
        GetSoundFlags(*clip),
        group, nullptr,
        &instance->sound);

    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to create 3D sound from clip '{}': {}",
            clip->GetPath(), static_cast<int>(result));
        return AudioHandle();
    }
    instance->backendSoundInitialized = true;

    // Apply playback settings
    ma_sound_set_volume(&instance->sound, playSettings.volume * (m_muted ? 0.0f : m_masterVolume));
    ma_sound_set_pitch(&instance->sound, playSettings.pitch);
    ma_sound_set_looping(&instance->sound, playSettings.loop);

    // Enable and configure 3D spatialization
    ma_sound_set_spatialization_enabled(&instance->sound, m_config.enableSpatialization ? MA_TRUE : MA_FALSE);
    ma_sound_set_position(&instance->sound,
        settings3D.position.x, settings3D.position.y, settings3D.position.z);
    ma_sound_set_velocity(&instance->sound,
        settings3D.velocity.x, settings3D.velocity.y, settings3D.velocity.z);
    ma_sound_set_direction(&instance->sound,
        settings3D.direction.x, settings3D.direction.y, settings3D.direction.z);

    // Attenuation settings
    ma_sound_set_min_distance(&instance->sound, settings3D.minDistance);
    ma_sound_set_max_distance(&instance->sound, settings3D.maxDistance);
    ma_sound_set_rolloff(&instance->sound, settings3D.rolloffFactor);

    // Attenuation model
    ma_attenuation_model attModel = ma_attenuation_model_inverse;
    switch (settings3D.attenuationModel)
    {
        case AttenuationModel::None:
            attModel = ma_attenuation_model_none;
            break;
        case AttenuationModel::Linear:
            attModel = ma_attenuation_model_linear;
            break;
        case AttenuationModel::Inverse:
            attModel = ma_attenuation_model_inverse;
            break;
        case AttenuationModel::ExponentialDistance:
            attModel = ma_attenuation_model_exponential;
            break;
    }
    ma_sound_set_attenuation_model(&instance->sound, attModel);

    // Cone settings
    ma_sound_set_cone(&instance->sound,
        settings3D.coneInnerAngle * (3.14159265f / 180.0f),
        settings3D.coneOuterAngle * (3.14159265f / 180.0f),
        settings3D.coneOuterGain);

    // Handle fade in
    if (playSettings.fadeInTime > 0.0f)
    {
        ma_sound_set_fade_in_milliseconds(&instance->sound, 0.0f, playSettings.volume,
            static_cast<ma_uint64>(playSettings.fadeInTime * 1000.0f));
    }

    // Start playback
    if (!playSettings.startPaused)
    {
        ma_sound_start(&instance->sound);
    }
    instance->isPlaying = !playSettings.startPaused;

    {
        std::lock_guard<std::mutex> lock(backend->soundsMutex);
        backend->sounds[id] = std::move(instance);
    }

    return AudioHandle(id);
}

void AudioEngine::Stop(AudioHandle handle, float fadeOutTime)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        if (!it->second->backendSoundInitialized)
        {
            it->second->isPlaying = false;
        }
        else if (fadeOutTime > 0.0f)
        {
            float currentVolume = ma_sound_get_volume(&it->second->sound);
            ma_sound_set_fade_in_milliseconds(&it->second->sound,
                currentVolume, 0.0f,
                static_cast<ma_uint64>(fadeOutTime * 1000.0f));
            ma_sound_set_stop_time_in_milliseconds(&it->second->sound,
                static_cast<ma_uint64>(fadeOutTime * 1000.0f));
        }
        else
        {
            ma_sound_stop(&it->second->sound);
        }
        it->second->isPlaying = false;
        it->second->isActive = false;
    }
}

void AudioEngine::StopAll(float fadeOutTime)
{
    if (!m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    for (auto& pair : backend->sounds)
    {
        if (!pair.second->backendSoundInitialized)
        {
            pair.second->isPlaying = false;
        }
        else if (fadeOutTime > 0.0f)
        {
            float currentVolume = ma_sound_get_volume(&pair.second->sound);
            ma_sound_set_fade_in_milliseconds(&pair.second->sound,
                currentVolume, 0.0f,
                static_cast<ma_uint64>(fadeOutTime * 1000.0f));
            ma_sound_set_stop_time_in_milliseconds(&pair.second->sound,
                static_cast<ma_uint64>(fadeOutTime * 1000.0f));
        }
        else
        {
            ma_sound_stop(&pair.second->sound);
        }
        pair.second->isPlaying = false;
        pair.second->isActive = false;
    }
}

void AudioEngine::Pause(AudioHandle handle)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        if (it->second->backendSoundInitialized)
        {
            ma_sound_stop(&it->second->sound);
        }
        it->second->isPlaying = false;
    }
}

void AudioEngine::Resume(AudioHandle handle)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        if (it->second->backendSoundInitialized)
        {
            ma_sound_start(&it->second->sound);
        }
        it->second->isPlaying = it->second->isActive;
    }
}

bool AudioEngine::IsPlaying(AudioHandle handle) const
{
    if (!handle.IsValid() || !m_initialized) return false;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return false;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        if (!it->second->backendSoundInitialized)
        {
            return it->second->isPlaying;
        }
        return ma_sound_is_playing(&it->second->sound) != 0;
    }
    return false;
}

float AudioEngine::GetPlaybackPosition(AudioHandle handle) const
{
    if (!handle.IsValid() || !m_initialized) return 0.0f;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return 0.0f;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end() && it->second->backendSoundInitialized)
    {
        float cursor = 0.0f;
        ma_sound_get_cursor_in_seconds(&it->second->sound, &cursor);
        return cursor;
    }
    return 0.0f;
}

void AudioEngine::SetPlaybackPosition(AudioHandle handle, float position)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end() && it->second->backendSoundInitialized)
    {
        ma_uint64 frameIndex = static_cast<ma_uint64>(position * m_config.sampleRate);
        ma_sound_seek_to_pcm_frame(&it->second->sound, frameIndex);
    }
}

void AudioEngine::SetVolume(AudioHandle handle, float volume)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        it->second->volume = volume;
        if (it->second->backendSoundInitialized)
        {
            ma_sound_set_volume(&it->second->sound, volume * (m_muted ? 0.0f : m_masterVolume));
        }
    }
}

void AudioEngine::SetPitch(AudioHandle handle, float pitch)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end() && it->second->backendSoundInitialized)
    {
        ma_sound_set_pitch(&it->second->sound, pitch);
    }
}

void AudioEngine::SetPan(AudioHandle handle, float pan)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end() && it->second->backendSoundInitialized)
    {
        ma_sound_set_pan(&it->second->sound, pan);
    }
}

void AudioEngine::SetLooping(AudioHandle handle, bool loop)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end() && it->second->backendSoundInitialized)
    {
        ma_sound_set_looping(&it->second->sound, loop);
    }
}

void AudioEngine::SetPosition(AudioHandle handle, const Vec3& position)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end() && it->second->is3D)
    {
        it->second->position = position;
        if (it->second->backendSoundInitialized)
        {
            ma_sound_set_position(&it->second->sound, position.x, position.y, position.z);
        }
    }
}

void AudioEngine::SetVelocity(AudioHandle handle, const Vec3& velocity)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;
    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end() && it->second->is3D)
    {
        it->second->velocity = velocity;
        if (it->second->backendSoundInitialized)
        {
            ma_sound_set_velocity(&it->second->sound, velocity.x, velocity.y, velocity.z);
        }
    }
}

void AudioEngine::SetLowPassCutoff(AudioHandle handle, float cutoffFrequency)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    const float clampedCutoff = NormalizeLowPassCutoff(cutoffFrequency);

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        it->second->lowPassCutoff = clampedCutoff;
        ApplyLowPassCutoff(backend, m_config, *it->second);
    }
}

void AudioEngine::SetListenerTransform(const Vec3& position, const Vec3& forward, const Vec3& up)
{
    m_listenerPosition = position;
    m_listenerForward = forward;
    m_listenerUp = up;

    if (!m_initialized) return;
    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend || !backend->engineInitialized) return;

    ma_engine_listener_set_position(&backend->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&backend->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&backend->engine, 0, up.x, up.y, up.z);
}

void AudioEngine::SetListenerVelocity(const Vec3& velocity)
{
    m_listenerVelocity = velocity;

    if (!m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend || !backend->engineInitialized) return;

    ma_engine_listener_set_velocity(&backend->engine, 0, velocity.x, velocity.y, velocity.z);
}

uint32 AudioEngine::CreateBus(const std::string& name, uint32 parentBus)
{
    for (const AudioBus& existingBus : m_buses)
    {
        if (existingBus.name == name)
        {
            return existingBus.id;
        }
    }

    AudioBus bus;
    if (!TryGetReservedBusId(name, bus.id))
    {
        bus.id = AllocateCustomBusId(m_buses);
    }
    bus.name = name;
    bus.parentBus = parentBus;
    m_buses.push_back(bus);

    // Create miniaudio sound group for this bus
    if (m_initialized && bus.id != BusId::Master)
    {
        auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
        if (backend && backend->engineInitialized)
        {
            ma_sound_group* parentGroup = nullptr;
            if (parentBus > 0)
            {
                auto it = backend->busGroups.find(parentBus);
                if (it != backend->busGroups.end())
                {
                    parentGroup = &it->second;
                }
            }

            auto [it, inserted] = backend->busGroups.try_emplace(bus.id);
            if (!inserted || ma_sound_group_init(&backend->engine, 0, parentGroup, &it->second) != MA_SUCCESS)
            {
                backend->busGroups.erase(it);
            }
            else
            {
                BusEffectState& effect = backend->busEffects[bus.id];
                effect.outputTargetNode = parentGroup ? static_cast<ma_node*>(parentGroup) : ma_engine_get_endpoint(&backend->engine);
                effect.outputTargetInputBusIndex = 0;
            }
        }
    }

    return bus.id;
}

void AudioEngine::SetBusVolume(uint32 busId, float volume)
{
    for (auto& bus : m_buses)
    {
        if (bus.id == busId)
        {
            bus.volume = volume;

            if (m_initialized)
            {
                auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
                if (backend && backend->engineInitialized)
                {
                    auto it = backend->busGroups.find(busId);
                    if (it != backend->busGroups.end())
                    {
                        ma_sound_group_set_volume(&it->second, volume);
                    }
                }
            }
            break;
        }
    }
}

void AudioEngine::SetBusMuted(uint32 busId, bool muted)
{
    for (auto& bus : m_buses)
    {
        if (bus.id == busId)
        {
            bus.muted = muted;

            if (m_initialized)
            {
                auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
                if (backend && backend->engineInitialized)
                {
                    auto it = backend->busGroups.find(busId);
                    if (it != backend->busGroups.end())
                    {
                        ma_sound_group_set_volume(&it->second, muted ? 0.0f : bus.volume);
                    }
                }
            }
            break;
        }
    }
}

void AudioEngine::SetBusLowPassCutoff(uint32 busId, float cutoffFrequency)
{
    if (!m_initialized)
    {
        return;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend || !backend->engineInitialized)
    {
        return;
    }

    auto groupIt = backend->busGroups.find(busId);
    if (groupIt == backend->busGroups.end())
    {
        return;
    }

    ma_node* outputTargetNode = nullptr;
    uint32 outputTargetInputBusIndex = 0;
    if (!GetBusEffectOutputTarget(backend, m_buses, busId, outputTargetNode, outputTargetInputBusIndex))
    {
        return;
    }

    BusEffectState& effect = backend->busEffects[busId];
    effect.lowPassCutoff = NormalizeLowPassCutoff(cutoffFrequency);
    RebuildBusEffectChain(
        backend,
        m_config,
        groupIt->second,
        busId,
        effect,
        outputTargetNode,
        outputTargetInputBusIndex);
}

void AudioEngine::SetBusReverb(uint32 busId, const ReverbSettings& settings, bool enabled)
{
    if (!m_initialized)
    {
        return;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend || !backend->engineInitialized)
    {
        return;
    }

    auto groupIt = backend->busGroups.find(busId);
    if (groupIt == backend->busGroups.end())
    {
        return;
    }

    ma_node* outputTargetNode = nullptr;
    uint32 outputTargetInputBusIndex = 0;
    if (!GetBusEffectOutputTarget(backend, m_buses, busId, outputTargetNode, outputTargetInputBusIndex))
    {
        return;
    }

    BusEffectState& effect = backend->busEffects[busId];
    effect.reverbSettings = settings;
    effect.reverbEnabled = enabled;
    RebuildBusEffectChain(
        backend,
        m_config,
        groupIt->second,
        busId,
        effect,
        outputTargetNode,
        outputTargetInputBusIndex);
}

void AudioEngine::ApplyBusEffects(uint32 busId, const std::vector<std::shared_ptr<IAudioEffect>>& effects)
{
    float lowPassCutoff = 20000.0f;
    bool hasLowPass = false;

    ReverbSettings reverbSettings;
    bool hasReverb = false;
    std::vector<std::shared_ptr<IAudioEffect>> genericEffects;

    for (const std::shared_ptr<IAudioEffect>& effect : effects)
    {
        if (!effect || !effect->IsEnabled())
        {
            continue;
        }

        switch (effect->GetType())
        {
        case EffectType::LowPass:
            if (!hasLowPass)
            {
                lowPassCutoff = effect->HasParameter("cutoff") ? effect->GetParameter("cutoff") : lowPassCutoff;
                hasLowPass = true;
            }
            break;

        case EffectType::Reverb:
            if (!hasReverb)
            {
                if (effect->HasParameter("roomSize"))
                {
                    reverbSettings.roomSize = effect->GetParameter("roomSize");
                }
                if (effect->HasParameter("damping"))
                {
                    reverbSettings.damping = effect->GetParameter("damping");
                }
                if (effect->HasParameter("wetLevel"))
                {
                    reverbSettings.wetLevel = effect->GetParameter("wetLevel");
                }
                if (effect->HasParameter("dryLevel"))
                {
                    reverbSettings.dryLevel = effect->GetParameter("dryLevel");
                }
                if (effect->HasParameter("width"))
                {
                    reverbSettings.width = effect->GetParameter("width");
                }
                hasReverb = true;
            }
            break;

        default:
            genericEffects.push_back(effect);
            break;
        }
    }

    SetBusLowPassCutoff(busId, hasLowPass ? lowPassCutoff : 20000.0f);
    SetBusReverb(busId, reverbSettings, hasReverb && reverbSettings.wetLevel > 0.0f);

    if (!m_initialized)
    {
        return;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend || !backend->engineInitialized)
    {
        return;
    }

    auto groupIt = backend->busGroups.find(busId);
    if (groupIt == backend->busGroups.end())
    {
        return;
    }

    ma_node* outputTargetNode = nullptr;
    uint32 outputTargetInputBusIndex = 0;
    if (!GetBusEffectOutputTarget(backend, m_buses, busId, outputTargetNode, outputTargetInputBusIndex))
    {
        return;
    }

    BusEffectState& busEffect = backend->busEffects[busId];
    busEffect.genericEffects = std::move(genericEffects);
    RebuildBusEffectChain(
        backend,
        m_config,
        groupIt->second,
        busId,
        busEffect,
        outputTargetNode,
        outputTargetInputBusIndex);
}

void AudioEngine::SetBusSends(uint32 busId, const std::vector<AudioBusSend>& sends)
{
    if (!m_initialized)
    {
        return;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend || !backend->engineInitialized)
    {
        return;
    }

    auto groupIt = backend->busGroups.find(busId);
    if (groupIt == backend->busGroups.end())
    {
        return;
    }

    ma_node* outputTargetNode = nullptr;
    uint32 outputTargetInputBusIndex = 0;
    if (!GetBusEffectOutputTarget(backend, m_buses, busId, outputTargetNode, outputTargetInputBusIndex))
    {
        return;
    }

    BusEffectState& busEffect = backend->busEffects[busId];
    busEffect.sends.clear();
    busEffect.sends.reserve(std::min(sends.size(), BusSendNode::kMaxSends));
    uint32 droppedSendCount = 0;
    for (const AudioBusSend& send : sends)
    {
        if (busEffect.sends.size() >= BusSendNode::kMaxSends)
        {
            ++droppedSendCount;
            continue;
        }

        const float amount = std::clamp(send.amount, 0.0f, 1.0f);
        if (amount <= 0.0f || send.targetBusId == busId)
        {
            ++droppedSendCount;
            continue;
        }
        if (send.targetBusId != BusId::Master && backend->busGroups.find(send.targetBusId) == backend->busGroups.end())
        {
            ++droppedSendCount;
            continue;
        }
        if (WouldCreateRuntimeSendCycle(backend, busId, send.targetBusId))
        {
            RVX_CORE_WARN("Dropping cyclic audio bus send {} -> {}", busId, send.targetBusId);
            ++droppedSendCount;
            continue;
        }

        busEffect.sends.push_back(AudioBusSend{send.targetBusId, amount});
    }
    busEffect.droppedSendCount = droppedSendCount;

    RebuildBusEffectChain(
        backend,
        m_config,
        groupIt->second,
        busId,
        busEffect,
        outputTargetNode,
        outputTargetInputBusIndex);
}

bool AudioEngine::HasBus(uint32 busId) const
{
    return std::any_of(m_buses.begin(), m_buses.end(),
        [busId](const AudioBus& bus) { return bus.id == busId; });
}

uint32 AudioEngine::GetBusId(const std::string& name) const
{
    auto it = std::find_if(m_buses.begin(), m_buses.end(),
        [&name](const AudioBus& bus) { return bus.name == name; });
    return it != m_buses.end() ? it->id : RVX_INVALID_INDEX;
}

uint32 AudioEngine::GetSoundBus(AudioHandle handle) const
{
    if (!handle.IsValid() || !m_initialized)
    {
        return 0;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    return it != backend->sounds.end() ? it->second->busId : 0;
}

float AudioEngine::GetBusLowPassCutoff(uint32 busId) const
{
    if (!m_initialized)
    {
        return 20000.0f;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend)
    {
        return 20000.0f;
    }

    auto it = backend->busEffects.find(busId);
    return it != backend->busEffects.end() ? it->second.lowPassCutoff : 20000.0f;
}

ReverbSettings AudioEngine::GetBusReverbSettings(uint32 busId) const
{
    if (!m_initialized)
    {
        return ReverbSettings{};
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend)
    {
        return ReverbSettings{};
    }

    auto it = backend->busEffects.find(busId);
    return it != backend->busEffects.end() ? it->second.reverbSettings : ReverbSettings{};
}

bool AudioEngine::IsBusReverbEnabled(uint32 busId) const
{
    if (!m_initialized)
    {
        return false;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend)
    {
        return false;
    }

    auto it = backend->busEffects.find(busId);
    return it != backend->busEffects.end() && it->second.reverbEnabled;
}

Vec3 AudioEngine::GetSoundPosition(AudioHandle handle) const
{
    if (!handle.IsValid() || !m_initialized)
    {
        return Vec3(0.0f);
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend)
    {
        return Vec3(0.0f);
    }

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    return it != backend->sounds.end() ? it->second->position : Vec3(0.0f);
}

float AudioEngine::GetSoundVolume(AudioHandle handle) const
{
    if (!handle.IsValid() || !m_initialized)
    {
        return 0.0f;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend)
    {
        return 0.0f;
    }

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    return it != backend->sounds.end() ? it->second->volume : 0.0f;
}

float AudioEngine::GetSoundLowPassCutoff(AudioHandle handle) const
{
    if (!handle.IsValid() || !m_initialized)
    {
        return 20000.0f;
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend)
    {
        return 20000.0f;
    }

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    return it != backend->sounds.end() ? it->second->lowPassCutoff : 20000.0f;
}

AudioEngine::Statistics AudioEngine::GetStatistics() const
{
    Statistics stats;
    stats.totalVoices = m_config.maxVoices;
    stats.busCount = static_cast<uint32>(m_buses.size());

    if (m_initialized)
    {
        auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
        if (backend)
        {
            std::lock_guard<std::mutex> lock(backend->soundsMutex);
            stats.activeVoices = static_cast<uint32>(backend->sounds.size());
            stats.routedVoices = static_cast<uint32>(std::count_if(
                backend->sounds.begin(),
                backend->sounds.end(),
                [](const auto& pair) { return pair.second && pair.second->busId != 0; }));
            stats.cachedClipFiles = static_cast<uint32>(backend->registeredClipFiles.size());
            stats.resourceManagerActive = backend->resourceManagerInitialized;

            for (const auto& pair : backend->busEffects)
            {
                stats.activeBusSends += static_cast<uint32>(pair.second.sends.size());
                stats.droppedBusSends += pair.second.droppedSendCount;
            }
        }
    }

    return stats;
}

AudioEngine& GetAudioEngine()
{
    static AudioEngine instance;
    return instance;
}

} // namespace RVX::Audio
