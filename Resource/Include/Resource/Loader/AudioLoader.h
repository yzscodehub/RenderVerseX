#pragma once

/**
 * @file AudioLoader.h
 * @brief Audio resource loader using miniaudio
 * 
 * Loads audio files from various formats:
 * - WAV (PCM, IEEE float)
 * - MP3 (via dr_mp3)
 * - OGG Vorbis (via stb_vorbis)  
 * - FLAC (via dr_flac)
 * 
 * Supports both fully loaded and streaming modes.
 */

#include "Resource/ResourceManager.h"
#include "Resource/Types/AudioResource.h"
#include <string>
#include <memory>

namespace RVX::Resource
{
    /**
     * @brief Audio loading options
     */
    struct AudioLoadOptions
    {
        /// Target sample format (Unknown = keep original)
        AudioFormat targetFormat = AudioFormat::Unknown;

        /// Target sample rate (0 = keep original)
        uint32_t targetSampleRate = 0;

        /// Force mono output
        bool forceMono = false;

        /// Force stereo output
        bool forceStereo = false;

        /// Enable streaming for large files
        bool enableStreaming = false;

        /// Threshold for auto-enabling streaming (bytes)
        size_t streamingThreshold = 5 * 1024 * 1024;  // 5 MB

        /// Preload some data even in streaming mode
        bool preloadStreamingBuffer = true;

        /// Size of streaming buffer to preload (in seconds)
        float preloadBufferSeconds = 1.0f;
    };

    /**
     * @brief Audio resource loader using miniaudio
     * 
     * Features:
     * - Loads audio from common formats (WAV, MP3, OGG, FLAC)
     * - Automatic format detection
     * - Format conversion (sample rate, channels, bit depth)
     * - Streaming mode for large files
     * - Provides default silent audio for errors
     */
    class AudioLoader : public IResourceLoader
    {
    public:
        explicit AudioLoader(ResourceManager* manager);
        ~AudioLoader() override;

        // =====================================================================
        // IResourceLoader Interface
        // =====================================================================

        ResourceType GetResourceType() const override { return ResourceType::Audio; }
        std::vector<std::string> GetSupportedExtensions() const override;
        IResource* Load(const std::string& path) override;
        bool CanLoad(const std::string& path) const override;

        // =====================================================================
        // Extended Loading API
        // =====================================================================

        /**
         * @brief Load audio with options
         * 
         * @param path Path to audio file
         * @param options Loading options
         * @return AudioResource pointer (ownership handled by ResourceManager)
         */
        AudioResource* LoadWithOptions(const std::string& path, 
                                        const AudioLoadOptions& options);

        /**
         * @brief Load audio from memory
         * 
         * @param data Pointer to encoded audio data
         * @param size Size of data in bytes
         * @param uniqueKey Unique key for caching
         * @param options Loading options
         * @return AudioResource pointer
         */
        AudioResource* LoadFromMemory(const void* data, size_t size,
                                       const std::string& uniqueKey,
                                       const AudioLoadOptions& options = {});

        /**
         * @brief Load audio for streaming
         * 
         * Creates an AudioResource configured for streaming.
         * Data is not loaded into memory, only metadata.
         * 
         * @param path Path to audio file
         * @return AudioResource with streaming configuration
         */
        AudioResource* LoadForStreaming(const std::string& path);

        // =====================================================================
        // Default Audio
        // =====================================================================

        /**
         * @brief Get a silent audio resource (for errors)
         */
        AudioResource* GetSilentAudio();

        /**
         * @brief Generate a simple sine wave test audio
         * 
         * @param frequency Frequency in Hz
         * @param duration Duration in seconds
         * @param sampleRate Sample rate
         * @return Generated audio resource
         */
        AudioResource* GenerateSineWave(float frequency = 440.0f, 
                                         float duration = 1.0f,
                                         uint32_t sampleRate = 44100);

        // =====================================================================
        // Utilities
        // =====================================================================

        /**
         * @brief Get audio file info without fully loading
         */
        struct AudioFileInfo
        {
            bool valid = false;
            std::string format;         // "wav", "mp3", "ogg", "flac"
            uint32_t sampleRate = 0;
            uint32_t channels = 0;
            uint64_t totalFrames = 0;
            float duration = 0.0f;
            size_t fileSize = 0;
        };

        AudioFileInfo GetFileInfo(const std::string& path);

        /**
         * @brief Check if format is supported
         */
        static bool IsFormatSupported(const std::string& extension);

        /**
         * @brief Get format description
         */
        static const char* GetFormatDescription(const std::string& extension);

    private:
        /// Decode audio file using miniaudio
        bool DecodeFile(const std::string& path,
                        std::vector<uint8_t>& outData,
                        AudioMetadata& outMetadata,
                        const AudioLoadOptions& options);

        /// Decode audio from memory using miniaudio
        bool DecodeMemory(const void* data, size_t size,
                          std::vector<uint8_t>& outData,
                          AudioMetadata& outMetadata,
                          const AudioLoadOptions& options);

        /// Create resource and store in cache
        AudioResource* CreateAudioResource(std::vector<uint8_t> data,
                                            const AudioMetadata& metadata,
                                            const std::string& uniqueKey);

        /// Generate ResourceId from unique key
        ResourceId GenerateAudioId(const std::string& uniqueKey);

        ResourceManager* m_manager;
        AudioResource* m_silentAudio = nullptr;
    };

} // namespace RVX::Resource
