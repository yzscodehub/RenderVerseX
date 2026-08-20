/**
 * @file AudioImporter.cpp
 * @brief Audio importer implementation
 */

#include "Tools/Importers/AudioImporter.h"
#include "Core/Log.h"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstring>

// Note: In a full implementation, this would use miniaudio for decoding
// For now, we provide a stub implementation that copies files

namespace RVX::Tools
{

ImportResult AudioImporterEx::Import(const fs::path& sourcePath,
                                      const fs::path& outputPath,
                                      const void* options)
{
    AudioImportOptions opts;
    if (options)
    {
        opts = *static_cast<const AudioImportOptions*>(options);
    }

    auto result = ImportAudio(sourcePath, outputPath, opts);
    
    ImportResult basicResult;
    basicResult.success = result.success;
    basicResult.error = result.error;
    basicResult.outputPaths = result.outputPaths;
    basicResult.warnings = result.warnings;
    
    return basicResult;
}

AudioImportResult AudioImporterEx::ImportAudio(const fs::path& sourcePath,
                                                const fs::path& outputPath,
                                                const AudioImportOptions& options)
{
    AudioImportResult result;

    // Check source exists
    if (!fs::exists(sourcePath))
    {
        result.success = false;
        result.error = "Source file not found: " + sourcePath.string();
        return result;
    }

    // Check format
    std::string ext = sourcePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (!IsFormatSupported(ext))
    {
        result.success = false;
        result.error = "Unsupported audio format: " + ext;
        return result;
    }

    // Analyze source
    auto analysis = AnalyzeAudio(sourcePath);
    result.sourceSampleRate = analysis.sourceSampleRate;
    result.sourceChannels = analysis.sourceChannels;
    result.sourceBitsPerSample = analysis.sourceBitsPerSample;
    result.sourceDuration = analysis.sourceDuration;

    // Load and decode
    std::vector<float> samples;
    uint32 sampleRate = 0;
    uint32 channels = 0;

    if (!LoadAndDecode(sourcePath, samples, sampleRate, channels))
    {
        result.success = false;
        result.error = "Failed to decode audio file";
        return result;
    }

    // Apply processing
    uint32 targetSampleRate = sampleRate;
    uint32 targetChannels = channels;

    // Resample
    if (options.resample && options.targetSampleRate != sampleRate)
    {
        if (Resample(samples, sampleRate, options.targetSampleRate, channels))
        {
            targetSampleRate = options.targetSampleRate;
            result.wasResampled = true;
        }
    }

    // Channel conversion
    if (options.forceMono && channels > 1)
    {
        if (ConvertChannels(samples, channels, 1))
        {
            targetChannels = 1;
        }
    }
    else if (options.forceStereo && channels == 1)
    {
        if (ConvertChannels(samples, channels, 2))
        {
            targetChannels = 2;
        }
    }

    // Normalize
    if (options.normalize)
    {
        if (Normalize(samples, options.normalizeTargetDb))
        {
            result.wasNormalized = true;
        }
    }

    // Trim silence
    if (options.trimSilence)
    {
        if (TrimSilence(samples, targetChannels, options.silenceThresholdDb))
        {
            result.wasTrimmed = true;
        }
    }

    // Detect loop points
    if (options.detectLoopPoints)
    {
        auto [loopStart, loopEnd] = DetectLoopPoints(samples, targetSampleRate, targetChannels);
        result.loopStartTime = loopStart;
        result.loopEndTime = loopEnd;
    }
    else if (options.loopStart > 0.0f || options.loopEnd > 0.0f)
    {
        result.loopStartTime = options.loopStart;
        result.loopEndTime = options.loopEnd;
    }

    // Check streaming
    size_t dataSize = samples.size() * sizeof(float);
    result.isStreaming = options.enableStreaming || (dataSize > options.streamingThreshold);

    // Create output directory
    fs::path outputDir = outputPath.parent_path();
    if (!outputDir.empty() && !fs::exists(outputDir))
    {
        fs::create_directories(outputDir);
    }

    // Write output
    if (!WriteOutput(outputPath, samples, targetSampleRate, targetChannels, options))
    {
        result.success = false;
        result.error = "Failed to write output file";
        return result;
    }

    // Fill result
    result.success = true;
    result.outputPaths.push_back(outputPath.string());
    result.outputSampleRate = targetSampleRate;
    result.outputChannels = targetChannels;
    result.outputFileSize = fs::file_size(outputPath);

    RVX_CORE_INFO("Imported audio: {} -> {} ({} Hz, {} ch, {:.2f}s)",
        sourcePath.filename().string(),
        outputPath.filename().string(),
        targetSampleRate,
        targetChannels,
        result.sourceDuration);

    return result;
}

AudioImportResult AudioImporterEx::AnalyzeAudio(const fs::path& sourcePath)
{
    AudioImportResult result;

    // For now, just get file size and estimate
    // Full implementation would use miniaudio to read metadata

    if (!fs::exists(sourcePath))
    {
        result.success = false;
        result.error = "File not found";
        return result;
    }

    size_t fileSize = fs::file_size(sourcePath);
    std::string ext = sourcePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Default assumptions (would be replaced by actual analysis)
    result.sourceSampleRate = 44100;
    result.sourceChannels = 2;
    result.sourceBitsPerSample = 16;

    // Estimate duration based on file size and format
    if (ext == ".wav")
    {
        // Assume 16-bit stereo PCM
        size_t headerSize = 44;
        size_t dataSize = fileSize - headerSize;
        result.sourceDuration = static_cast<float>(dataSize) / 
            (result.sourceSampleRate * result.sourceChannels * 2);
    }
    else if (ext == ".mp3")
    {
        // Rough estimate: MP3 at 128kbps
        result.sourceDuration = static_cast<float>(fileSize) / 16000.0f;
    }
    else if (ext == ".ogg")
    {
        // Rough estimate: OGG at similar bitrate
        result.sourceDuration = static_cast<float>(fileSize) / 16000.0f;
    }
    else if (ext == ".flac")
    {
        // FLAC is about 50-60% of WAV size
        size_t estimatedWavSize = fileSize * 2;
        result.sourceDuration = static_cast<float>(estimatedWavSize) / 
            (result.sourceSampleRate * result.sourceChannels * 2);
    }

    result.success = true;
    return result;
}

AudioImportOptions AudioImporterEx::GetRecommendedOptions(const fs::path& sourcePath)
{
    AudioImportOptions options;

    size_t fileSize = fs::file_size(sourcePath);
    std::string ext = sourcePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Large files should stream
    if (fileSize > 5 * 1024 * 1024)  // 5 MB
    {
        options.enableStreaming = true;
    }

    // Music files typically don't need resampling
    // SFX might benefit from consistent sample rate

    return options;
}

bool AudioImporterEx::IsFormatSupported(const std::string& extension)
{
    std::string ext = extension;
    if (!ext.empty() && ext[0] != '.')
    {
        ext = "." + ext;
    }
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    static const std::vector<std::string> supported = {
        ".wav", ".mp3", ".ogg", ".flac", ".aiff", ".aif"
    };

    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

const char* AudioImporterEx::GetFormatDescription(const std::string& extension)
{
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".wav" || ext == "wav") return "Waveform Audio (PCM)";
    if (ext == ".mp3" || ext == "mp3") return "MPEG Audio Layer III (Lossy)";
    if (ext == ".ogg" || ext == "ogg") return "Ogg Vorbis (Lossy)";
    if (ext == ".flac" || ext == "flac") return "Free Lossless Audio Codec";
    if (ext == ".aiff" || ext == "aiff" || ext == ".aif" || ext == "aif") 
        return "Audio Interchange File Format";

    return "Unknown Format";
}

bool AudioImporterEx::LoadAndDecode(const fs::path& sourcePath,
                                     std::vector<float>& outSamples,
                                     uint32& outSampleRate,
                                     uint32& outChannels)
{
    // Stub implementation - in real code, use miniaudio here
    // For now, just copy the file

    std::ifstream file(sourcePath, std::ios::binary);
    if (!file)
    {
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Pretend we decoded it
    // Real implementation would use ma_decoder

    outSampleRate = 44100;
    outChannels = 2;

    // Allocate based on estimated duration
    size_t estimatedFrames = fileSize / 4;  // Rough estimate
    outSamples.resize(estimatedFrames * outChannels, 0.0f);

    return true;
}

bool AudioImporterEx::Resample(std::vector<float>& samples,
                                uint32 sourceRate,
                                uint32 targetRate,
                                uint32 channels)
{
    if (sourceRate == targetRate)
    {
        return true;
    }

    // Simple linear interpolation resampling
    // Real implementation would use higher quality algorithm

    double ratio = static_cast<double>(targetRate) / static_cast<double>(sourceRate);
    size_t sourceFrames = samples.size() / channels;
    size_t targetFrames = static_cast<size_t>(sourceFrames * ratio);

    std::vector<float> resampled(targetFrames * channels);

    for (size_t i = 0; i < targetFrames; ++i)
    {
        double sourcePos = i / ratio;
        size_t sourceIndex = static_cast<size_t>(sourcePos);
        double frac = sourcePos - sourceIndex;

        if (sourceIndex + 1 >= sourceFrames)
        {
            sourceIndex = sourceFrames - 1;
            frac = 0.0;
        }

        for (uint32 ch = 0; ch < channels; ++ch)
        {
            float s0 = samples[sourceIndex * channels + ch];
            float s1 = samples[(sourceIndex + 1) * channels + ch];
            resampled[i * channels + ch] = static_cast<float>(s0 * (1.0 - frac) + s1 * frac);
        }
    }

    samples = std::move(resampled);
    return true;
}

bool AudioImporterEx::ConvertChannels(std::vector<float>& samples,
                                       uint32 sourceChannels,
                                       uint32 targetChannels)
{
    if (sourceChannels == targetChannels)
    {
        return true;
    }

    size_t frameCount = samples.size() / sourceChannels;
    std::vector<float> converted(frameCount * targetChannels);

    if (sourceChannels == 2 && targetChannels == 1)
    {
        // Stereo to mono
        for (size_t i = 0; i < frameCount; ++i)
        {
            converted[i] = (samples[i * 2] + samples[i * 2 + 1]) * 0.5f;
        }
    }
    else if (sourceChannels == 1 && targetChannels == 2)
    {
        // Mono to stereo
        for (size_t i = 0; i < frameCount; ++i)
        {
            converted[i * 2] = samples[i];
            converted[i * 2 + 1] = samples[i];
        }
    }

    samples = std::move(converted);
    return true;
}

bool AudioImporterEx::Normalize(std::vector<float>& samples, float targetDb)
{
    if (samples.empty())
    {
        return false;
    }

    // Find peak
    float peak = 0.0f;
    for (float sample : samples)
    {
        peak = std::max(peak, std::abs(sample));
    }

    if (peak <= 0.0f)
    {
        return false;
    }

    // Calculate gain
    float targetLinear = std::pow(10.0f, targetDb / 20.0f);
    float gain = targetLinear / peak;

    // Apply gain
    for (float& sample : samples)
    {
        sample *= gain;
    }

    return true;
}

bool AudioImporterEx::TrimSilence(std::vector<float>& samples,
                                   uint32 channels,
                                   float thresholdDb)
{
    if (samples.empty())
    {
        return false;
    }

    float thresholdLinear = std::pow(10.0f, thresholdDb / 20.0f);
    size_t frameCount = samples.size() / channels;

    // Find first non-silent frame
    size_t startFrame = 0;
    for (size_t i = 0; i < frameCount; ++i)
    {
        bool isSilent = true;
        for (uint32 ch = 0; ch < channels; ++ch)
        {
            if (std::abs(samples[i * channels + ch]) > thresholdLinear)
            {
                isSilent = false;
                break;
            }
        }
        if (!isSilent)
        {
            startFrame = i;
            break;
        }
    }

    // Find last non-silent frame
    size_t endFrame = frameCount;
    for (size_t i = frameCount; i > 0; --i)
    {
        bool isSilent = true;
        for (uint32 ch = 0; ch < channels; ++ch)
        {
            if (std::abs(samples[(i - 1) * channels + ch]) > thresholdLinear)
            {
                isSilent = false;
                break;
            }
        }
        if (!isSilent)
        {
            endFrame = i;
            break;
        }
    }

    if (startFrame >= endFrame)
    {
        return false;
    }

    // Trim
    size_t trimmedSize = (endFrame - startFrame) * channels;
    std::vector<float> trimmed(trimmedSize);
    std::memcpy(trimmed.data(), samples.data() + startFrame * channels, 
                trimmedSize * sizeof(float));
    samples = std::move(trimmed);

    return true;
}

std::pair<float, float> AudioImporterEx::DetectLoopPoints(
    const std::vector<float>& samples,
    uint32 sampleRate,
    uint32 channels)
{
    // Simple loop detection: find similar waveforms at start and end
    // Real implementation would use more sophisticated correlation

    float duration = static_cast<float>(samples.size() / channels) / 
                     static_cast<float>(sampleRate);

    // Default: loop entire file
    return {0.0f, duration};
}

bool AudioImporterEx::WriteOutput(const fs::path& outputPath,
                                   const std::vector<float>& samples,
                                   uint32 sampleRate,
                                   uint32 channels,
                                   const AudioImportOptions& options)
{
    (void)options;

    // Write as WAV for now
    // Full implementation would support multiple output formats

    std::ofstream file(outputPath, std::ios::binary);
    if (!file)
    {
        return false;
    }

    // WAV header
    uint32 dataSize = static_cast<uint32>(samples.size() * sizeof(int16));
    uint32 fileSize = 36 + dataSize;

    // RIFF header
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&fileSize), 4);
    file.write("WAVE", 4);

    // fmt chunk
    file.write("fmt ", 4);
    uint32 fmtSize = 16;
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);

    uint16 audioFormat = 1;  // PCM
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);

    uint16 numChannels = static_cast<uint16>(channels);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);

    file.write(reinterpret_cast<const char*>(&sampleRate), 4);

    uint32 byteRate = sampleRate * channels * 2;
    file.write(reinterpret_cast<const char*>(&byteRate), 4);

    uint16 blockAlign = static_cast<uint16>(channels * 2);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);

    uint16 bitsPerSample = 16;
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

    // data chunk
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);

    // Convert float to int16 and write
    for (float sample : samples)
    {
        float clamped = std::clamp(sample, -1.0f, 1.0f);
        int16 int16Sample = static_cast<int16>(clamped * 32767.0f);
        file.write(reinterpret_cast<const char*>(&int16Sample), 2);
    }

    return true;
}

} // namespace RVX::Tools
