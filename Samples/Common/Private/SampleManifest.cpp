/** @file SampleManifest.cpp @brief Strict RVX.SampleManifest v1 parser. */

#include "Samples/SampleManifest.h"

#include "Core/Hash/SHA256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace RVX
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr size_t MaximumSamples = 256;
        constexpr size_t MaximumCapabilitiesPerClass = 256;
        constexpr size_t MaximumFallbacks = 256;
        constexpr size_t MaximumCookedAssets = 4096;
        constexpr size_t MaximumCameraBookmarks = 64;
        constexpr size_t MaximumCaptureTicks = 4096;
        constexpr size_t MaximumRegionsOfInterest = 64;
        constexpr size_t MaximumMetricPolicies = 256;
        constexpr size_t MaximumIdentifierLength = 128;
        constexpr size_t MaximumAssessmentCodeLength = 256;
        constexpr float64 MaximumWorldCoordinateMagnitude = 1.0e6;
        constexpr float64 MaximumMetricMagnitude = 1.0e15;
        constexpr uint64 MaximumCaptureTick = 10000000;

        bool Fail(std::string* outError, std::string message)
        {
            if (outError)
            {
                *outError = std::move(message);
            }
            return false;
        }

        class DuplicateKeySax final : public nlohmann::json_sax<Json>
        {
        public:
            using number_integer_t = Json::number_integer_t;
            using number_unsigned_t = Json::number_unsigned_t;
            using number_float_t = Json::number_float_t;
            using string_t = Json::string_t;
            using binary_t = Json::binary_t;

            bool null() override { return true; }
            bool boolean(bool) override { return true; }
            bool number_integer(number_integer_t) override { return true; }
            bool number_unsigned(number_unsigned_t) override { return true; }
            bool number_float(number_float_t, const string_t&) override
            {
                return true;
            }
            bool string(string_t&) override { return true; }
            bool binary(binary_t&) override { return true; }

            bool start_object(size_t) override
            {
                m_frames.push_back({true, {}});
                return true;
            }

            bool key(string_t& value) override
            {
                if (m_frames.empty() || !m_frames.back().object)
                {
                    m_error = "JSON parser reported an object key outside an object";
                    return false;
                }
                if (!m_frames.back().keys.insert(value).second)
                {
                    m_error = "Duplicate JSON object key: " + value;
                    return false;
                }
                return true;
            }

            bool end_object() override { return PopFrame(true); }

            bool start_array(size_t) override
            {
                m_frames.push_back({false, {}});
                return true;
            }

            bool end_array() override { return PopFrame(false); }

            bool parse_error(size_t position,
                             const std::string&,
                             const nlohmann::detail::exception& exception) override
            {
                if (m_error.empty())
                {
                    m_error = "JSON parse error at byte " +
                              std::to_string(position) + ": " + exception.what();
                }
                return false;
            }

            [[nodiscard]] const std::string& GetError() const noexcept
            {
                return m_error;
            }

        private:
            struct Frame
            {
                bool object = false;
                std::unordered_set<std::string> keys;
            };

            bool PopFrame(bool expectedObject)
            {
                if (m_frames.empty() ||
                    m_frames.back().object != expectedObject)
                {
                    m_error = "JSON container nesting is inconsistent";
                    return false;
                }
                m_frames.pop_back();
                return true;
            }

            std::vector<Frame> m_frames;
            std::string m_error;
        };

        bool HasExactFields(const Json& object,
                            std::initializer_list<std::string_view> fields,
                            std::string_view path,
                            std::string* outError)
        {
            if (!object.is_object())
            {
                return Fail(outError, std::string(path) + " must be an object");
            }

            std::set<std::string, std::less<>> expected;
            for (const std::string_view field : fields)
            {
                expected.emplace(field);
                if (!object.contains(std::string(field)))
                {
                    return Fail(outError,
                                std::string(path) + " is missing required field '" +
                                    std::string(field) + "'");
                }
            }

            for (const auto& [key, value] : object.items())
            {
                static_cast<void>(value);
                if (!expected.contains(key))
                {
                    return Fail(outError,
                                std::string(path) + " contains unknown field '" +
                                    key + "'");
                }
            }
            return true;
        }

        bool ReadString(const Json& object,
                        const char* field,
                        std::string_view path,
                        size_t maximumLength,
                        std::string& output,
                        std::string* outError)
        {
            const Json& value = object.at(field);
            if (!value.is_string())
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must be a string");
            }
            output = value.get<std::string>();
            if (output.empty() || output.size() > maximumLength)
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must be a non-empty bounded string");
            }
            return true;
        }

        bool IsLogicalId(std::string_view value) noexcept
        {
            if (value.empty() || value.size() > MaximumIdentifierLength)
            {
                return false;
            }
            bool previousWasSeparator = true;
            for (const char character : value)
            {
                const bool alphanumeric =
                    (character >= 'a' && character <= 'z') ||
                    (character >= '0' && character <= '9');
                if (alphanumeric)
                {
                    previousWasSeparator = false;
                    continue;
                }
                if (character != '-' || previousWasSeparator)
                {
                    return false;
                }
                previousWasSeparator = true;
            }
            return !previousWasSeparator;
        }

        bool ReadLogicalId(const Json& object,
                           const char* field,
                           std::string_view path,
                           std::string& output,
                           std::string* outError)
        {
            if (!ReadString(object, field, path, MaximumIdentifierLength,
                            output, outError))
            {
                return false;
            }
            if (!IsLogicalId(output))
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must be a lowercase kebab-case logical id");
            }
            return true;
        }

        bool ReadBoundedArray(const Json& object,
                              const char* field,
                              std::string_view path,
                              size_t maximumSize,
                              const Json*& output,
                              std::string* outError)
        {
            const Json& value = object.at(field);
            if (!value.is_array())
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must be an array");
            }
            if (value.size() > maximumSize)
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " exceeds its entry limit");
            }
            output = &value;
            return true;
        }

        bool ReadAssessmentCodeArray(const Json& object,
                                     const char* field,
                                     std::string_view path,
                                     size_t maximumSize,
                                     std::vector<AssessmentCode>& output,
                                     std::string* outError)
        {
            const Json* values = nullptr;
            if (!ReadBoundedArray(object, field, path, maximumSize, values,
                                  outError))
            {
                return false;
            }

            std::set<std::string, std::less<>> unique;
            output.clear();
            output.reserve(values->size());
            for (size_t index = 0; index < values->size(); ++index)
            {
                const Json& value = (*values)[index];
                if (!value.is_string())
                {
                    return Fail(outError,
                                std::string(path) + "." + field + "[" +
                                    std::to_string(index) +
                                    "] must be a string");
                }
                const std::string codeValue = value.get<std::string>();
                if (codeValue.empty() ||
                    codeValue.size() > MaximumAssessmentCodeLength ||
                    !AssessmentCode::IsStable(codeValue))
                {
                    return Fail(outError,
                                std::string(path) + "." + field + "[" +
                                    std::to_string(index) +
                                    "] is not a stable AssessmentCode");
                }
                if (!unique.insert(codeValue).second)
                {
                    return Fail(outError,
                                std::string(path) + "." + field +
                                    " contains duplicate code '" + codeValue +
                                    "'");
                }
                output.emplace_back(codeValue);
            }
            return true;
        }

        bool ParseDomain(std::string_view value, SampleDomain& output) noexcept
        {
            if (value == "rendering") output = SampleDomain::Rendering;
            else if (value == "animation") output = SampleDomain::Animation;
            else if (value == "vfx") output = SampleDomain::VFX;
            else if (value == "environment") output = SampleDomain::Environment;
            else if (value == "physics") output = SampleDomain::Physics;
            else if (value == "systems") output = SampleDomain::Systems;
            else if (value == "qualification") output = SampleDomain::Qualification;
            else return false;
            return true;
        }

        bool ParseIntent(std::string_view value, SampleIntent& output) noexcept
        {
            if (value == "demonstration") output = SampleIntent::Demonstration;
            else if (value == "regression") output = SampleIntent::Regression;
            else if (value == "stress") output = SampleIntent::Stress;
            else if (value == "integration") output = SampleIntent::Integration;
            else return false;
            return true;
        }

        bool ReadFiniteNumber(const Json& object,
                              const char* field,
                              std::string_view path,
                              float64& output,
                              std::string* outError)
        {
            const Json& value = object.at(field);
            if (!value.is_number())
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must be a number");
            }
            output = value.get<float64>();
            if (!std::isfinite(output))
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must be finite");
            }
            return true;
        }

        bool ReadVector3(const Json& object,
                         const char* field,
                         std::string_view path,
                         std::array<float32, 3>& output,
                         std::string* outError)
        {
            const Json& value = object.at(field);
            if (!value.is_array() || value.size() != output.size())
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must contain exactly three numbers");
            }
            for (size_t index = 0; index < output.size(); ++index)
            {
                if (!value[index].is_number())
                {
                    return Fail(outError,
                                std::string(path) + "." + field + "[" +
                                    std::to_string(index) +
                                    "] must be a number");
                }
                const float64 component = value[index].get<float64>();
                if (!std::isfinite(component) ||
                    std::abs(component) > MaximumWorldCoordinateMagnitude)
                {
                    return Fail(outError,
                                std::string(path) + "." + field + "[" +
                                    std::to_string(index) +
                                    "] must be a finite bounded coordinate");
                }
                output[index] = static_cast<float32>(component);
            }
            return true;
        }

        bool ReadCameraBookmarks(const Json& object,
                                 std::string_view path,
                                 std::vector<SampleCameraBookmark>& output,
                                 std::string* outError)
        {
            const Json* values = nullptr;
            if (!ReadBoundedArray(object, "cameraBookmarks", path,
                                  MaximumCameraBookmarks, values, outError))
            {
                return false;
            }

            std::set<std::string, std::less<>> ids;
            output.clear();
            output.reserve(values->size());
            for (size_t index = 0; index < values->size(); ++index)
            {
                const std::string itemPath = std::string(path) +
                    ".cameraBookmarks[" + std::to_string(index) + "]";
                const Json& value = (*values)[index];
                if (!HasExactFields(
                        value,
                        {"id", "position", "target",
                         "verticalFieldOfViewDegrees", "nearPlane",
                         "farPlane", "exposure"},
                        itemPath, outError))
                {
                    return false;
                }

                SampleCameraBookmark bookmark;
                if (!ReadLogicalId(value, "id", itemPath, bookmark.id,
                                   outError) ||
                    !ReadVector3(value, "position", itemPath,
                                 bookmark.position, outError) ||
                    !ReadVector3(value, "target", itemPath,
                                 bookmark.target, outError))
                {
                    return false;
                }
                if (!ids.insert(bookmark.id).second)
                {
                    return Fail(outError,
                                std::string(path) +
                                    ".cameraBookmarks contains duplicate id '" +
                                    bookmark.id + "'");
                }

                float64 fieldOfView = 0.0;
                float64 nearPlane = 0.0;
                float64 farPlane = 0.0;
                float64 exposure = 0.0;
                if (!ReadFiniteNumber(value, "verticalFieldOfViewDegrees",
                                      itemPath, fieldOfView, outError) ||
                    !ReadFiniteNumber(value, "nearPlane", itemPath,
                                      nearPlane, outError) ||
                    !ReadFiniteNumber(value, "farPlane", itemPath,
                                      farPlane, outError) ||
                    !ReadFiniteNumber(value, "exposure", itemPath,
                                      exposure, outError))
                {
                    return false;
                }
                if (fieldOfView < 1.0 || fieldOfView >= 179.0)
                {
                    return Fail(outError,
                                itemPath +
                                    ".verticalFieldOfViewDegrees must be in [1, 179)");
                }
                if (nearPlane <= 0.0 || farPlane <= nearPlane ||
                    farPlane > 1.0e9)
                {
                    return Fail(outError,
                                itemPath +
                                    " must have 0 < nearPlane < farPlane <= 1e9");
                }
                if (exposure <= 0.0 || exposure > 1024.0)
                {
                    return Fail(outError,
                                itemPath + ".exposure must be in (0, 1024]");
                }

                float64 targetDistanceSquared = 0.0;
                for (size_t component = 0; component < 3; ++component)
                {
                    const float64 delta =
                        static_cast<float64>(bookmark.target[component]) -
                        static_cast<float64>(bookmark.position[component]);
                    targetDistanceSquared += delta * delta;
                }
                if (targetDistanceSquared <= 1.0e-12)
                {
                    return Fail(outError,
                                itemPath +
                                    ".position and target must be distinct");
                }

                bookmark.verticalFieldOfViewDegrees =
                    static_cast<float32>(fieldOfView);
                bookmark.nearPlane = static_cast<float32>(nearPlane);
                bookmark.farPlane = static_cast<float32>(farPlane);
                bookmark.exposure = static_cast<float32>(exposure);
                output.push_back(std::move(bookmark));
            }
            return true;
        }

        bool ReadLogicalIdArray(const Json& object,
                                const char* field,
                                std::string_view path,
                                size_t maximumSize,
                                std::vector<std::string>& output,
                                std::string* outError)
        {
            const Json* values = nullptr;
            if (!ReadBoundedArray(object, field, path, maximumSize, values,
                                  outError))
            {
                return false;
            }

            std::set<std::string, std::less<>> ids;
            output.clear();
            output.reserve(values->size());
            for (size_t index = 0; index < values->size(); ++index)
            {
                if (!(*values)[index].is_string())
                {
                    return Fail(outError,
                                std::string(path) + "." + field + "[" +
                                    std::to_string(index) +
                                    "] must be a string");
                }
                const std::string id = (*values)[index].get<std::string>();
                if (!IsLogicalId(id))
                {
                    return Fail(outError,
                                std::string(path) + "." + field + "[" +
                                    std::to_string(index) +
                                    "] must be a lowercase kebab-case logical id");
                }
                if (!ids.insert(id).second)
                {
                    return Fail(outError,
                                std::string(path) + "." + field +
                                    " contains duplicate id '" + id + "'");
                }
                output.push_back(id);
            }
            return true;
        }

        bool ReadCaptureTicks(const Json& object,
                              std::string_view path,
                              std::vector<uint32>& output,
                              std::string* outError)
        {
            const Json* values = nullptr;
            if (!ReadBoundedArray(object, "captureTicks", path,
                                  MaximumCaptureTicks, values, outError))
            {
                return false;
            }

            output.clear();
            output.reserve(values->size());
            uint64 previous = 0;
            for (size_t index = 0; index < values->size(); ++index)
            {
                const Json& value = (*values)[index];
                if (!value.is_number_unsigned())
                {
                    return Fail(outError,
                                std::string(path) + ".captureTicks[" +
                                    std::to_string(index) +
                                    "] must be an unsigned integer");
                }
                const uint64 tick = value.get<uint64>();
                if (tick == 0 || tick > MaximumCaptureTick ||
                    (index > 0 && tick <= previous))
                {
                    return Fail(outError,
                                std::string(path) +
                                    ".captureTicks must be strictly increasing positive bounded integers");
                }
                previous = tick;
                output.push_back(static_cast<uint32>(tick));
            }
            return true;
        }

        bool ReadOptionalMetricBound(const Json& object,
                                     const char* field,
                                     std::string_view path,
                                     std::optional<float64>& output,
                                     std::string* outError)
        {
            const Json& value = object.at(field);
            if (value.is_null())
            {
                output.reset();
                return true;
            }
            if (!value.is_number())
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must be a finite number or null");
            }
            const float64 number = value.get<float64>();
            if (!std::isfinite(number) ||
                std::abs(number) > MaximumMetricMagnitude)
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must be a finite bounded number or null");
            }
            output = number;
            return true;
        }

        bool ReadROIMetricPolicy(const Json& object,
                                 std::string_view path,
                                 SampleROIMetricPolicy& output,
                                 std::string* outError)
        {
            const Json& policy = object.at("roiMetricPolicy");
            const std::string policyPath =
                std::string(path) + ".roiMetricPolicy";
            if (!HasExactFields(policy, {"regionsOfInterest", "metrics"},
                                policyPath, outError))
            {
                return false;
            }

            const Json* regions = nullptr;
            if (!ReadBoundedArray(policy, "regionsOfInterest", policyPath,
                                  MaximumRegionsOfInterest, regions, outError))
            {
                return false;
            }
            std::set<std::string, std::less<>> regionIds;
            output.regionsOfInterest.clear();
            output.regionsOfInterest.reserve(regions->size());
            for (size_t index = 0; index < regions->size(); ++index)
            {
                const std::string regionPath = policyPath +
                    ".regionsOfInterest[" + std::to_string(index) + "]";
                const Json& regionJson = (*regions)[index];
                if (!HasExactFields(regionJson,
                                    {"id", "x", "y", "width", "height"},
                                    regionPath, outError))
                {
                    return false;
                }

                SampleVisualRegionOfInterest region;
                if (!ReadLogicalId(regionJson, "id", regionPath, region.id,
                                   outError))
                {
                    return false;
                }
                if (!regionIds.insert(region.id).second)
                {
                    return Fail(outError,
                                policyPath +
                                    ".regionsOfInterest contains duplicate id '" +
                                    region.id + "'");
                }

                float64 x = 0.0;
                float64 y = 0.0;
                float64 width = 0.0;
                float64 height = 0.0;
                if (!ReadFiniteNumber(regionJson, "x", regionPath, x,
                                      outError) ||
                    !ReadFiniteNumber(regionJson, "y", regionPath, y,
                                      outError) ||
                    !ReadFiniteNumber(regionJson, "width", regionPath, width,
                                      outError) ||
                    !ReadFiniteNumber(regionJson, "height", regionPath,
                                      height, outError))
                {
                    return false;
                }
                if (x < 0.0 || y < 0.0 || width <= 0.0 || height <= 0.0 ||
                    x + width > 1.0 + 1.0e-9 ||
                    y + height > 1.0 + 1.0e-9)
                {
                    return Fail(outError,
                                regionPath +
                                    " must be a non-empty normalized rectangle contained in [0,1]^2");
                }
                region.x = static_cast<float32>(x);
                region.y = static_cast<float32>(y);
                region.width = static_cast<float32>(width);
                region.height = static_cast<float32>(height);
                output.regionsOfInterest.push_back(std::move(region));
            }

            const Json* metrics = nullptr;
            if (!ReadBoundedArray(policy, "metrics", policyPath,
                                  MaximumMetricPolicies, metrics, outError))
            {
                return false;
            }
            std::set<std::pair<std::string, std::string>> metricKeys;
            output.metrics.clear();
            output.metrics.reserve(metrics->size());
            for (size_t index = 0; index < metrics->size(); ++index)
            {
                const std::string metricPath = policyPath + ".metrics[" +
                                               std::to_string(index) + "]";
                const Json& metricJson = (*metrics)[index];
                if (!HasExactFields(
                        metricJson,
                        {"code", "regionOfInterestId", "minimum", "maximum"},
                        metricPath, outError))
                {
                    return false;
                }

                std::string codeValue;
                std::string regionId;
                if (!ReadString(metricJson, "code", metricPath,
                                MaximumAssessmentCodeLength, codeValue,
                                outError) ||
                    !AssessmentCode::IsStable(codeValue))
                {
                    return Fail(outError,
                                metricPath +
                                    ".code must be a stable AssessmentCode");
                }
                if (!ReadLogicalId(metricJson, "regionOfInterestId",
                                   metricPath, regionId, outError))
                {
                    return false;
                }
                if (!regionIds.contains(regionId))
                {
                    return Fail(outError,
                                metricPath +
                                    ".regionOfInterestId does not reference a declared ROI");
                }

                SampleVisualMetricPolicy metric;
                metric.code = AssessmentCode(codeValue);
                metric.regionOfInterestId = regionId;
                if (!ReadOptionalMetricBound(metricJson, "minimum", metricPath,
                                             metric.minimum, outError) ||
                    !ReadOptionalMetricBound(metricJson, "maximum", metricPath,
                                             metric.maximum, outError))
                {
                    return false;
                }
                if (!metric.minimum.has_value() && !metric.maximum.has_value())
                {
                    return Fail(outError,
                                metricPath +
                                    " must declare at least one metric bound");
                }
                if (metric.minimum.has_value() && metric.maximum.has_value() &&
                    *metric.minimum > *metric.maximum)
                {
                    return Fail(outError,
                                metricPath +
                                    ".minimum must not exceed .maximum");
                }
                if (!metricKeys.emplace(codeValue, regionId).second)
                {
                    return Fail(outError,
                                policyPath +
                                    ".metrics contains a duplicate code/ROI pair");
                }
                output.metrics.push_back(std::move(metric));
            }
            return true;
        }

        bool ReadUnsigned32(const Json& object,
                            const char* field,
                            std::string_view path,
                            uint32& output,
                            std::string* outError)
        {
            const Json& value = object.at(field);
            if (!value.is_number_unsigned())
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " must be an unsigned integer");
            }
            const uint64 number = value.get<uint64>();
            if (number > std::numeric_limits<uint32>::max())
            {
                return Fail(outError,
                            std::string(path) + "." + field +
                                " exceeds uint32 range");
            }
            output = static_cast<uint32>(number);
            return true;
        }

        bool ReadFixedTime(const Json& object,
                           std::string_view path,
                           SampleFixedTime& output,
                           std::string* outError)
        {
            const Json& fixedTime = object.at("fixedTime");
            const std::string fixedTimePath =
                std::string(path) + ".fixedTime";
            if (!HasExactFields(
                    fixedTime,
                    {"numerator", "denominator", "deterministicSeed"},
                    fixedTimePath, outError))
            {
                return false;
            }

            SampleFixedTime parsed;
            if (!ReadUnsigned32(fixedTime, "numerator", fixedTimePath,
                                parsed.numerator, outError) ||
                !ReadUnsigned32(fixedTime, "denominator", fixedTimePath,
                                parsed.denominator, outError) ||
                !ReadUnsigned32(fixedTime, "deterministicSeed", fixedTimePath,
                                parsed.deterministicSeed, outError))
            {
                return false;
            }
            if (parsed.numerator == 0 || parsed.denominator == 0)
            {
                return Fail(outError,
                            fixedTimePath +
                                ".numerator and .denominator must both be positive");
            }

            const uint32 divisor =
                std::gcd(parsed.numerator, parsed.denominator);
            parsed.numerator /= divisor;
            parsed.denominator /= divisor;

            const long double seconds =
                static_cast<long double>(parsed.numerator) /
                static_cast<long double>(parsed.denominator);
            if (!std::isfinite(seconds) || seconds <= 0.0L || seconds > 1.0L)
            {
                return Fail(outError,
                            fixedTimePath +
                                " must describe a finite step in (0, 1] seconds");
            }

            output = parsed;
            return true;
        }

        bool ReadScenario(const Json& value,
                          size_t index,
                          SampleScenarioManifest& output,
                          std::string* outError)
        {
            const std::string path =
                "samples[" + std::to_string(index) + "]";
            if (!HasExactFields(
                    value,
                    {"id", "domain", "intent", "defaultRecipe",
                     "defaultQualityProfile", "requiredCapabilities",
                     "optionalCapabilities", "allowedFallbacks",
                     "cookedAssetIds", "fixedTime", "cameraBookmarks", "captureTicks",
                     "visualContractVersion", "roiMetricPolicy",
                     "deterministicQualification"},
                    path, outError))
            {
                return false;
            }

            if (!ReadLogicalId(value, "id", path, output.id, outError))
            {
                return false;
            }

            std::string domain;
            std::string intent;
            std::string recipe;
            std::string quality;
            if (!ReadString(value, "domain", path, MaximumIdentifierLength,
                            domain, outError) ||
                !ParseDomain(domain, output.domain))
            {
                return Fail(outError, path + ".domain is not a known SampleDomain");
            }
            if (!ReadString(value, "intent", path, MaximumIdentifierLength,
                            intent, outError) ||
                !ParseIntent(intent, output.intent))
            {
                return Fail(outError, path + ".intent is not a known SampleIntent");
            }
            if (!ReadString(value, "defaultRecipe", path,
                            MaximumIdentifierLength, recipe, outError) ||
                !ParseSamplePipelineRecipe(recipe, output.defaultRecipe))
            {
                return Fail(outError,
                            path +
                                ".defaultRecipe is not a known SamplePipelineRecipe");
            }
            if (!ReadString(value, "defaultQualityProfile", path,
                            MaximumIdentifierLength, quality, outError) ||
                !ParseSampleQualityTier(quality, output.defaultQualityProfile))
            {
                return Fail(outError,
                            path +
                                ".defaultQualityProfile is not a known SampleQualityTier");
            }

            if (!ReadAssessmentCodeArray(
                    value, "requiredCapabilities", path,
                    MaximumCapabilitiesPerClass, output.requiredCapabilities,
                    outError) ||
                !ReadAssessmentCodeArray(
                    value, "optionalCapabilities", path,
                    MaximumCapabilitiesPerClass, output.optionalCapabilities,
                    outError) ||
                !ReadAssessmentCodeArray(
                    value, "allowedFallbacks", path, MaximumFallbacks,
                    output.allowedFallbacks, outError) ||
                !ReadLogicalIdArray(value, "cookedAssetIds", path,
                                    MaximumCookedAssets,
                                    output.cookedAssetIds, outError) ||
                !ReadFixedTime(value, path, output.fixedTime, outError) ||
                !ReadCameraBookmarks(value, path, output.cameraBookmarks,
                                     outError) ||
                !ReadCaptureTicks(value, path, output.captureTicks,
                                  outError) ||
                !ReadUnsigned32(value, "visualContractVersion", path,
                                output.visualContractVersion, outError) ||
                !ReadROIMetricPolicy(value, path, output.roiMetricPolicy,
                                     outError))
            {
                return false;
            }

            std::set<std::string, std::less<>> requiredCodes;
            for (const AssessmentCode& code : output.requiredCapabilities)
            {
                requiredCodes.insert(code.GetValue());
            }
            for (const AssessmentCode& code : output.optionalCapabilities)
            {
                if (requiredCodes.contains(code.GetValue()))
                {
                    return Fail(outError,
                                path +
                                    ".requiredCapabilities and .optionalCapabilities must be disjoint");
                }
            }

            const Json& deterministic = value.at("deterministicQualification");
            if (!deterministic.is_boolean())
            {
                return Fail(outError,
                            path +
                                ".deterministicQualification must be boolean");
            }
            output.deterministicQualification = deterministic.get<bool>();
            if (output.deterministicQualification)
            {
                if (output.visualContractVersion == 0)
                {
                    return Fail(outError,
                                path +
                                    ".visualContractVersion must be positive for deterministic qualification");
                }
                if (output.requiredCapabilities.empty())
                {
                    return Fail(outError,
                                path +
                                    ".requiredCapabilities must not be empty for deterministic qualification");
                }
                if (output.cameraBookmarks.empty() ||
                    output.captureTicks.empty() ||
                    output.roiMetricPolicy.regionsOfInterest.empty() ||
                    output.roiMetricPolicy.metrics.empty())
                {
                    return Fail(outError,
                                path +
                                    " deterministic qualification requires camera bookmarks, capture ticks, ROIs, and metric policies");
                }
            }
            return true;
        }
    } // namespace

    const SampleScenarioManifest* SampleManifestCatalog::Find(
        std::string_view id) const noexcept
    {
        const auto found = samples.find(id);
        return found == samples.end() ? nullptr : &found->second;
    }

    bool SampleManifestParser::Parse(std::string_view jsonText,
                                     SampleManifestCatalog& outCatalog,
                                     std::string* outError)
    {
        if (jsonText.empty())
        {
            return Fail(outError, "Sample manifest must not be empty");
        }
        if (jsonText.size() > MaximumDocumentBytes)
        {
            return Fail(outError, "Sample manifest exceeds the 1 MiB limit");
        }

        const std::string source(jsonText);
        DuplicateKeySax duplicateKeySax;
        if (!Json::sax_parse(source, &duplicateKeySax))
        {
            return Fail(outError,
                        duplicateKeySax.GetError().empty()
                            ? "Sample manifest is not valid JSON"
                            : duplicateKeySax.GetError());
        }

        Json root;
        try
        {
            root = Json::parse(source);
        }
        catch (const std::exception& exception)
        {
            return Fail(outError,
                        std::string("Sample manifest JSON parse failed: ") +
                            exception.what());
        }

        try
        {
            if (!HasExactFields(root, {"schema", "schemaVersion", "samples"},
                                "manifest", outError))
            {
                return false;
            }

            SampleManifestCatalog parsed;
            if (!root.at("schema").is_string() ||
                root.at("schema").get<std::string>() !=
                    RVX_SAMPLE_MANIFEST_SCHEMA)
            {
                return Fail(outError,
                            "manifest.schema must equal RVX.SampleManifest");
            }
            if (!root.at("schemaVersion").is_number_unsigned() ||
                root.at("schemaVersion").get<uint64>() !=
                    RVX_SAMPLE_MANIFEST_SCHEMA_VERSION)
            {
                return Fail(outError,
                            "manifest.schemaVersion must equal 1");
            }

            const Json& samples = root.at("samples");
            if (!samples.is_array() || samples.empty() ||
                samples.size() > MaximumSamples)
            {
                return Fail(outError,
                            "manifest.samples must be a non-empty bounded array");
            }

            for (size_t index = 0; index < samples.size(); ++index)
            {
                SampleScenarioManifest scenario;
                if (!ReadScenario(samples[index], index, scenario, outError))
                {
                    return false;
                }
                const std::string id = scenario.id;
                if (!parsed.samples.emplace(id, std::move(scenario)).second)
                {
                    return Fail(
                        outError,
                        "manifest.samples contains duplicate sample id '" + id +
                            "'");
                }
            }

            parsed.contentSha256 = Hash::FormatSHA256Digest(
                Hash::ComputeSHA256(jsonText));
            outCatalog = std::move(parsed);
            if (outError)
            {
                outError->clear();
            }
            return true;
        }
        catch (const std::exception& exception)
        {
            return Fail(outError,
                        std::string("Sample manifest value conversion failed: ") +
                            exception.what());
        }
    }

    bool SampleManifestParser::LoadFile(const std::filesystem::path& path,
                                        SampleManifestCatalog& outCatalog,
                                        std::string* outError)
    {
        if (path.empty())
        {
            return Fail(outError, "Sample manifest path must not be empty");
        }

        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            return Fail(outError,
                        "Failed to open Sample manifest: " + path.string());
        }
        const std::streampos end = stream.tellg();
        if (end < 0)
        {
            return Fail(outError,
                        "Failed to determine Sample manifest size: " +
                            path.string());
        }
        const uint64 size = static_cast<uint64>(end);
        if (size > MaximumDocumentBytes)
        {
            return Fail(outError,
                        "Sample manifest exceeds the 1 MiB limit: " +
                            path.string());
        }
        stream.seekg(0, std::ios::beg);
        std::string text(static_cast<size_t>(size), '\0');
        if (size != 0 &&
            !stream.read(text.data(), static_cast<std::streamsize>(size)))
        {
            return Fail(outError,
                        "Failed to read Sample manifest: " + path.string());
        }
        return Parse(text, outCatalog, outError);
    }
} // namespace RVX
