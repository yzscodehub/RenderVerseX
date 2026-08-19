#include "Resource/Importer/GLTFImporter.h"
#include "Core/Assert.h"
#include "Core/Hash/SHA256.h"
#include "Core/Log.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace RVX::Resource
{
    namespace
    {
        uint32_t GetTextureUsageRank(TextureUsage usage)
        {
            switch (usage)
            {
                case TextureUsage::Normal:
                    return 2;
                case TextureUsage::Data:
                    return 1;
                case TextureUsage::Color:
                default:
                    return 0;
            }
        }

        const char* TextureUsageName(TextureUsage usage)
        {
            switch (usage)
            {
                case TextureUsage::Normal:
                    return "Normal";
                case TextureUsage::Data:
                    return "Data";
                case TextureUsage::Color:
                default:
                    return "Color";
            }
        }

        int ResolveTextureImageIndex(const tinygltf::Model& gltf, int textureIndex)
        {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(gltf.textures.size()))
            {
                return -1;
            }

            const int imageIndex = gltf.textures[textureIndex].source;
            if (imageIndex < 0 || imageIndex >= static_cast<int>(gltf.images.size()))
            {
                return -1;
            }

            return imageIndex;
        }

        bool HasCookedTextureArtifactExtension(const std::string& uri)
        {
            std::string extension = std::filesystem::path(uri).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return extension == ".rva";
        }

        struct ImageCaptureContext
        {
            Diagnostics::TraceContext traceContext;
            std::string modelPath;
        };

        struct GLTFContentReadRecord
        {
            std::string normalizedRelativeUri;
            Hash::SHA256Digest digest{};
            uint64 byteCount = 0;
        };

        /**
         * @brief TinyGLTF filesystem boundary which records parser-consumed bytes.
         *
         * The callback deliberately delegates the read to TinyGLTF's normal
         * implementation, then hashes the exact returned vector.  It never
         * pre-reads or re-opens a file merely to produce an identity.
         */
        class GLTFContentCaptureContext
        {
        public:
            explicit GLTFContentCaptureContext(const std::string& rootPath)
            {
                std::string error;
                if (!NormalizePath(rootPath, m_rootFilePath, error))
                {
                    m_failure = std::move(error);
                    return;
                }
                m_rootDirectory = m_rootFilePath.parent_path();
            }

            [[nodiscard]] bool IsReady() const noexcept
            {
                return m_failure.empty() && !m_rootFilePath.empty() &&
                       !m_rootDirectory.empty();
            }

            [[nodiscard]] const std::string& GetFailure() const noexcept
            {
                return m_failure;
            }

            bool CanAccess(const std::string& path, std::filesystem::path& outPath,
                           std::string& outError) const
            {
                if (!IsReady())
                {
                    outError = m_failure.empty()
                                   ? "glTF content capture did not initialize a root path."
                                   : m_failure;
                    return false;
                }
                if (!NormalizePath(path, outPath, outError))
                {
                    return false;
                }
                if (outPath == m_rootFilePath || IsContainedByRoot(outPath))
                {
                    return true;
                }

                outError = "glTF external URI resolves outside the source root directory: " +
                           outPath.generic_string();
                return false;
            }

            bool RecordRead(const std::filesystem::path& path,
                            const std::vector<unsigned char>& bytes,
                            std::string& outError)
            {
                const bool isRoot = path == m_rootFilePath;
                const std::string relativeUri = isRoot
                    ? std::string{}
                    : path.lexically_relative(m_rootDirectory).generic_string();
                if (!isRoot && (relativeUri.empty() || relativeUri == "."))
                {
                    outError = "glTF dependency URI could not be normalized relative to the source root.";
                    return false;
                }

                const std::string key = isRoot
                    ? std::string(1, '\0')
                    : std::string(1, '\1') + relativeUri;
                GLTFContentReadRecord record;
                record.normalizedRelativeUri = relativeUri;
                record.digest = Hash::ComputeSHA256(bytes.data(), bytes.size());
                record.byteCount = static_cast<uint64>(bytes.size());

                const auto found = m_reads.find(key);
                if (found != m_reads.end())
                {
                    if (found->second.byteCount != record.byteCount ||
                        found->second.digest != record.digest)
                    {
                        outError = "glTF source changed while TinyGLTF was parsing it: " +
                                   path.generic_string();
                        return false;
                    }
                    return true;
                }

                m_reads.emplace(key, std::move(record));
                return true;
            }

            [[nodiscard]] ResourceContentIdentity BuildIdentity() const
            {
                const auto root = m_reads.find(std::string(1, '\0'));
                if (root == m_reads.end())
                {
                    return {};
                }

                ResourceContentIdentity identity;
                identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
                identity.domain = ResourceContentIdentityDomain::Source;
                identity.algorithm = ResourceContentHashAlgorithm::SHA256;
                identity.fileCount = static_cast<uint32>(m_reads.size());
                for (const auto& [key, record] : m_reads)
                {
                    (void)key;
                    if (record.byteCount > std::numeric_limits<uint64>::max() - identity.byteCount)
                    {
                        return {};
                    }
                    identity.byteCount += record.byteCount;
                }

                if (m_reads.size() == 1)
                {
                    identity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
                    identity.digest = Hash::FormatSHA256Digest(root->second.digest);
                    return identity;
                }

                // Closure format v1: a domain marker followed by URI-sorted
                // records of [normalized URI, NUL, byte count (big endian),
                // raw SHA-256 digest]. The individual digest is derived from
                // the exact vector returned to TinyGLTF, avoiding a second IO
                // pass and retaining deterministic bounded memory overhead.
                Hash::SHA256Hasher closureHasher;
                constexpr std::string_view closureDomain =
                    "RVX.ResourceContentIdentity.DependencyClosure.v1";
                constexpr uint8 separator = 0;
                closureHasher.Update(closureDomain);
                closureHasher.Update(&separator, 1);
                for (const auto& [key, record] : m_reads)
                {
                    (void)key;
                    closureHasher.Update(record.normalizedRelativeUri);
                    closureHasher.Update(&separator, 1);
                    std::array<uint8, 8> byteCountBytes{};
                    for (size_t index = 0; index < byteCountBytes.size(); ++index)
                    {
                        const uint32 shift = static_cast<uint32>(
                            (byteCountBytes.size() - 1 - index) * 8);
                        byteCountBytes[index] = static_cast<uint8>(record.byteCount >> shift);
                    }
                    closureHasher.Update(byteCountBytes.data(), byteCountBytes.size());
                    closureHasher.Update(record.digest.data(), record.digest.size());
                }

                identity.scope = ResourceContentIdentityScope::DependencyClosure;
                identity.digest = closureHasher.FinalizeHex();
                return identity;
            }

        private:
            static bool NormalizePath(const std::string& input,
                                      std::filesystem::path& outPath,
                                      std::string& outError)
            {
                std::error_code error;
                const std::filesystem::path absolute =
                    std::filesystem::absolute(std::filesystem::path(input), error);
                if (error)
                {
                    outError = "Could not make glTF source path absolute: " + input;
                    return false;
                }

                outPath = std::filesystem::weakly_canonical(absolute, error);
                if (error)
                {
                    outError = "Could not normalize glTF source path: " + input;
                    return false;
                }
                return true;
            }

            [[nodiscard]] bool IsContainedByRoot(const std::filesystem::path& path) const
            {
                const std::filesystem::path relative =
                    path.lexically_relative(m_rootDirectory);
                if (relative.empty() || relative.is_absolute())
                {
                    return false;
                }
                for (const std::filesystem::path& component : relative)
                {
                    if (component == "..")
                    {
                        return false;
                    }
                }
                return true;
            }

            std::filesystem::path m_rootFilePath;
            std::filesystem::path m_rootDirectory;
            std::map<std::string, GLTFContentReadRecord> m_reads;
            std::string m_failure;
        };

        bool ContentCaptureFileExists(const std::string& path, void* userData)
        {
            auto* capture = static_cast<GLTFContentCaptureContext*>(userData);
            std::filesystem::path resolved;
            std::string error;
            return capture && capture->CanAccess(path, resolved, error) &&
                   tinygltf::FileExists(resolved.string(), nullptr);
        }

        std::string ContentCaptureExpandFilePath(const std::string& path, void*)
        {
            // URI expansion is intentionally disabled here. The final path is
            // normalized and checked against the root at every filesystem edge.
            return path;
        }

        bool ContentCaptureReadWholeFile(std::vector<unsigned char>* out,
                                         std::string* error,
                                         const std::string& path,
                                         void* userData)
        {
            auto* capture = static_cast<GLTFContentCaptureContext*>(userData);
            std::filesystem::path resolved;
            std::string captureError;
            if (!capture || !capture->CanAccess(path, resolved, captureError))
            {
                if (error)
                {
                    *error += captureError.empty()
                                  ? "glTF content capture callback has no context."
                                  : captureError;
                }
                return false;
            }
            if (!tinygltf::ReadWholeFile(out, error, resolved.string(), nullptr))
            {
                return false;
            }
            if (!out || !capture->RecordRead(resolved, *out, captureError))
            {
                if (error)
                {
                    *error += captureError.empty()
                                  ? "glTF content capture could not record parser bytes."
                                  : captureError;
                }
                if (out)
                {
                    out->clear();
                }
                return false;
            }
            return true;
        }

        bool ContentCaptureWriteWholeFile(std::string* error,
                                          const std::string&,
                                          const std::vector<unsigned char>&,
                                          void*)
        {
            if (error)
            {
                *error += "glTF loading does not permit filesystem writes.";
            }
            return false;
        }

        bool ContentCaptureGetFileSize(size_t* outSize,
                                       std::string* error,
                                       const std::string& path,
                                       void* userData)
        {
            auto* capture = static_cast<GLTFContentCaptureContext*>(userData);
            std::filesystem::path resolved;
            std::string captureError;
            if (!capture || !capture->CanAccess(path, resolved, captureError))
            {
                if (error)
                {
                    *error += captureError.empty()
                                  ? "glTF content capture callback has no context."
                                  : captureError;
                }
                return false;
            }
            return tinygltf::GetFileSizeInBytes(outSize, error, resolved.string(), nullptr);
        }

        bool ValidateExternalDependenciesWereCaptured(const tinygltf::Model& model,
                                                      std::string& outError)
        {
            for (size_t index = 0; index < model.buffers.size(); ++index)
            {
                const tinygltf::Buffer& buffer = model.buffers[index];
                if (!buffer.uri.empty() && buffer.uri.rfind("data:", 0) != 0 &&
                    buffer.data.empty())
                {
                    outError = "TinyGLTF accepted an external buffer without consuming its bytes: " +
                               buffer.uri;
                    return false;
                }
            }
            for (size_t index = 0; index < model.images.size(); ++index)
            {
                const tinygltf::Image& image = model.images[index];
                if (!image.uri.empty() && image.uri.rfind("data:", 0) != 0 &&
                    image.image.empty())
                {
                    outError = "TinyGLTF accepted an external image without consuming its bytes: " +
                               image.uri;
                    return false;
                }
            }
            return true;
        }

        bool LoadImageDataAllowingCookedArtifact(tinygltf::Image* image,
                                                 const int imageIndex,
                                                 std::string* error,
                                                 std::string* warning,
                                                 int requiredWidth,
                                                 int requiredHeight,
                                                 const unsigned char* bytes,
                                                 int size,
                                                 void* userData)
        {
            if (!image || !bytes || size <= 0)
            {
                if (error)
                {
                    *error += "Encoded image payload is empty for image[" +
                              std::to_string(imageIndex) + "]\n";
                }
                return false;
            }

            (void)requiredWidth;
            (void)requiredHeight;
            (void)warning;

            const auto* capture = static_cast<const ImageCaptureContext*>(userData);
            const std::string source = image->uri.empty()
                                           ? (capture ? capture->modelPath : std::string{}) +
                                                 "#image_" + std::to_string(imageIndex)
                                           : image->uri;
            Diagnostics::TraceSpan encodedSpan = Diagnostics::BeginTraceSpan(
                capture ? capture->traceContext : Diagnostics::TraceContext{},
                "TextureEncodedRead",
                {{"path", source},
                 {"imageIndex", static_cast<uint64>(imageIndex)},
                 {"bytes", static_cast<uint64>(size)},
                 {"capturedBy", "tinygltf-image-callback"},
                 {"ioIncludedByParent", true}});

            // Source import captures encoded bytes only. Decode is deliberately
            // deferred until the minimum-resident model has reached Render.
            image->width = -1;
            image->height = -1;
            image->component = -1;
            image->bits = -1;
            image->pixel_type = -1;
            image->as_is = true;
            image->image.assign(bytes, bytes + size);
            encodedSpan.SetAttribute(
                "cookedArtifact",
                !image->uri.empty() &&
                    HasCookedTextureArtifactExtension(image->uri));
            encodedSpan.SetAttribute("result", "captured");
            return true;
        }

        TextureFallbackSemantic GetFallbackSemantic(
            TextureUsage usage,
            const char* slotName)
        {
            if (usage == TextureUsage::Normal)
                return TextureFallbackSemantic::FlatNormal;
            if (slotName != nullptr && std::strcmp(slotName, "emissive") == 0)
                return TextureFallbackSemantic::Black;
            return TextureFallbackSemantic::White;
        }

        void MarkTextureReferenceUsage(const tinygltf::Model& gltf,
                                       int textureIndex,
                                       TextureUsage usage,
                                       bool isSRGB,
                                       const char* slotName,
                                       GLTFImportResult& result,
                                       std::vector<bool>& assigned)
        {
            const int imageIndex = ResolveTextureImageIndex(gltf, textureIndex);
            if (imageIndex < 0 || imageIndex >= static_cast<int>(result.textures.size()))
            {
                return;
            }

            TextureReference& ref = result.textures[imageIndex];
            if (!assigned[static_cast<size_t>(imageIndex)])
            {
                ref.usage = usage;
                ref.isSRGB = isSRGB;
                ref.fallbackSemantic = GetFallbackSemantic(usage, slotName);
                assigned[static_cast<size_t>(imageIndex)] = true;
                return;
            }

            if (ref.usage == usage && ref.isSRGB == isSRGB)
            {
                return;
            }

            const TextureUsage previousUsage = ref.usage;
            const bool previousSRGB = ref.isSRGB;
            const uint32_t previousRank = GetTextureUsageRank(previousUsage);
            const uint32_t desiredRank = GetTextureUsageRank(usage);

            if (desiredRank > previousRank)
            {
                ref.usage = usage;
                ref.isSRGB = isSRGB;
                ref.fallbackSemantic = GetFallbackSemantic(usage, slotName);
            }

            RVX_CORE_WARN(
                "GLTFImporter: image {} is referenced by incompatible PBR texture slots; "
                "previous usage={} sRGB={}, new slot={} usage={} sRGB={}, resolved usage={} sRGB={}",
                imageIndex,
                TextureUsageName(previousUsage),
                previousSRGB ? "true" : "false",
                slotName ? slotName : "<unknown>",
                TextureUsageName(usage),
                isSRGB ? "true" : "false",
                TextureUsageName(ref.usage),
                ref.isSRGB ? "true" : "false");
        }

        void MarkMaterialTextureUsages(const tinygltf::Model& gltf,
                                       const tinygltf::Material& material,
                                       GLTFImportResult& result,
                                       std::vector<bool>& assigned)
        {
            const auto& pbr = material.pbrMetallicRoughness;
            MarkTextureReferenceUsage(gltf,
                                      pbr.baseColorTexture.index,
                                      TextureUsage::Color,
                                      true,
                                      "baseColor",
                                      result,
                                      assigned);
            MarkTextureReferenceUsage(gltf,
                                      pbr.metallicRoughnessTexture.index,
                                      TextureUsage::Data,
                                      false,
                                      "metallicRoughness",
                                      result,
                                      assigned);
            MarkTextureReferenceUsage(gltf,
                                      material.normalTexture.index,
                                      TextureUsage::Normal,
                                      false,
                                      "normal",
                                      result,
                                      assigned);
            MarkTextureReferenceUsage(gltf,
                                      material.occlusionTexture.index,
                                      TextureUsage::Data,
                                      false,
                                      "occlusion",
                                      result,
                                      assigned);
            MarkTextureReferenceUsage(gltf,
                                      material.emissiveTexture.index,
                                      TextureUsage::Color,
                                      true,
                                      "emissive",
                                      result,
                                      assigned);
        }

        struct AccessorView
        {
            const uint8* bytes = nullptr;
            size_t count = 0;
            size_t stride = 0;
            size_t componentSize = 0;
            size_t componentCount = 0;
            int componentType = -1;
            int valueType = -1;
            bool normalized = false;
        };

        bool ReadAccessorView(const tinygltf::Model& gltf,
                              int accessorIndex,
                              AccessorView& outView,
                              std::string& outError)
        {
            outView = {};
            if (accessorIndex < 0 ||
                accessorIndex >= static_cast<int>(gltf.accessors.size()))
            {
                outError = "glTF accessor index is out of range: " +
                           std::to_string(accessorIndex);
                return false;
            }

            const tinygltf::Accessor& accessor = gltf.accessors[accessorIndex];
            if (accessor.sparse.isSparse)
            {
                outError = "Sparse glTF accessors are not supported by this importer slice.";
                return false;
            }
            if (accessor.bufferView < 0 ||
                accessor.bufferView >= static_cast<int>(gltf.bufferViews.size()))
            {
                outError = "glTF accessor has no valid buffer view.";
                return false;
            }

            const tinygltf::BufferView& bufferView =
                gltf.bufferViews[accessor.bufferView];
            if (bufferView.buffer < 0 ||
                bufferView.buffer >= static_cast<int>(gltf.buffers.size()))
            {
                outError = "glTF buffer view references an invalid buffer.";
                return false;
            }

            const int componentSize =
                tinygltf::GetComponentSizeInBytes(accessor.componentType);
            const int componentCount =
                tinygltf::GetNumComponentsInType(accessor.type);
            const int byteStride = accessor.ByteStride(bufferView);
            if (componentSize <= 0 || componentCount <= 0 || byteStride <= 0)
            {
                outError = "glTF accessor has an unsupported component or value type.";
                return false;
            }

            const size_t elementSize = static_cast<size_t>(componentSize) *
                                       static_cast<size_t>(componentCount);
            const size_t stride = static_cast<size_t>(byteStride);
            if (stride < elementSize)
            {
                outError = "glTF accessor stride is smaller than its element size.";
                return false;
            }
            if (bufferView.byteOffset >
                std::numeric_limits<size_t>::max() - accessor.byteOffset)
            {
                outError = "glTF accessor byte offset overflows size_t.";
                return false;
            }
            const size_t start = bufferView.byteOffset + accessor.byteOffset;
            size_t occupiedBytes = 0;
            if (accessor.count > 0)
            {
                if ((accessor.count - 1) >
                    (std::numeric_limits<size_t>::max() - elementSize) / stride)
                {
                    outError = "glTF accessor byte range overflows size_t.";
                    return false;
                }
                occupiedBytes = (accessor.count - 1) * stride + elementSize;
            }
            if (accessor.byteOffset > bufferView.byteLength ||
                occupiedBytes > bufferView.byteLength - accessor.byteOffset)
            {
                outError = "glTF accessor byte range exceeds its buffer view.";
                return false;
            }

            const tinygltf::Buffer& buffer = gltf.buffers[bufferView.buffer];
            if (start > buffer.data.size() ||
                occupiedBytes > buffer.data.size() - start)
            {
                outError = "glTF accessor byte range exceeds its buffer payload.";
                return false;
            }

            outView.bytes = buffer.data.data() + start;
            outView.count = accessor.count;
            outView.stride = stride;
            outView.componentSize = static_cast<size_t>(componentSize);
            outView.componentCount = static_cast<size_t>(componentCount);
            outView.componentType = accessor.componentType;
            outView.valueType = accessor.type;
            outView.normalized = accessor.normalized;
            return true;
        }

        template <typename T>
        T ReadUnaligned(const uint8* bytes)
        {
            T value{};
            std::memcpy(&value, bytes, sizeof(T));
            return value;
        }

        bool IsFinite(const Vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z);
        }

        bool IsFinite(const Vec4& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z) && std::isfinite(value.w);
        }

        bool IsFinite(const Quat& value)
        {
            return std::isfinite(value.w) && std::isfinite(value.x) &&
                   std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool IsFinite(const Mat4& value)
        {
            for (int column = 0; column < 4; ++column)
            {
                for (int row = 0; row < 4; ++row)
                {
                    if (!std::isfinite(value[column][row]))
                        return false;
                }
            }
            return true;
        }

        bool ReadFloatScalarAccessor(const tinygltf::Model& gltf,
                                     int accessorIndex,
                                     std::vector<float>& outValues,
                                     std::string& outError)
        {
            AccessorView view;
            if (!ReadAccessorView(gltf, accessorIndex, view, outError))
                return false;
            if (view.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                view.valueType != TINYGLTF_TYPE_SCALAR || view.componentCount != 1)
            {
                outError = "Animation time accessor must be FLOAT SCALAR.";
                return false;
            }
            outValues.resize(view.count);
            for (size_t index = 0; index < view.count; ++index)
            {
                outValues[index] = ReadUnaligned<float>(view.bytes + index * view.stride);
                if (!std::isfinite(outValues[index]))
                {
                    outError = "Animation time accessor contains a non-finite value.";
                    return false;
                }
            }
            return true;
        }

        template <size_t ComponentCount>
        bool ReadFloatVectorAccessor(const tinygltf::Model& gltf,
                                     int accessorIndex,
                                     std::vector<std::array<float, ComponentCount>>& outValues,
                                     std::string& outError)
        {
            AccessorView view;
            if (!ReadAccessorView(gltf, accessorIndex, view, outError))
                return false;
            const int expectedType = ComponentCount == 3 ? TINYGLTF_TYPE_VEC3
                                                         : TINYGLTF_TYPE_VEC4;
            if (view.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                view.valueType != expectedType ||
                view.componentCount != ComponentCount)
            {
                outError = "Animation output accessor has an unexpected FLOAT vector type.";
                return false;
            }
            outValues.resize(view.count);
            for (size_t index = 0; index < view.count; ++index)
            {
                const uint8* element = view.bytes + index * view.stride;
                for (size_t component = 0; component < ComponentCount; ++component)
                {
                    const float value = ReadUnaligned<float>(
                        element + component * sizeof(float));
                    if (!std::isfinite(value))
                    {
                        outError = "Animation output accessor contains a non-finite value.";
                        return false;
                    }
                    outValues[index][component] = value;
                }
            }
            return true;
        }

        bool ReadMatrixAccessor(const tinygltf::Model& gltf,
                                int accessorIndex,
                                std::vector<Mat4>& outValues,
                                std::string& outError)
        {
            AccessorView view;
            if (!ReadAccessorView(gltf, accessorIndex, view, outError))
                return false;
            if (view.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                view.valueType != TINYGLTF_TYPE_MAT4 || view.componentCount != 16)
            {
                outError = "Inverse bind matrices accessor must be FLOAT MAT4.";
                return false;
            }
            outValues.resize(view.count);
            for (size_t index = 0; index < view.count; ++index)
            {
                Mat4 matrix{1.0f};
                const uint8* element = view.bytes + index * view.stride;
                for (size_t component = 0; component < 16; ++component)
                {
                    matrix[component / 4][component % 4] =
                        ReadUnaligned<float>(element + component * sizeof(float));
                }
                if (!IsFinite(matrix))
                {
                    outError = "Inverse bind matrices accessor contains a non-finite matrix.";
                    return false;
                }
                outValues[index] = matrix;
            }
            return true;
        }

        bool BuildNodeLocalMatrix(const tinygltf::Node& node,
                                  float scaleFactor,
                                  Mat4& outMatrix,
                                  std::string& outError)
        {
            if (!node.matrix.empty())
            {
                if (node.matrix.size() != 16)
                {
                    outError = "glTF node matrix must contain exactly 16 values.";
                    return false;
                }
                outMatrix = Mat4(1.0f);
                for (size_t component = 0; component < 16; ++component)
                {
                    const double value = node.matrix[component];
                    if (!std::isfinite(value))
                    {
                        outError = "glTF node matrix contains a non-finite value.";
                        return false;
                    }
                    outMatrix[component / 4][component % 4] =
                        static_cast<float>(value);
                }
                outMatrix[3][0] *= scaleFactor;
                outMatrix[3][1] *= scaleFactor;
                outMatrix[3][2] *= scaleFactor;
                return true;
            }

            if ((!node.translation.empty() && node.translation.size() != 3) ||
                (!node.rotation.empty() && node.rotation.size() != 4) ||
                (!node.scale.empty() && node.scale.size() != 3))
            {
                outError = "glTF node TRS arrays have invalid component counts.";
                return false;
            }

            Vec3 translation{0.0f};
            Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
            Vec3 scale{1.0f};
            if (!node.translation.empty())
            {
                translation = Vec3(static_cast<float>(node.translation[0]),
                                   static_cast<float>(node.translation[1]),
                                   static_cast<float>(node.translation[2])) *
                              scaleFactor;
            }
            if (!node.rotation.empty())
            {
                rotation = Quat(static_cast<float>(node.rotation[3]),
                                static_cast<float>(node.rotation[0]),
                                static_cast<float>(node.rotation[1]),
                                static_cast<float>(node.rotation[2]));
                const float lengthSquared = glm::dot(rotation, rotation);
                if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f)
                {
                    outError = "glTF node rotation is not a finite non-zero quaternion.";
                    return false;
                }
                rotation = glm::normalize(rotation);
            }
            if (!node.scale.empty())
            {
                scale = Vec3(static_cast<float>(node.scale[0]),
                             static_cast<float>(node.scale[1]),
                             static_cast<float>(node.scale[2]));
            }
            if (!IsFinite(translation) || !IsFinite(rotation) || !IsFinite(scale))
            {
                outError = "glTF node TRS contains a non-finite value.";
                return false;
            }

            outMatrix = glm::translate(Mat4(1.0f), translation) *
                        glm::mat4_cast(rotation);
            outMatrix = glm::scale(outMatrix, scale);
            return true;
        }

        std::string CanonicalNodeSegment(const tinygltf::Node& node,
                                         size_t nodeIndex)
        {
            const std::string source = node.name.empty()
                                           ? "Node_" + std::to_string(nodeIndex)
                                           : node.name;
            std::string encoded;
            encoded.reserve(source.size());
            constexpr char hex[] = "0123456789ABCDEF";
            for (const unsigned char value : source)
            {
                if (value == '%' || value == '/' || value == '#')
                {
                    encoded.push_back('%');
                    encoded.push_back(hex[value >> 4]);
                    encoded.push_back(hex[value & 0x0f]);
                }
                else
                {
                    encoded.push_back(static_cast<char>(value));
                }
            }
            return encoded;
        }

        void DiscardImportedPayload(GLTFImportResult& result)
        {
            result.model.reset();
            result.meshes.clear();
            result.materials.clear();
            result.textures.clear();
            result.skeleton.reset();
            result.animationClips.clear();
        }
    } // namespace

    // =========================================================================
    // Public Interface
    // =========================================================================

    bool GLTFImporter::CanImport(const std::string& path) const
    {
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".gltf" || ext == ".glb";
    }

    GLTFImportResult GLTFImporter::Import(
        const std::string& path,
        const GLTFImportOptions& options,
        const Diagnostics::TraceContext& traceContext)
    {
        Diagnostics::TraceSpan parseSpan = Diagnostics::BeginTraceSpan(
            traceContext,
            "GltfParse",
            {{"path", path},
             {"parser", "tinygltf"}});
        GLTFImportResult result;
        m_currentFilePath = path;
        m_parsedNodes.clear();
        m_skinJointRemap.clear();
        m_nodeCanonicalPaths.clear();
        m_jointNodes.clear();

        if (!std::isfinite(options.scaleFactor) || options.scaleFactor <= 0.0f)
        {
            result.errorMessage = "glTF import scale factor must be finite and greater than zero.";
            parseSpan.SetAttribute("result", "invalid-options");
            return result;
        }

        ReportProgress(0.0f, "Loading file");

        // Load the glTF file
        tinygltf::Model gltfModel;
        std::string error, warning;
        
        if (!LoadFile(path,
                      gltfModel,
                      error,
                      warning,
                      result.observedContentIdentity,
                      parseSpan.GetChildContext()))
        {
            result.success = false;
            result.errorMessage = error;
            parseSpan.SetAttribute("result", "failed");
            return result;
        }

        if (!warning.empty())
        {
            result.warnings.push_back(warning);
        }

        result.hasSkins = !gltfModel.skins.empty();
        result.hasAnimations = !gltfModel.animations.empty();
        result.extensionsUsed = gltfModel.extensionsUsed;
        result.extensionsRequired = gltfModel.extensionsRequired;
        for (const tinygltf::Mesh& mesh : gltfModel.meshes)
        {
            for (const tinygltf::Primitive& primitive : mesh.primitives)
            {
                if (!primitive.targets.empty())
                {
                    result.hasMorphTargets = true;
                    break;
                }
            }
            if (result.hasMorphTargets)
                break;
        }

        ReportProgress(0.1f, "Extracting textures");
        ExtractTextures(gltfModel, path, result);

        ReportProgress(0.3f, "Parsing materials");
        ParseMaterials(gltfModel, result);

        ReportProgress(0.45f, "Parsing skeleton and animation clips");
        if (!ParseSkeletalData(gltfModel, result, options, error))
        {
            result.success = false;
            result.errorMessage = error;
            DiscardImportedPayload(result);
            parseSpan.SetAttribute("result", "skeletal-data-invalid");
            return result;
        }

        ReportProgress(0.55f, "Parsing meshes");
        if (!ParseMeshes(gltfModel, result, options, error))
        {
            result.success = false;
            result.errorMessage = error;
            DiscardImportedPayload(result);
            parseSpan.SetAttribute("result", "mesh-data-invalid");
            return result;
        }

        ReportProgress(0.75f, "Building scene graph");
        if (!ParseNodes(gltfModel, result, options, error))
        {
            result.success = false;
            result.errorMessage = error;
            DiscardImportedPayload(result);
            parseSpan.SetAttribute("result", "scene-graph-invalid");
            return result;
        }
        if (!ParseScene(gltfModel, result, error))
        {
            result.success = false;
            result.errorMessage = error;
            DiscardImportedPayload(result);
            parseSpan.SetAttribute("result", "scene-invalid");
            return result;
        }

        ReportProgress(0.9f, "Computing bounds");
        if (result.model)
        {
            result.model->path = path;
            std::filesystem::path filePath(path);
            result.model->suffix = filePath.extension().string();
        }
        if (!result.model || !result.model->ComputeBoundingBox(result.meshes))
        {
            result.success = false;
            result.errorMessage =
                "glTF indexed model bounds require complete valid mesh references.";
            DiscardImportedPayload(result);
            parseSpan.SetAttribute("result", "indexed-bounds-invalid");
            return result;
        }

        ReportProgress(1.0f, "Complete");
        result.success = true;
        parseSpan.SetAttribute("result", "loaded");
        parseSpan.SetAttribute("meshCount", static_cast<uint64>(result.meshes.size()));
        parseSpan.SetAttribute("materialCount", static_cast<uint64>(result.materials.size()));
        parseSpan.SetAttribute("textureCount", static_cast<uint64>(result.textures.size()));
        parseSpan.SetAttribute(
            "boneCount",
            result.skeleton ? static_cast<uint64>(result.skeleton->GetBoneCount()) : 0u);
        parseSpan.SetAttribute("animationClipCount",
                               static_cast<uint64>(result.animationClips.size()));
        return result;
    }

    // =========================================================================
    // File Loading
    // =========================================================================

    bool GLTFImporter::LoadFile(const std::string& path,
                                tinygltf::Model& gltfModel,
                                std::string& error,
                                std::string& warning,
                                ResourceContentIdentity& outObservedContentIdentity,
                                const Diagnostics::TraceContext& traceContext)
    {
        // TinyGLTF owns both the source document read and all referenced
        // buffer/image loads inside this call.  Its API does not expose those
        // sub-operations, so this is deliberately reported as one composite
        // observed boundary rather than fabricated per-file timings.
        Diagnostics::TraceSpan bufferReadSpan = Diagnostics::BeginTraceSpan(
            traceContext,
            "BufferRead",
            {{"path", path},
             {"reader", "tinygltf"},
             {"composite", true},
             {"includesReferencedBuffers", true},
             {"includesImageLoadCallbacks", true}});
        outObservedContentIdentity = {};
        GLTFContentCaptureContext contentCapture{path};
        if (!contentCapture.IsReady())
        {
            error = contentCapture.GetFailure();
            bufferReadSpan.SetAttribute("result", "content-capture-init-failed");
            return false;
        }

        tinygltf::TinyGLTF loader;
        tinygltf::FsCallbacks callbacks{};
        callbacks.FileExists = ContentCaptureFileExists;
        callbacks.ExpandFilePath = ContentCaptureExpandFilePath;
        callbacks.ReadWholeFile = ContentCaptureReadWholeFile;
        callbacks.WriteWholeFile = ContentCaptureWriteWholeFile;
        callbacks.GetFileSizeInBytes = ContentCaptureGetFileSize;
        callbacks.user_data = &contentCapture;
        std::string callbackError;
        if (!loader.SetFsCallbacks(std::move(callbacks), &callbackError))
        {
            error = callbackError.empty()
                        ? "TinyGLTF rejected content-capture filesystem callbacks."
                        : callbackError;
            bufferReadSpan.SetAttribute("result", "content-capture-callback-failed");
            return false;
        }
        ImageCaptureContext imageCapture{traceContext, path};
        loader.SetImageLoader(LoadImageDataAllowingCookedArtifact,
                              &imageCapture);

        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool success = false;
        if (ext == ".glb")
        {
            success = loader.LoadBinaryFromFile(&gltfModel, &error, &warning, path);
        }
        else
        {
            success = loader.LoadASCIIFromFile(&gltfModel, &error, &warning, path);
        }

        if (success)
        {
            if (!ValidateExternalDependenciesWereCaptured(gltfModel, error))
            {
                success = false;
            }
            else
            {
                outObservedContentIdentity = contentCapture.BuildIdentity();
                if (!outObservedContentIdentity.IsValid())
                {
                    error = "TinyGLTF completed parsing without a valid consumed-byte identity.";
                    success = false;
                }
            }
        }
        bufferReadSpan.SetAttribute("result", success ? "loaded" : "failed");
        return success;
    }

    // =========================================================================
    // Texture Extraction
    // =========================================================================

    void GLTFImporter::ExtractTextures(tinygltf::Model& gltf, const std::string& basePath,
                                        GLTFImportResult& result)
    {
        (void)basePath;
        result.textures.resize(gltf.images.size());

        for (size_t i = 0; i < gltf.images.size(); ++i)
        {
            auto& image = gltf.images[i];
            TextureReference& ref = result.textures[i];
            ref.imageIndex = static_cast<int>(i);

            if (!image.uri.empty() && image.uri.find("data:") != 0)
            {
                // External texture file
                ref.sourceType = TextureSourceType::External;
                ref.path = image.uri;
                ref.mimeType = image.mimeType;
                ref.capturedPayload =
                    std::make_shared<const std::vector<uint8_t>>(
                        std::move(image.image));
            }
            else
            {
                // Embedded texture (in GLB or base64)
                ref.sourceType = TextureSourceType::Embedded;
                ref.mimeType = image.mimeType;
                
                if (!image.image.empty())
                {
                    // The custom tinygltf callback preserves encoded PNG/JPEG
                    // bytes. Move them into the streaming descriptor so the
                    // source is never opened by TextureLoader a second time.
                    ref.capturedPayload =
                        std::make_shared<const std::vector<uint8_t>>(
                            std::move(image.image));
                    ref.isRawPixelData = false;
                }
                else if (image.bufferView >= 0)
                {
                    // Need to extract from buffer - still encoded (PNG/JPEG)
                    const auto& bufferView = gltf.bufferViews[image.bufferView];
                    const auto& buffer = gltf.buffers[bufferView.buffer];
                    ref.embeddedData.assign(
                        buffer.data.begin() + bufferView.byteOffset,
                        buffer.data.begin() + bufferView.byteOffset + bufferView.byteLength
                    );
                    ref.isRawPixelData = false;
                }
            }
        }
    }

    // =========================================================================
    // Material Parsing
    // =========================================================================

    void GLTFImporter::ParseMaterials(const tinygltf::Model& gltf, GLTFImportResult& result)
    {
        result.materials.reserve(gltf.materials.size());
        std::vector<bool> textureUsageAssigned(result.textures.size(), false);

        for (size_t i = 0; i < gltf.materials.size(); ++i)
        {
            const tinygltf::Material& material = gltf.materials[i];
            result.materials.push_back(ConvertMaterial(gltf, material, static_cast<int>(i)));
            MarkMaterialTextureUsages(gltf, material, result, textureUsageAssigned);
        }

        // Add a default material if none exist
        if (result.materials.empty())
        {
            auto defaultMat = std::make_shared<Material>("DefaultMaterial");
            defaultMat->SetMaterialId(0);
            result.materials.push_back(defaultMat);
        }
    }

    Material::Ptr GLTFImporter::ConvertMaterial(const tinygltf::Model& gltf,
                                                  const tinygltf::Material& mat,
                                                  int materialIndex)
    {
        auto material = std::make_shared<Material>(mat.name.empty() ? "Material_" + std::to_string(materialIndex) : mat.name);
        material->SetMaterialId(static_cast<uint32_t>(materialIndex));

        // PBR Metallic-Roughness
        const auto& pbr = mat.pbrMetallicRoughness;
        
        // Base color
        material->SetBaseColor(
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3])
        );

        if (pbr.baseColorTexture.index >= 0)
        {
            material->SetBaseColorTexture(ExtractTextureInfo(gltf, pbr.baseColorTexture.index, pbr.baseColorTexture.texCoord));
        }

        // Metallic and roughness
        material->SetMetallicFactor(static_cast<float>(pbr.metallicFactor));
        material->SetRoughnessFactor(static_cast<float>(pbr.roughnessFactor));

        if (pbr.metallicRoughnessTexture.index >= 0)
        {
            material->SetMetallicRoughnessTexture(ExtractTextureInfo(gltf, pbr.metallicRoughnessTexture.index, pbr.metallicRoughnessTexture.texCoord));
        }

        // Normal map
        if (mat.normalTexture.index >= 0)
        {
            material->SetNormalTexture(ExtractTextureInfo(gltf, mat.normalTexture.index, mat.normalTexture.texCoord));
            material->SetNormalScale(static_cast<float>(mat.normalTexture.scale));
        }

        // Occlusion
        if (mat.occlusionTexture.index >= 0)
        {
            material->SetOcclusionTexture(ExtractTextureInfo(gltf, mat.occlusionTexture.index, mat.occlusionTexture.texCoord));
            material->SetOcclusionStrength(static_cast<float>(mat.occlusionTexture.strength));
        }

        // Emissive
        material->SetEmissiveColor(Vec3(
            static_cast<float>(mat.emissiveFactor[0]),
            static_cast<float>(mat.emissiveFactor[1]),
            static_cast<float>(mat.emissiveFactor[2])
        ));

        if (mat.emissiveTexture.index >= 0)
        {
            material->SetEmissiveTexture(ExtractTextureInfo(gltf, mat.emissiveTexture.index, mat.emissiveTexture.texCoord));
        }

        // Alpha mode
        if (mat.alphaMode == "OPAQUE")
        {
            material->SetAlphaMode(Material::AlphaMode::Opaque);
        }
        else if (mat.alphaMode == "MASK")
        {
            material->SetAlphaMode(Material::AlphaMode::Mask);
            material->SetAlphaCutoff(static_cast<float>(mat.alphaCutoff));
        }
        else if (mat.alphaMode == "BLEND")
        {
            material->SetAlphaMode(Material::AlphaMode::Blend);
        }

        material->SetDoubleSided(mat.doubleSided);

        return material;
    }

    TextureInfo GLTFImporter::ExtractTextureInfo(const tinygltf::Model& gltf, int textureIndex, int uvSet)
    {
        TextureInfo info;
        info.uvSet = uvSet;

        if (textureIndex < 0 || textureIndex >= static_cast<int>(gltf.textures.size()))
        {
            return info;
        }

        const auto& texture = gltf.textures[textureIndex];
        info.imageId = texture.source;

        // Get the image path if it's an external file
        if (texture.source >= 0 && texture.source < static_cast<int>(gltf.images.size()))
        {
            const auto& image = gltf.images[texture.source];
            if (!image.uri.empty() && image.uri.find("data:") != 0)
            {
                info.texturePath = image.uri;
            }
        }

        // Sampler settings
        if (texture.sampler >= 0 && texture.sampler < static_cast<int>(gltf.samplers.size()))
        {
            const auto& sampler = gltf.samplers[texture.sampler];

            // Wrap mode
            auto convertWrap = [](int mode) -> TextureInfo::WrapMode {
                switch (mode)
                {
                    case TINYGLTF_TEXTURE_WRAP_REPEAT: return TextureInfo::WrapMode::Repeat;
                    case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE: return TextureInfo::WrapMode::ClampToEdge;
                    case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT: return TextureInfo::WrapMode::MirrorRepeat;
                    default: return TextureInfo::WrapMode::Repeat;
                }
            };

            info.wrapS = convertWrap(sampler.wrapS);
            info.wrapT = convertWrap(sampler.wrapT);

            // Filter mode
            auto convertMinFilter = [](int mode) -> TextureInfo::FilterMode {
                switch (mode)
                {
                    case TINYGLTF_TEXTURE_FILTER_NEAREST: return TextureInfo::FilterMode::Nearest;
                    case TINYGLTF_TEXTURE_FILTER_LINEAR: return TextureInfo::FilterMode::Linear;
                    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST: return TextureInfo::FilterMode::NearestMipmapNearest;
                    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST: return TextureInfo::FilterMode::LinearMipmapNearest;
                    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR: return TextureInfo::FilterMode::NearestMipmapLinear;
                    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR: return TextureInfo::FilterMode::LinearMipmapLinear;
                    default: return TextureInfo::FilterMode::LinearMipmapLinear;
                }
            };

            auto convertMagFilter = [](int mode) -> TextureInfo::FilterMode {
                switch (mode)
                {
                    case TINYGLTF_TEXTURE_FILTER_NEAREST: return TextureInfo::FilterMode::Nearest;
                    case TINYGLTF_TEXTURE_FILTER_LINEAR: return TextureInfo::FilterMode::Linear;
                    default: return TextureInfo::FilterMode::Linear;
                }
            };

            info.minFilter = convertMinFilter(sampler.minFilter);
            info.magFilter = convertMagFilter(sampler.magFilter);
        }

        return info;
    }

    // =========================================================================
    // Skeleton and Animation Parsing
    // =========================================================================

    bool GLTFImporter::ParseSkeletalData(const tinygltf::Model& gltf,
                                         GLTFImportResult& result,
                                         const GLTFImportOptions& options,
                                         std::string& error)
    {
        const size_t nodeCount = gltf.nodes.size();
        std::vector<int> parents(nodeCount, -1);
        for (size_t parentIndex = 0; parentIndex < nodeCount; ++parentIndex)
        {
            for (const int childIndex : gltf.nodes[parentIndex].children)
            {
                if (childIndex < 0 || childIndex >= static_cast<int>(nodeCount) ||
                    childIndex == static_cast<int>(parentIndex))
                {
                    error = "glTF node hierarchy contains an invalid child index.";
                    return false;
                }
                if (parents[static_cast<size_t>(childIndex)] >= 0)
                {
                    error = "glTF node hierarchy gives a node more than one parent.";
                    return false;
                }
                parents[static_cast<size_t>(childIndex)] =
                    static_cast<int>(parentIndex);
            }
        }

        std::vector<uint8> visitState(nodeCount, 0);
        std::function<bool(size_t)> visit = [&](size_t nodeIndex)
        {
            if (visitState[nodeIndex] == 1)
            {
                error = "glTF node hierarchy contains a cycle.";
                return false;
            }
            if (visitState[nodeIndex] == 2)
                return true;
            visitState[nodeIndex] = 1;
            for (const int childIndex : gltf.nodes[nodeIndex].children)
            {
                if (!visit(static_cast<size_t>(childIndex)))
                    return false;
            }
            visitState[nodeIndex] = 2;
            return true;
        };
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            if (!visit(nodeIndex))
                return false;
        }

        std::vector<std::string> segments(nodeCount);
        std::map<std::pair<int, std::string>, size_t> siblingNameCounts;
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            segments[nodeIndex] = CanonicalNodeSegment(gltf.nodes[nodeIndex], nodeIndex);
            ++siblingNameCounts[{parents[nodeIndex], segments[nodeIndex]}];
        }
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            if (siblingNameCounts[{parents[nodeIndex], segments[nodeIndex]}] > 1)
                segments[nodeIndex] += "#" + std::to_string(nodeIndex);
        }

        m_nodeCanonicalPaths.assign(nodeCount, {});
        std::function<const std::string&(size_t)> buildPath = [&](size_t nodeIndex)
            -> const std::string&
        {
            std::string& path = m_nodeCanonicalPaths[nodeIndex];
            if (!path.empty())
                return path;
            const int parentIndex = parents[nodeIndex];
            path = parentIndex >= 0
                       ? buildPath(static_cast<size_t>(parentIndex)) + "/" +
                             segments[nodeIndex]
                       : segments[nodeIndex];
            return path;
        };
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
            buildPath(nodeIndex);

        std::vector<Mat4> localMatrices(nodeCount, Mat4(1.0f));
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            if (!BuildNodeLocalMatrix(gltf.nodes[nodeIndex],
                                      options.scaleFactor,
                                      localMatrices[nodeIndex],
                                      error))
            {
                error = "Node " + std::to_string(nodeIndex) + ": " + error;
                return false;
            }
        }

        std::vector<Mat4> globalMatrices(nodeCount, Mat4(1.0f));
        std::vector<bool> globalReady(nodeCount, false);
        std::function<const Mat4&(size_t)> buildGlobal = [&](size_t nodeIndex)
            -> const Mat4&
        {
            if (!globalReady[nodeIndex])
            {
                const int parentIndex = parents[nodeIndex];
                globalMatrices[nodeIndex] = parentIndex >= 0
                    ? buildGlobal(static_cast<size_t>(parentIndex)) *
                          localMatrices[nodeIndex]
                    : localMatrices[nodeIndex];
                globalReady[nodeIndex] = true;
            }
            return globalMatrices[nodeIndex];
        };
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            if (!IsFinite(buildGlobal(nodeIndex)))
            {
                error = "glTF node hierarchy produces a non-finite global transform.";
                return false;
            }
        }

        if (gltf.skins.size() > 1)
        {
            error = "This skeletal importer slice supports exactly one glTF skin per model.";
            return false;
        }
        if (!gltf.skins.empty() && std::abs(options.scaleFactor - 1.0f) > 1.0e-6f)
        {
            error = "Non-unit import scaling for skinned glTF assets is unsupported until inverse-bind rescaling is defined.";
            return false;
        }
        std::vector<bool> meshHasSkinReference(gltf.meshes.size(), false);
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            const tinygltf::Node& node = gltf.nodes[nodeIndex];
            if (node.skin < -1 || node.skin >= static_cast<int>(gltf.skins.size()))
            {
                error = "glTF node references an invalid skin index.";
                return false;
            }
            if (node.skin >= 0 && node.mesh < 0)
            {
                error = "glTF node references a skin without a mesh.";
                return false;
            }
            if (node.mesh >= static_cast<int>(gltf.meshes.size()))
            {
                error = "glTF node references an invalid mesh index.";
                return false;
            }
            if (node.skin >= 0)
                meshHasSkinReference[static_cast<size_t>(node.mesh)] = true;
        }
        for (size_t meshIndex = 0; meshIndex < gltf.meshes.size(); ++meshIndex)
        {
            for (const tinygltf::Primitive& primitive :
                 gltf.meshes[meshIndex].primitives)
            {
                const bool hasJoints = primitive.attributes.contains("JOINTS_0");
                const bool hasWeights = primitive.attributes.contains("WEIGHTS_0");
                if (hasJoints != hasWeights)
                {
                    error = "glTF primitive must provide JOINTS_0 and WEIGHTS_0 together.";
                    return false;
                }
                if (meshHasSkinReference[meshIndex] != hasJoints)
                {
                    error = meshHasSkinReference[meshIndex]
                        ? "A skinned glTF mesh primitive lacks JOINTS_0/WEIGHTS_0."
                        : "A glTF mesh has skinning attributes but no node binds its skin.";
                    return false;
                }
            }
        }

        m_jointNodes.assign(nodeCount, false);
        std::vector<int> jointNodeToSourceOrdinal(nodeCount, -1);
        if (!gltf.skins.empty())
        {
            const tinygltf::Skin& skin = gltf.skins.front();
            if (skin.joints.empty())
            {
                error = "glTF skin does not contain any joints.";
                return false;
            }
            if (skin.skeleton >= static_cast<int>(nodeCount) || skin.skeleton < -1)
            {
                error = "glTF skin references an invalid skeleton root node.";
                return false;
            }

            for (size_t sourceOrdinal = 0;
                 sourceOrdinal < skin.joints.size();
                 ++sourceOrdinal)
            {
                const int nodeIndex = skin.joints[sourceOrdinal];
                if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodeCount))
                {
                    error = "glTF skin joint index is out of range.";
                    return false;
                }
                if (jointNodeToSourceOrdinal[static_cast<size_t>(nodeIndex)] >= 0)
                {
                    error = "glTF skin contains a duplicate joint node.";
                    return false;
                }
                jointNodeToSourceOrdinal[static_cast<size_t>(nodeIndex)] =
                    static_cast<int>(sourceOrdinal);
                m_jointNodes[static_cast<size_t>(nodeIndex)] = true;
            }

            std::vector<size_t> sourceOrder(skin.joints.size());
            for (size_t ordinal = 0; ordinal < sourceOrder.size(); ++ordinal)
                sourceOrder[ordinal] = ordinal;
            auto nodeDepth = [&](int nodeIndex)
            {
                int depth = 0;
                while (nodeIndex >= 0)
                {
                    ++depth;
                    nodeIndex = parents[static_cast<size_t>(nodeIndex)];
                }
                return depth;
            };
            std::sort(sourceOrder.begin(),
                      sourceOrder.end(),
                      [&](size_t lhs, size_t rhs)
                      {
                          const int lhsNode = skin.joints[lhs];
                          const int rhsNode = skin.joints[rhs];
                          const int lhsDepth = nodeDepth(lhsNode);
                          const int rhsDepth = nodeDepth(rhsNode);
                          return lhsDepth != rhsDepth ? lhsDepth < rhsDepth
                                                     : lhsNode < rhsNode;
                      });

            std::vector<Mat4> inverseBindMatrices;
            if (skin.inverseBindMatrices >= 0)
            {
                if (!ReadMatrixAccessor(gltf,
                                        skin.inverseBindMatrices,
                                        inverseBindMatrices,
                                        error))
                {
                    return false;
                }
                if (inverseBindMatrices.size() != skin.joints.size())
                {
                    error = "Inverse bind matrix count does not match the skin joint count.";
                    return false;
                }
            }

            auto skeleton = Animation::Skeleton::Create();
            skeleton->Reserve(skin.joints.size());
            m_skinJointRemap.assign(skin.joints.size(), -1);
            for (const size_t sourceOrdinal : sourceOrder)
            {
                const int jointNode = skin.joints[sourceOrdinal];
                int parentNode = parents[static_cast<size_t>(jointNode)];
                int parentBone = -1;
                while (parentNode >= 0)
                {
                    const int parentOrdinal =
                        jointNodeToSourceOrdinal[static_cast<size_t>(parentNode)];
                    if (parentOrdinal >= 0)
                    {
                        parentBone = m_skinJointRemap[static_cast<size_t>(parentOrdinal)];
                        break;
                    }
                    parentNode = parents[static_cast<size_t>(parentNode)];
                }

                Mat4 localBind = globalMatrices[static_cast<size_t>(jointNode)];
                if (parentBone >= 0)
                {
                    const int parentJointNode =
                        skin.joints[sourceOrder[static_cast<size_t>(parentBone)]];
                    const Mat4& parentGlobal =
                        globalMatrices[static_cast<size_t>(parentJointNode)];
                    const float determinant = glm::determinant(parentGlobal);
                    if (!std::isfinite(determinant) ||
                        std::abs(determinant) <= 1.0e-12f)
                    {
                        error = "A parent joint bind matrix is not invertible.";
                        return false;
                    }
                    localBind = glm::inverse(parentGlobal) * localBind;
                }
                if (!IsFinite(localBind))
                {
                    error = "A joint local bind matrix is not finite.";
                    return false;
                }

                Animation::Bone bone;
                bone.name = m_nodeCanonicalPaths[static_cast<size_t>(jointNode)];
                bone.parentIndex = parentBone;
                bone.localBindPose = Animation::TransformSample::FromMatrix(localBind);
                if (!IsFinite(bone.localBindPose.translation) ||
                    !IsFinite(bone.localBindPose.rotation) ||
                    !IsFinite(bone.localBindPose.scale))
                {
                    error = "A joint bind matrix cannot be decomposed into a finite TRS pose.";
                    return false;
                }
                if (!inverseBindMatrices.empty())
                {
                    bone.inverseBindPose = inverseBindMatrices[sourceOrdinal];
                }
                else
                {
                    const Mat4& globalBind =
                        globalMatrices[static_cast<size_t>(jointNode)];
                    const float determinant = glm::determinant(globalBind);
                    if (!std::isfinite(determinant) ||
                        std::abs(determinant) <= 1.0e-12f)
                    {
                        error = "A joint global bind matrix is not invertible.";
                        return false;
                    }
                    bone.inverseBindPose = glm::inverse(globalBind);
                }

                const int boneIndex = skeleton->AddBone(bone);
                m_skinJointRemap[sourceOrdinal] = boneIndex;
            }
            if (!skeleton->Validate())
            {
                error = "Canonical glTF skeleton failed parent/name validation.";
                return false;
            }
            result.skeleton = std::move(skeleton);
        }

        for (size_t animationIndex = 0;
             animationIndex < gltf.animations.size();
             ++animationIndex)
        {
            const tinygltf::Animation& sourceAnimation =
                gltf.animations[animationIndex];
            std::string clipName = sourceAnimation.name.empty()
                                       ? "Animation_" + std::to_string(animationIndex)
                                       : sourceAnimation.name;
            if (result.animationClips.contains(clipName))
                clipName += "#" + std::to_string(animationIndex);
            for (uint32 collision = 1;
                 result.animationClips.contains(clipName);
                 ++collision)
            {
                clipName = sourceAnimation.name + "#" +
                           std::to_string(animationIndex) + "_" +
                           std::to_string(collision);
            }

            auto clip = Animation::AnimationClip::Create(clipName);
            clip->metadata.sourceFormat = Animation::AnimationSourceFormat::glTF;
            clip->metadata.sourceFile = m_currentFilePath;
            clip->skeleton = result.skeleton;

            std::map<std::string, Animation::TransformTrack> tracks;
            std::map<std::string, int> trackNodes;
            for (size_t channelIndex = 0;
                 channelIndex < sourceAnimation.channels.size();
                 ++channelIndex)
            {
                const tinygltf::AnimationChannel& channel =
                    sourceAnimation.channels[channelIndex];
                if (channel.sampler < 0 ||
                    channel.sampler >= static_cast<int>(sourceAnimation.samplers.size()))
                {
                    error = "glTF animation channel references an invalid sampler.";
                    return false;
                }
                if (channel.target_node < 0 ||
                    channel.target_node >= static_cast<int>(nodeCount))
                {
                    error = "glTF animation channel has no valid target node.";
                    return false;
                }
                if (channel.target_path != "translation" &&
                    channel.target_path != "rotation" &&
                    channel.target_path != "scale")
                {
                    error = "Unsupported glTF animation target path: " +
                            channel.target_path;
                    return false;
                }

                const tinygltf::AnimationSampler& sampler =
                    sourceAnimation.samplers[static_cast<size_t>(channel.sampler)];
                Animation::InterpolationMode interpolation;
                if (sampler.interpolation == "STEP")
                    interpolation = Animation::InterpolationMode::Step;
                else if (sampler.interpolation.empty() ||
                         sampler.interpolation == "LINEAR")
                    interpolation = Animation::InterpolationMode::Linear;
                else if (sampler.interpolation == "CUBICSPLINE")
                {
                    error = "glTF CUBICSPLINE animation is unsupported until tangent semantics are implemented.";
                    return false;
                }
                else
                {
                    error = "Unsupported glTF animation interpolation: " +
                            sampler.interpolation;
                    return false;
                }

                std::vector<float> sourceTimes;
                if (!ReadFloatScalarAccessor(gltf,
                                             sampler.input,
                                             sourceTimes,
                                             error) ||
                    sourceTimes.empty())
                {
                    if (error.empty())
                        error = "glTF animation sampler has no input times.";
                    return false;
                }
                std::vector<Animation::TimeUs> times(sourceTimes.size());
                for (size_t keyIndex = 0; keyIndex < sourceTimes.size(); ++keyIndex)
                {
                    const double seconds = static_cast<double>(sourceTimes[keyIndex]);
                    const double micros = seconds * 1'000'000.0;
                    if (seconds < 0.0 || !std::isfinite(micros) ||
                        micros > static_cast<double>(
                            std::numeric_limits<Animation::TimeUs>::max()))
                    {
                        error = "glTF animation time is negative or cannot be represented in microseconds.";
                        return false;
                    }
                    times[keyIndex] = static_cast<Animation::TimeUs>(std::llround(micros));
                    if (keyIndex > 0 && times[keyIndex] <= times[keyIndex - 1])
                    {
                        error = "glTF animation times are not strictly increasing after microsecond quantization.";
                        return false;
                    }
                }

                const size_t targetNode = static_cast<size_t>(channel.target_node);
                const std::string& targetPath = m_nodeCanonicalPaths[targetNode];
                Animation::TransformTrack& track = tracks[targetPath];
                track.targetName = targetPath;
                track.targetType = m_jointNodes[targetNode]
                                       ? Animation::TrackTargetType::Bone
                                       : Animation::TrackTargetType::Node;
                trackNodes[targetPath] = channel.target_node;

                if (channel.target_path == "rotation")
                {
                    if (!track.rotationKeyframes.empty())
                    {
                        error = "glTF animation contains duplicate rotation channels for one target.";
                        return false;
                    }
                    std::vector<std::array<float, 4>> values;
                    if (!ReadFloatVectorAccessor<4>(gltf,
                                                    sampler.output,
                                                    values,
                                                    error) ||
                        values.size() != times.size())
                    {
                        if (error.empty())
                            error = "glTF rotation output count does not match its input times.";
                        return false;
                    }
                    track.rotationKeyframes.reserve(values.size());
                    for (size_t keyIndex = 0; keyIndex < values.size(); ++keyIndex)
                    {
                        Quat rotation(values[keyIndex][3],
                                      values[keyIndex][0],
                                      values[keyIndex][1],
                                      values[keyIndex][2]);
                        const float lengthSquared = glm::dot(rotation, rotation);
                        if (!std::isfinite(lengthSquared) ||
                            lengthSquared <= 1.0e-12f)
                        {
                            error = "glTF animation contains a zero or non-finite rotation.";
                            return false;
                        }
                        track.rotationKeyframes.emplace_back(
                            times[keyIndex], glm::normalize(rotation), interpolation);
                    }
                }
                else
                {
                    std::vector<std::array<float, 3>> values;
                    if (!ReadFloatVectorAccessor<3>(gltf,
                                                    sampler.output,
                                                    values,
                                                    error) ||
                        values.size() != times.size())
                    {
                        if (error.empty())
                            error = "glTF vector output count does not match its input times.";
                        return false;
                    }
                    std::vector<Animation::KeyframeVec3>& keyframes =
                        channel.target_path == "translation"
                            ? track.translationKeyframes
                            : track.scaleKeyframes;
                    if (!keyframes.empty())
                    {
                        error = "glTF animation contains duplicate " +
                                channel.target_path + " channels for one target.";
                        return false;
                    }
                    keyframes.reserve(values.size());
                    for (size_t keyIndex = 0; keyIndex < values.size(); ++keyIndex)
                    {
                        Vec3 value(values[keyIndex][0],
                                   values[keyIndex][1],
                                   values[keyIndex][2]);
                        if (channel.target_path == "translation")
                            value *= options.scaleFactor;
                        keyframes.emplace_back(times[keyIndex], value, interpolation);
                    }
                }
            }

            if (tracks.empty())
            {
                error = "glTF animation does not contain a supported transform channel.";
                return false;
            }
            for (auto& [targetPath, track] : tracks)
            {
                const size_t nodeIndex =
                    static_cast<size_t>(trackNodes[targetPath]);
                Animation::TransformSample defaults =
                    Animation::TransformSample::FromMatrix(localMatrices[nodeIndex]);
                if (track.targetType == Animation::TrackTargetType::Bone &&
                    result.skeleton)
                {
                    const int sourceOrdinal = jointNodeToSourceOrdinal[nodeIndex];
                    const int boneIndex = sourceOrdinal >= 0
                        ? m_skinJointRemap[static_cast<size_t>(sourceOrdinal)]
                        : -1;
                    const Animation::Bone* bone = result.skeleton->GetBone(boneIndex);
                    if (!bone)
                    {
                        error = "Animation target could not be mapped to its canonical bone.";
                        return false;
                    }
                    defaults = bone->localBindPose;
                }
                if (track.translationKeyframes.empty())
                    track.translationKeyframes.emplace_back(
                        0, defaults.translation, Animation::InterpolationMode::Step);
                if (track.rotationKeyframes.empty())
                    track.rotationKeyframes.emplace_back(
                        0, defaults.rotation, Animation::InterpolationMode::Step);
                if (track.scaleKeyframes.empty())
                    track.scaleKeyframes.emplace_back(
                        0, defaults.scale, Animation::InterpolationMode::Step);
                clip->AddTransformTrack(std::move(track));
            }
            clip->UpdateDuration();
            if (result.skeleton && !clip->ValidateAgainstSkeleton(*result.skeleton).empty())
            {
                error = "Imported glTF animation contains an unresolved bone target.";
                return false;
            }
            result.animationClips.emplace(clipName, std::move(clip));
        }
        return true;
    }

    // =========================================================================
    // Mesh Parsing
    // =========================================================================

    bool GLTFImporter::ParseMeshes(const tinygltf::Model& gltf,
                                   GLTFImportResult& result,
                                   const GLTFImportOptions& options,
                                   std::string& error)
    {
        // In glTF, a "mesh" can have multiple primitives
        // We treat each primitive as a separate Mesh (SubMesh pattern)
        // But we can also combine primitives into a single Mesh with SubMeshes

        for (size_t meshIdx = 0; meshIdx < gltf.meshes.size(); ++meshIdx)
        {
            const auto& gltfMesh = gltf.meshes[meshIdx];
            std::string meshName = gltfMesh.name.empty() ? "Mesh_" + std::to_string(meshIdx) : gltfMesh.name;

            // For each primitive, create a Mesh
            for (size_t primIdx = 0; primIdx < gltfMesh.primitives.size(); ++primIdx)
            {
                auto mesh = ConvertPrimitive(gltf,
                                             gltfMesh.primitives[primIdx],
                                             meshName,
                                             static_cast<int>(primIdx),
                                             options,
                                             error);
                if (!mesh)
                    return false;
                result.meshes.push_back(std::move(mesh));
            }
        }
        return true;
    }

    Mesh::Ptr GLTFImporter::ConvertPrimitive(const tinygltf::Model& gltf,
                                               const tinygltf::Primitive& primitive,
                                               const std::string& meshName,
                                               int primitiveIndex,
                                               const GLTFImportOptions& options,
                                               std::string& error)
    {
        if (!primitive.targets.empty())
        {
            error = "glTF morph targets are not supported by this importer slice.";
            return nullptr;
        }
        auto mesh = std::make_shared<Mesh>();
        mesh->name = meshName + (primitiveIndex > 0 ? "_" + std::to_string(primitiveIndex) : "");

        // Set primitive type
        mesh->SetPrimitiveType(ConvertPrimitiveMode(primitive.mode));

        const bool hasJoints0 = primitive.attributes.contains("JOINTS_0");
        const bool hasWeights0 = primitive.attributes.contains("WEIGHTS_0");
        if (primitive.attributes.contains("JOINTS_1") ||
            primitive.attributes.contains("WEIGHTS_1"))
        {
            error = "glTF secondary joint/weight sets are not supported.";
            return nullptr;
        }
        if (hasJoints0 != hasWeights0)
        {
            error = "glTF primitive must provide JOINTS_0 and WEIGHTS_0 together.";
            return nullptr;
        }
        if (hasJoints0)
        {
            if (m_skinJointRemap.empty())
            {
                error = "glTF primitive has skinning attributes without a canonical skin.";
                return nullptr;
            }

            AccessorView jointView;
            AccessorView weightView;
            if (!ReadAccessorView(gltf,
                                  primitive.attributes.at("JOINTS_0"),
                                  jointView,
                                  error) ||
                !ReadAccessorView(gltf,
                                  primitive.attributes.at("WEIGHTS_0"),
                                  weightView,
                                  error))
            {
                return nullptr;
            }
            if ((jointView.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 jointView.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) ||
                jointView.valueType != TINYGLTF_TYPE_VEC4 ||
                jointView.componentCount != 4)
            {
                error = "JOINTS_0 must be an UNSIGNED_BYTE or UNSIGNED_SHORT VEC4 accessor.";
                return nullptr;
            }
            const bool floatWeights =
                weightView.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT;
            const bool integerWeights =
                weightView.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
                weightView.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
            if ((!floatWeights && !integerWeights) ||
                weightView.valueType != TINYGLTF_TYPE_VEC4 ||
                weightView.componentCount != 4 ||
                (integerWeights && !weightView.normalized))
            {
                error = "WEIGHTS_0 must be FLOAT VEC4 or a normalized unsigned integer VEC4 accessor.";
                return nullptr;
            }
            if (jointView.count == 0 || jointView.count != weightView.count)
            {
                error = "JOINTS_0 and WEIGHTS_0 must contain the same non-zero vertex count.";
                return nullptr;
            }

            std::vector<IVec4> boneIndices(jointView.count);
            std::vector<Vec4> boneWeights(weightView.count);
            for (size_t vertexIndex = 0;
                 vertexIndex < jointView.count;
                 ++vertexIndex)
            {
                IVec4 remapped{0};
                Vec4 weights{0.0f};
                const uint8* jointElement =
                    jointView.bytes + vertexIndex * jointView.stride;
                const uint8* weightElement =
                    weightView.bytes + vertexIndex * weightView.stride;
                for (size_t component = 0; component < 4; ++component)
                {
                    const uint32 sourceJoint =
                        jointView.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
                            ? static_cast<uint32>(jointElement[component])
                            : static_cast<uint32>(ReadUnaligned<uint16>(
                                  jointElement + component * sizeof(uint16)));
                    if (sourceJoint >= m_skinJointRemap.size() ||
                        m_skinJointRemap[sourceJoint] < 0)
                    {
                        error = "JOINTS_0 references a joint outside the imported skin.";
                        return nullptr;
                    }
                    remapped[static_cast<int>(component)] =
                        m_skinJointRemap[sourceJoint];

                    float weight = 0.0f;
                    if (floatWeights)
                    {
                        weight = ReadUnaligned<float>(
                            weightElement + component * sizeof(float));
                    }
                    else if (weightView.componentType ==
                             TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                    {
                        weight = static_cast<float>(weightElement[component]) /
                                 255.0f;
                    }
                    else
                    {
                        weight = static_cast<float>(ReadUnaligned<uint16>(
                                     weightElement + component * sizeof(uint16))) /
                                 65535.0f;
                    }
                    if (!std::isfinite(weight) || weight < 0.0f)
                    {
                        error = "WEIGHTS_0 contains a negative or non-finite value.";
                        return nullptr;
                    }
                    weights[static_cast<int>(component)] = weight;
                }
                const float weightSum = weights.x + weights.y + weights.z + weights.w;
                if (!std::isfinite(weightSum) || weightSum <= 1.0e-8f)
                {
                    error = "WEIGHTS_0 contains a vertex with no effective influence.";
                    return nullptr;
                }
                boneIndices[vertexIndex] = remapped;
                boneWeights[vertexIndex] = weights / weightSum;
            }
            mesh->SetBoneData(boneIndices, boneWeights);
        }

        size_t declaredVertexCount = 0;
        // Extract vertex attributes
        for (const auto& [attrName, accessorIdx] : primitive.attributes)
        {
            if (attrName == "JOINTS_0" || attrName == "WEIGHTS_0")
                continue;
            AccessorView view;
            if (!ReadAccessorView(gltf, accessorIdx, view, error))
                return nullptr;
            if (view.count == 0)
            {
                error = "glTF vertex attribute accessor is empty.";
                return nullptr;
            }
            if (declaredVertexCount == 0)
                declaredVertexCount = view.count;
            else if (view.count != declaredVertexCount)
            {
                error = "glTF primitive vertex attributes have inconsistent counts.";
                return nullptr;
            }

            // Map glTF attribute names to our names
            std::string ourAttrName;
            if (attrName == "POSITION") ourAttrName = VertexBufferNames::Position;
            else if (attrName == "NORMAL") ourAttrName = VertexBufferNames::Normal;
            else if (attrName == "TANGENT") ourAttrName = VertexBufferNames::Tangent;
            else if (attrName == "TEXCOORD_0") ourAttrName = VertexBufferNames::UV;
            else if (attrName == "TEXCOORD_1") ourAttrName = VertexBufferNames::UV1;
            else if (attrName == "COLOR_0") ourAttrName = VertexBufferNames::Color;
            else ourAttrName = attrName;

            // Extract data based on component type
            if (view.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                if (view.componentCount == 2)
                {
                    std::vector<Vec2> data(view.count);
                    for (size_t i = 0; i < view.count; ++i)
                    {
                        const uint8* ptr = view.bytes + i * view.stride;
                        const float x = ReadUnaligned<float>(ptr);
                        const float y = ReadUnaligned<float>(ptr + sizeof(float));
                        data[i] = Vec2(x, options.flipUVs &&
                                                (ourAttrName == VertexBufferNames::UV ||
                                                 ourAttrName == VertexBufferNames::UV1)
                                            ? 1.0f - y
                                            : y);
                    }
                    mesh->AddAttribute(ourAttrName, data);
                }
                else if (view.componentCount == 3)
                {
                    std::vector<Vec3> data(view.count);
                    for (size_t i = 0; i < view.count; ++i)
                    {
                        const uint8* ptr = view.bytes + i * view.stride;
                        data[i] = Vec3(ReadUnaligned<float>(ptr),
                                       ReadUnaligned<float>(ptr + sizeof(float)),
                                       ReadUnaligned<float>(ptr + 2 * sizeof(float)));
                        if (ourAttrName == VertexBufferNames::Position)
                            data[i] *= options.scaleFactor;
                    }
                    // Don't scale normals
                    if (ourAttrName == VertexBufferNames::Normal)
                    {
                        for (auto& v : data) v = glm::normalize(v);
                    }
                    mesh->AddAttribute(ourAttrName, data);
                }
                else if (view.componentCount == 4)
                {
                    std::vector<Vec4> data(view.count);
                    for (size_t i = 0; i < view.count; ++i)
                    {
                        const uint8* ptr = view.bytes + i * view.stride;
                        data[i] = Vec4(ReadUnaligned<float>(ptr),
                                       ReadUnaligned<float>(ptr + sizeof(float)),
                                       ReadUnaligned<float>(ptr + 2 * sizeof(float)),
                                       ReadUnaligned<float>(ptr + 3 * sizeof(float)));
                    }
                    mesh->AddAttribute(ourAttrName, data);
                }
                else
                {
                    error = "glTF FLOAT vertex attribute has an unsupported vector width.";
                    return nullptr;
                }
            }
            else
            {
                error = "glTF vertex attribute uses an unsupported component type.";
                return nullptr;
            }
        }

        if (hasJoints0 && declaredVertexCount != 0 &&
            declaredVertexCount != mesh->GetAttribute(VertexBufferNames::BoneIndices)->GetVertexCount())
        {
            error = "Skinning attribute count does not match the primitive vertex count.";
            return nullptr;
        }

        // Extract indices
        if (primitive.indices >= 0)
        {
            AccessorView indexView;
            if (!ReadAccessorView(gltf, primitive.indices, indexView, error))
                return nullptr;
            if (indexView.valueType != TINYGLTF_TYPE_SCALAR ||
                (indexView.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 indexView.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                 indexView.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT))
            {
                error = "glTF indices must use an unsigned SCALAR accessor.";
                return nullptr;
            }
            std::vector<uint32> indices(indexView.count);
            for (size_t index = 0; index < indexView.count; ++index)
            {
                const uint8* element = indexView.bytes + index * indexView.stride;
                if (indexView.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                    indices[index] = element[0];
                else if (indexView.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                    indices[index] = ReadUnaligned<uint16>(element);
                else
                    indices[index] = ReadUnaligned<uint32>(element);
                if (declaredVertexCount > 0 && indices[index] >= declaredVertexCount)
                {
                    error = "glTF index references a vertex outside the primitive.";
                    return nullptr;
                }
            }
            mesh->SetIndices(indices);
        }
        else if (mesh->GetPrimitiveType() == PrimitiveType::Triangles)
        {
            const VertexAttribute* posAttr = mesh->GetAttribute(VertexBufferNames::Position);
            const size_t vertexCount = posAttr ? posAttr->GetVertexCount() : 0;
            if (vertexCount > 0 && vertexCount <= std::numeric_limits<uint32_t>::max())
            {
                std::vector<uint32_t> indices(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i)
                {
                    indices[i] = static_cast<uint32_t>(i);
                }
                mesh->SetIndices(indices);
            }
        }

        // Generate normals if needed
        if (options.generateNormals && !mesh->HasAttribute(VertexBufferNames::Normal))
        {
            mesh->GenerateNormals();
        }

        // Generate tangents if needed
        if (options.generateTangents && !mesh->HasAttribute(VertexBufferNames::Tangent) &&
            mesh->HasAttribute(VertexBufferNames::Normal) && mesh->HasAttribute(VertexBufferNames::UV))
        {
            mesh->GenerateTangents();
        }

        // Compute bounding box
        mesh->ComputeBoundingBox();

        // Create a single SubMesh referencing the material
        SubMesh subMesh;
        subMesh.indexOffset = 0;
        subMesh.indexCount = static_cast<uint32_t>(mesh->GetIndexCount());
        subMesh.materialId = primitive.material >= 0 ? static_cast<uint32_t>(primitive.material) : 0;
        subMesh.name = mesh->name;
        subMesh.localBounds = mesh->GetBoundingBox();
        mesh->AddSubMesh(subMesh);

        return mesh;
    }

    std::vector<uint32_t> GLTFImporter::ExtractIndices(const tinygltf::Model& gltf, int accessorIndex)
    {
        if (accessorIndex < 0 || accessorIndex >= static_cast<int>(gltf.accessors.size()))
        {
            return {};
        }

        const auto& accessor = gltf.accessors[accessorIndex];
        const auto& bufferView = gltf.bufferViews[accessor.bufferView];
        const auto& buffer = gltf.buffers[bufferView.buffer];

        size_t byteOffset = bufferView.byteOffset + accessor.byteOffset;
        std::vector<uint32_t> indices(accessor.count);

        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
        {
            const uint32_t* data = reinterpret_cast<const uint32_t*>(buffer.data.data() + byteOffset);
            std::memcpy(indices.data(), data, accessor.count * sizeof(uint32_t));
        }
        else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
        {
            const uint16_t* data = reinterpret_cast<const uint16_t*>(buffer.data.data() + byteOffset);
            for (size_t i = 0; i < accessor.count; ++i)
            {
                indices[i] = data[i];
            }
        }
        else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
        {
            const uint8_t* data = buffer.data.data() + byteOffset;
            for (size_t i = 0; i < accessor.count; ++i)
            {
                indices[i] = data[i];
            }
        }

        return indices;
    }

    PrimitiveType GLTFImporter::ConvertPrimitiveMode(int mode)
    {
        switch (mode)
        {
            case TINYGLTF_MODE_POINTS: return PrimitiveType::Points;
            case TINYGLTF_MODE_LINE: return PrimitiveType::Lines;
            case TINYGLTF_MODE_LINE_LOOP: return PrimitiveType::LineLoop;
            case TINYGLTF_MODE_LINE_STRIP: return PrimitiveType::LineStrip;
            case TINYGLTF_MODE_TRIANGLES: return PrimitiveType::Triangles;
            case TINYGLTF_MODE_TRIANGLE_STRIP: return PrimitiveType::TriangleStrip;
            case TINYGLTF_MODE_TRIANGLE_FAN: return PrimitiveType::TriangleFan;
            default: return PrimitiveType::Triangles;
        }
    }

    // =========================================================================
    // Node Parsing
    // =========================================================================

    bool GLTFImporter::ParseNodes(const tinygltf::Model& gltf,
                                  GLTFImportResult& result,
                                  const GLTFImportOptions& options,
                                  std::string& error)
    {
        (void)result;
        // Build a mapping from glTF mesh index to the first engine Mesh.  Each
        // glTF primitive becomes one engine Mesh, and Nodes retain the full
        // contiguous primitive range below rather than losing it to this
        // compatibility first-index mapping.
        std::vector<int> meshToFirstPrimitiveIndex;
        int currentIndex = 0;
        for (const auto& gltfMesh : gltf.meshes)
        {
            meshToFirstPrimitiveIndex.push_back(currentIndex);
            currentIndex += static_cast<int>(gltfMesh.primitives.size());
        }

        // Create all nodes first (we'll connect hierarchy after)
        std::vector<Node::Ptr> allNodes;
        allNodes.reserve(gltf.nodes.size());

        for (size_t i = 0; i < gltf.nodes.size(); ++i)
        {
            const tinygltf::Node& sourceNode = gltf.nodes[i];
            if (sourceNode.mesh < -1 ||
                sourceNode.mesh >= static_cast<int>(meshToFirstPrimitiveIndex.size()))
            {
                error = "glTF node references an invalid mesh index.";
                return false;
            }
            Node::Ptr node = ConvertNode(gltf,
                                         static_cast<int>(i),
                                         meshToFirstPrimitiveIndex,
                                         options);
            if (sourceNode.mesh >= 0)
            {
                const int firstPrimitive =
                    meshToFirstPrimitiveIndex[sourceNode.mesh];
                const size_t primitiveCount =
                    gltf.meshes[sourceNode.mesh].primitives.size();
                std::vector<int> meshIndices;
                meshIndices.reserve(primitiveCount);
                for (size_t primitiveIndex = 0;
                     primitiveIndex < primitiveCount;
                     ++primitiveIndex)
                {
                    meshIndices.push_back(
                        firstPrimitive + static_cast<int>(primitiveIndex));
                }
                node->SetMeshIndices(std::move(meshIndices));
            }
            allNodes.push_back(std::move(node));
        }

        // Build hierarchy
        for (size_t i = 0; i < gltf.nodes.size(); ++i)
        {
            const auto& gltfNode = gltf.nodes[i];
            for (int childIdx : gltfNode.children)
            {
                if (childIdx >= 0 && childIdx < static_cast<int>(allNodes.size()))
                {
                    allNodes[i]->AddChild(allNodes[childIdx]);
                }
            }
        }

        // Store the nodes in result for scene building
        // We'll use them in ParseScene
        m_parsedNodes = std::move(allNodes);
        return true;
    }
    Node::Ptr GLTFImporter::ConvertNode(const tinygltf::Model& gltf, int nodeIndex,
                                          const std::vector<int>& meshToFirstPrimitiveIndex,
                                          const GLTFImportOptions& options)
    {
        const auto& gltfNode = gltf.nodes[nodeIndex];

        auto node = std::make_shared<Node>(gltfNode.name.empty() ? "Node_" + std::to_string(nodeIndex) : gltfNode.name);

        // Transform
        Transform& transform = node->GetLocalTransform();

        Mat4 localMatrix{1.0f};
        std::string ignoredError;
        const bool localValid = BuildNodeLocalMatrix(gltfNode,
                                                     options.scaleFactor,
                                                     localMatrix,
                                                     ignoredError);
        RVX_ASSERT_MSG(localValid,
                       "glTF node was validated before conversion: {}",
                       ignoredError);
        transform.SetMatrix(localMatrix);
        node->SetSkinIndex(gltfNode.skin);

        // Set mesh index (use index-based mode instead of MeshComponent)
        if (gltfNode.mesh >= 0 && gltfNode.mesh < static_cast<int>(meshToFirstPrimitiveIndex.size()))
        {
            const auto& gltfMesh = gltf.meshes[gltfNode.mesh];
            
            // For multi-primitive meshes, we use the first primitive index
            node->SetMeshIndex(meshToFirstPrimitiveIndex[gltfNode.mesh]);

            // Set material indices for each primitive/submesh
            std::vector<int> materialIndices;
            for (const auto& prim : gltfMesh.primitives)
            {
                materialIndices.push_back(prim.material >= 0 ? prim.material : 0);
            }
            node->SetMaterialIndices(materialIndices);
        }

        return node;
    }

    bool GLTFImporter::ParseScene(const tinygltf::Model& gltf,
                                  GLTFImportResult& result,
                                  std::string& error)
    {
        result.model = std::make_shared<Model>();

        // Find root nodes (nodes that are scene roots or have no parent)
        if (gltf.scenes.empty())
        {
            m_parsedNodes.clear();
            return true;
        }
        int sceneIdx = gltf.defaultScene >= 0 ? gltf.defaultScene : 0;

        if (sceneIdx < 0 || sceneIdx >= static_cast<int>(gltf.scenes.size()))
        {
            error = "glTF default scene index is out of range.";
            m_parsedNodes.clear();
            return false;
        }
        if (sceneIdx < static_cast<int>(gltf.scenes.size()))
        {
            const auto& scene = gltf.scenes[sceneIdx];

            if (scene.nodes.empty())
            {
                m_parsedNodes.clear();
                return true;
            }

            if (scene.nodes.size() == 1)
            {
                // Single root
                int rootIdx = scene.nodes[0];
                if (rootIdx < 0 || rootIdx >= static_cast<int>(m_parsedNodes.size()))
                {
                    error = "glTF scene references an invalid root node.";
                    m_parsedNodes.clear();
                    return false;
                }
                result.model->SetRootNode(m_parsedNodes[rootIdx]);
            }
            else if (scene.nodes.size() > 1)
            {
                // Multiple roots - create a container node
                auto containerRoot = std::make_shared<Node>("Root");
                for (int nodeIdx : scene.nodes)
                {
                    if (nodeIdx < 0 ||
                        nodeIdx >= static_cast<int>(m_parsedNodes.size()))
                    {
                        error = "glTF scene references an invalid root node.";
                        m_parsedNodes.clear();
                        return false;
                    }
                    containerRoot->AddChild(m_parsedNodes[nodeIdx]);
                }
                result.model->SetRootNode(containerRoot);
            }
        }

        // Clear temporary storage
        m_parsedNodes.clear();
        return result.model->GetRootNode() != nullptr;
    }

    // =========================================================================
    // Utility
    // =========================================================================

    void GLTFImporter::ReportProgress(float progress, const std::string& stage)
    {
        if (m_progressCallback)
        {
            m_progressCallback(progress, stage);
        }
    }

} // namespace RVX::Resource
