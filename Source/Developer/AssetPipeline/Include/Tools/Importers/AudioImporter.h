/**
 * @file AudioImporter.h
 * @brief Audio asset importer
 */

#pragma once

#include "Tools/AssetPipeline.h"
#include "Audio/AudioTypes.h"
#include <string>

namespace RVX::Tools
{

/**
 * @brief Audio import options
 */
struct AudioImportOptions
{
    // Format conversion
    bool forceFormat = false;                  ///< Convert to specific format
    Audio::AudioFormat targetFormat = Audio::AudioFormat::S16;
    
    // Sample rate
    bool resample = false;                     ///< Resample to target rate
    uint32 targetSampleRate = 44100;
    
    // Channels
    bool forceMono = false;                    ///< Force mono output
    bool forceStereo = false;                  ///< Force stereo output
    
    // Compression (for output format)
    bool compress = false;                     ///< Compress output
    float compressionQuality = 0.8f;           ///< 0-1 for lossy formats
    
    // Streaming
    bool enableStreaming = false;              ///< Mark for streaming (large files)
    size_t streamingThreshold = 1024 * 1024;   ///< Auto-enable streaming above this size
    
    // Normalization
    bool normalize = false;                    ///< Normalize audio levels
    float normalizeTargetDb = -3.0f;           ///< Target peak in dB
    
    // Trim
    bool trimSilence = false;                  ///< Remove silence from start/end
    float silenceThresholdDb = -60.0f;         ///< Threshold for silence detection
    
    // Metadata
    bool preserveMetadata = true;              ///< Keep embedded metadata
    
    // Loop points (for game audio)
    bool detectLoopPoints = false;             ///< Auto-detect loop points
    bool embedLoopPoints = false;              ///< Embed loop markers in output
    float loopStart = 0.0f;                    ///< Manual loop start (seconds)
    float loopEnd = 0.0f;                      ///< Manual loop end (seconds)
};

/**
 * @brief Audio import result with detailed info
 */
struct AudioImportResult : public ImportResult
{
    // Source info
    uint32 sourceSampleRate = 0;
    uint32 sourceChannels = 0;
    uint32 sourceBitsPerSample = 0;
    float sourceDuration = 0.0f;
    
    // Output info
    uint32 outputSampleRate = 0;
    uint32 outputChannels = 0;
    size_t outputFileSize = 0;
    
    // Processing info
    bool wasResampled = false;
    bool wasNormalized = false;
    bool wasTrimmed = false;
    bool isStreaming = false;
    
    // Loop points (if detected/specified)
    float loopStartTime = 0.0f;
    float loopEndTime = 0.0f;
};

/**
 * @brief Extended audio importer with full processing options
 * 
 * Supports importing various audio formats and optionally:
 * - Resampling to target sample rate
 * - Converting to mono/stereo
 * - Normalizing levels
 * - Trimming silence
 * - Detecting/embedding loop points
 * 
 * Supported formats:
 * - WAV (PCM, float)
 * - MP3 (via miniaudio/dr_libs)
 * - OGG Vorbis
 * - FLAC
 * 
 * Usage:
 * @code
 * AudioImporterEx importer;
 * 
 * AudioImportOptions options;
 * options.normalize = true;
 * options.targetSampleRate = 44100;
 * options.resample = true;
 * 
 * auto result = importer.ImportAudio("sound.wav", "Assets/sound.audio", options);
 * @endcode
 */
class AudioImporterEx : public IAssetImporter
{
public:
    AudioImporterEx() = default;
    ~AudioImporterEx() override = default;

    // =========================================================================
    // IAssetImporter Interface
    // =========================================================================

    const char* GetName() const override { return "AudioImporterEx"; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".wav", ".mp3", ".ogg", ".flac", ".aiff", ".aif"};
    }

    AssetType GetAssetType() const override { return AssetType::Audio; }

    ImportResult Import(const fs::path& sourcePath,
                        const fs::path& outputPath,
                        const void* options = nullptr) override;

    // =========================================================================
    // Extended Import
    // =========================================================================

    /**
     * @brief Import with detailed options and result
     */
    AudioImportResult ImportAudio(const fs::path& sourcePath,
                                   const fs::path& outputPath,
                                   const AudioImportOptions& options);

    /**
     * @brief Analyze audio file without importing
     */
    AudioImportResult AnalyzeAudio(const fs::path& sourcePath);

    // =========================================================================
    // Utilities
    // =========================================================================

    /**
     * @brief Get recommended options for a file
     */
    static AudioImportOptions GetRecommendedOptions(const fs::path& sourcePath);

    /**
     * @brief Check if format is supported
     */
    static bool IsFormatSupported(const std::string& extension);

    /**
     * @brief Get format description
     */
    static const char* GetFormatDescription(const std::string& extension);

private:
    bool LoadAndDecode(const fs::path& sourcePath,
                       std::vector<float>& outSamples,
                       uint32& outSampleRate,
                       uint32& outChannels);

    bool Resample(std::vector<float>& samples,
                  uint32 sourceRate,
                  uint32 targetRate,
                  uint32 channels);

    bool ConvertChannels(std::vector<float>& samples,
                         uint32 sourceChannels,
                         uint32 targetChannels);

    bool Normalize(std::vector<float>& samples, float targetDb);

    bool TrimSilence(std::vector<float>& samples,
                     uint32 channels,
                     float thresholdDb);

    std::pair<float, float> DetectLoopPoints(const std::vector<float>& samples,
                                             uint32 sampleRate,
                                             uint32 channels);

    bool WriteOutput(const fs::path& outputPath,
                     const std::vector<float>& samples,
                     uint32 sampleRate,
                     uint32 channels,
                     const AudioImportOptions& options);
};

} // namespace RVX::Tools
