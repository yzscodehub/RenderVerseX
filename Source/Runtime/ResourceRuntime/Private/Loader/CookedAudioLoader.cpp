/** @file CookedAudioLoader.cpp @brief Cooked PCM16 runtime loader */

#include "Resource/Loader/CookedAudioLoader.h"

#include "Core/Hash/SHA256.h"
#include "Resource/Cooked/CookedAudioArtifact.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace RVX::Resource
{
namespace
{
    bool ReadArtifact(
        const std::string& path,
        std::vector<uint8>& outBytes,
        CookedAudioArtifact& outArtifact,
        std::string& outError)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            outError = "Cannot open cooked audio artifact: " + path;
            return false;
        }
        outBytes.assign(std::istreambuf_iterator<char>(file), {});
        if (outBytes.empty())
        {
            outError = "Cooked audio artifact is empty: " + path;
            return false;
        }
        return DeserializeCookedAudioArtifact(outBytes, outArtifact, outError);
    }

    AudioMetadata MakeMetadata(const CookedAudioArtifact& artifact)
    {
        AudioMetadata metadata;
        metadata.sampleRate = artifact.sampleRate;
        metadata.channels = artifact.channels;
        metadata.bitsPerSample = 16;
        metadata.totalFrames = artifact.frameCount;
        metadata.duration = FramesToDuration(artifact.frameCount,
                                             artifact.sampleRate);
        metadata.format = AudioFormat::S16;
        metadata.loadMode = AudioLoadMode::FullyLoaded;
        metadata.sourceFormat = "rvx-audio-v2";
        metadata.isLooping = artifact.looping;
        metadata.loopStartFrame = artifact.loopStartFrame;
        metadata.loopEndFrame = artifact.looping
            ? artifact.loopEndFrame
            : artifact.frameCount;
        return metadata;
    }

    std::vector<uint8> MakePcmBytes(const CookedAudioArtifact& artifact)
    {
        std::vector<uint8> bytes(artifact.pcm16.size() * sizeof(int16));
        for (size_t index = 0; index < artifact.pcm16.size(); ++index)
        {
            const uint16 value =
                std::bit_cast<uint16>(artifact.pcm16[index]);
            bytes[index * 2u] = static_cast<uint8>(value);
            bytes[index * 2u + 1u] = static_cast<uint8>(value >> 8u);
        }
        return bytes;
    }

    ResourceContentIdentity MakeObservedIdentity(
        std::span<const uint8> bytes)
    {
        ResourceContentIdentity identity;
        identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = ResourceContentIdentityDomain::CookedArtifact;
        identity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = ResourceContentHashAlgorithm::SHA256;
        identity.digest = Hash::FormatSHA256Digest(
            Hash::ComputeSHA256(bytes.data(), bytes.size()));
        identity.byteCount = bytes.size();
        identity.fileCount = 1;
        return identity;
    }
} // namespace

std::vector<std::string> CookedAudioLoader::GetSupportedExtensions() const
{
    return {".rva"};
}

bool CookedAudioLoader::Supports(
    const ResourceLoaderRequest& request) const noexcept
{
    try
    {
        std::string extension =
            std::filesystem::path(request.resolvedPath).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value)
                       {
                           return static_cast<char>(std::tolower(value));
                       });
        return request.type == ResourceType::Audio &&
            (request.domain == ResourceLoadDomain::Cooked ||
             request.domain == ResourceLoadDomain::Package) &&
            extension == ".rva";
    }
    catch (...)
    {
        return false;
    }
}

bool CookedAudioLoader::Prepare(
    const ResourceLoadPreparationContext& context,
    PreparedResourceBundle& outBundle,
    ResourceLoadError& outError)
{
    if (context.IsCancellationRequested())
    {
        outError = {ResourceLoadErrorCode::Cancelled,
                    "Cooked audio load was cancelled."};
        return false;
    }
    std::vector<uint8> bytes;
    CookedAudioArtifact artifact;
    std::string error;
    if (!ReadArtifact(context.resolvedPath, bytes, artifact, error))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure, std::move(error)};
        return false;
    }
    auto root = ResourceHandle<AudioResource>(new AudioResource());
    root->SetId(context.rootResourceId);
    root->SetPath(context.requestedPath);
    root->SetName(std::filesystem::path(context.requestedPath).stem().string());
    if (!root->SetPreparedData(MakePcmBytes(artifact), MakeMetadata(artifact)) ||
        !outBundle.SetRoot(ResourceHandle<IResource>(root)) ||
        !outBundle.SetObservedContentIdentity(MakeObservedIdentity(bytes)))
    {
        outError = {ResourceLoadErrorCode::LoaderFailure,
                    "Cooked audio could not form a prepared bundle."};
        return false;
    }
    return true;
}
} // namespace RVX::Resource
