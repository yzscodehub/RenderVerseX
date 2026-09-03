#include "Core/Hash/SHA256.h"
#include "Samples/SampleManifest.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    std::string ReplaceOnce(std::string source,
                            std::string_view before,
                            std::string_view after)
    {
        const size_t position = source.find(before);
        EXPECT_NE(position, std::string::npos) << before;
        if (position != std::string::npos)
        {
            source.replace(position, before.size(), after);
        }
        return source;
    }

    std::string MakeSample(std::string_view id = "interior-rendering")
    {
        std::string sample = R"JSON(
    {
      "id": "$ID$",
      "domain": "rendering",
      "intent": "regression",
      "defaultRecipe": "forward-plus",
      "defaultQualityProfile": "high",
      "requiredCapabilities": [
        "RENDER.CAPABILITY.DIRECTIONAL_CSM",
        "RENDER.CAPABILITY.TRANSPARENT_PASS"
      ],
      "optionalCapabilities": [
        "RENDER.CAPABILITY.POINT_SHADOW"
      ],
      "allowedFallbacks": [
        "RENDER.FALLBACK.SOFTWARE_ADAPTER"
      ],
      "cookedAssetIds": [
        "crytek-sponza",
        "kloofendal-sky"
      ],
      "fixedTime": {
        "numerator": 1,
        "denominator": 60,
        "deterministicSeed": 4294967295
      },
      "cameraBookmarks": [
        {
          "id": "hero",
          "position": [0.0, 1.5, 4.0],
          "target": [0.0, 1.0, 0.0],
          "verticalFieldOfViewDegrees": 45.0,
          "nearPlane": 0.05,
          "farPlane": 1000.0,
          "exposure": 1.25
        }
      ],
      "captureTicks": [8, 16],
      "visualContractVersion": 3,
      "roiMetricPolicy": {
        "regionsOfInterest": [
          {
            "id": "frame",
            "x": 0.0,
            "y": 0.0,
            "width": 1.0,
            "height": 1.0
          },
          {
            "id": "subject",
            "x": 0.2,
            "y": 0.15,
            "width": 0.6,
            "height": 0.7
          }
        ],
        "metrics": [
          {
            "code": "VISUAL.METRIC.LUMA_RANGE",
            "regionOfInterestId": "frame",
            "minimum": 8.0,
            "maximum": null
          },
          {
            "code": "VISUAL.METRIC.SUBJECT_CONTRAST",
            "regionOfInterestId": "subject",
            "minimum": 0.1,
            "maximum": 1.0
          }
        ]
      },
      "deterministicQualification": true
    })JSON";
        return ReplaceOnce(std::move(sample), "$ID$", id);
    }

    std::string MakeManifest(std::string samples = MakeSample())
    {
        return std::string(R"JSON({
  "schema": "RVX.SampleManifest",
  "schemaVersion": 1,
  "samples": [)JSON") + samples + R"JSON(
  ]
})JSON";
    }

    void ExpectRejected(const std::string& text,
                        std::string_view expectedError = {})
    {
        RVX::SampleManifestCatalog catalog;
        catalog.schema = "unchanged";
        catalog.schemaVersion = 77;
        catalog.contentSha256 = "sentinel";
        const RVX::SampleManifestCatalog before = catalog;

        std::string error;
        EXPECT_FALSE(RVX::SampleManifestParser::Parse(text, catalog, &error));
        EXPECT_FALSE(error.empty());
        if (!expectedError.empty())
        {
            EXPECT_NE(error.find(expectedError), std::string::npos) << error;
        }
        EXPECT_EQ(catalog, before) << "Parser failure must be transactional";
    }

    std::filesystem::path MakeTemporaryPath(std::string_view suffix)
    {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        return std::filesystem::temp_directory_path() /
               ("rvx-sample-manifest-" + std::to_string(stamp) + "-" +
                std::string(suffix));
    }

    TEST(SampleManifestValidation, ParsesCompleteV1ContractAndHashesExactBytes)
    {
        const std::string text = MakeManifest();
        RVX::SampleManifestCatalog catalog;
        std::string error;
        ASSERT_TRUE(RVX::SampleManifestParser::Parse(text, catalog, &error))
            << error;

        EXPECT_EQ(catalog.schema, RVX::RVX_SAMPLE_MANIFEST_SCHEMA);
        EXPECT_EQ(catalog.schemaVersion,
                  RVX::RVX_SAMPLE_MANIFEST_SCHEMA_VERSION);
        EXPECT_EQ(catalog.contentSha256,
                  RVX::Hash::FormatSHA256Digest(
                      RVX::Hash::ComputeSHA256(text)));

        const RVX::SampleScenarioManifest* sample =
            catalog.Find("interior-rendering");
        ASSERT_NE(sample, nullptr);
        EXPECT_EQ(sample->domain, RVX::SampleDomain::Rendering);
        EXPECT_EQ(sample->intent, RVX::SampleIntent::Regression);
        EXPECT_EQ(sample->defaultRecipe,
                  RVX::SamplePipelineRecipe::ForwardStandard);
        EXPECT_EQ(sample->defaultQualityProfile,
                  RVX::SampleQualityTier::High);
        EXPECT_EQ(sample->requiredCapabilities.size(), 2u);
        EXPECT_EQ(sample->optionalCapabilities.size(), 1u);
        EXPECT_EQ(sample->allowedFallbacks.size(), 1u);
        EXPECT_EQ(sample->cookedAssetIds.size(), 2u);
        EXPECT_EQ(sample->fixedTime.numerator, 1u);
        EXPECT_EQ(sample->fixedTime.denominator, 60u);
        EXPECT_EQ(sample->fixedTime.deterministicSeed,
                  std::numeric_limits<RVX::uint32>::max());
        ASSERT_EQ(sample->cameraBookmarks.size(), 1u);
        EXPECT_FLOAT_EQ(sample->cameraBookmarks[0].position[1], 1.5f);
        EXPECT_FLOAT_EQ(sample->cameraBookmarks[0].target[2], 0.0f);
        EXPECT_FLOAT_EQ(sample->cameraBookmarks[0].exposure, 1.25f);
        EXPECT_EQ(sample->captureTicks,
                  (std::vector<RVX::uint32>{8u, 16u}));
        EXPECT_EQ(sample->visualContractVersion, 3u);
        EXPECT_TRUE(sample->deterministicQualification);
        EXPECT_EQ(sample->roiMetricPolicy.regionsOfInterest.size(), 2u);
        ASSERT_EQ(sample->roiMetricPolicy.metrics.size(), 2u);
        EXPECT_EQ(sample->roiMetricPolicy.metrics[0].code.GetValue(),
                  "VISUAL.METRIC.LUMA_RANGE");
        EXPECT_TRUE(sample->roiMetricPolicy.metrics[0].minimum.has_value());
        EXPECT_FALSE(sample->roiMetricPolicy.metrics[0].maximum.has_value());
        EXPECT_EQ(catalog.Find("missing"), nullptr);
    }

    TEST(SampleManifestValidation, SortsCatalogByIdAndRejectsDuplicateIds)
    {
        const std::string twoSamples =
            MakeManifest(MakeSample("zeta") + "," + MakeSample("alpha"));
        RVX::SampleManifestCatalog catalog;
        std::string error;
        ASSERT_TRUE(
            RVX::SampleManifestParser::Parse(twoSamples, catalog, &error))
            << error;
        ASSERT_EQ(catalog.samples.size(), 2u);
        EXPECT_EQ(catalog.samples.begin()->first, "alpha");

        ExpectRejected(
            MakeManifest(MakeSample("same") + "," + MakeSample("same")),
            "duplicate sample id");
    }

    TEST(SampleManifestValidation,
         Gfx25QualificationContractsUseBoundsSafeCameras)
    {
        const std::filesystem::path manifestPath =
            std::filesystem::path(RVX_SOURCE_DIR) /
            "Samples/RenderVerseSamples/Manifests/renderverse-samples.json";
        RVX::SampleManifestCatalog catalog;
        std::string error;
        ASSERT_TRUE(
            RVX::SampleManifestParser::LoadFile(manifestPath, catalog, &error))
            << error;

        const RVX::SampleScenarioManifest* modelViewer =
            catalog.Find("model-viewer");
        const RVX::SampleScenarioManifest* pbrMaterials =
            catalog.Find("pbr-materials");
        ASSERT_NE(modelViewer, nullptr);
        ASSERT_NE(pbrMaterials, nullptr);
        const std::vector<RVX::uint32> expectedTicks = {120u, 360u, 361u};
        EXPECT_TRUE(modelViewer->deterministicQualification);
        EXPECT_EQ(modelViewer->captureTicks, expectedTicks);
        EXPECT_EQ(modelViewer->cameraBookmarks.size(), 3u);
        EXPECT_EQ(modelViewer->roiMetricPolicy.regionsOfInterest.size(), 2u);
        ASSERT_EQ(modelViewer->requiredCapabilities.size(), 1u);
        EXPECT_EQ(modelViewer->requiredCapabilities.front().GetValue(),
                  "RENDER.CAPABILITY.COOKED_MODEL_RASTER");

        // Water Bottle source bounds are [-0.05446, 0.05446] on X/Z and
        // [-0.13023, 0.13023] on Y before its symmetric 180 degree node turn.
        for (const RVX::SampleCameraBookmark& bookmark :
             modelViewer->cameraBookmarks)
        {
            const auto& position = bookmark.position;
            EXPECT_TRUE(position[0] < -0.05446f || position[0] > 0.05446f ||
                        position[1] < -0.13023f || position[1] > 0.13023f ||
                        position[2] < -0.05446f || position[2] > 0.05446f);
            EXPECT_GT(bookmark.nearPlane, 0.0f);
            EXPECT_GT(bookmark.farPlane, bookmark.nearPlane);
        }

        EXPECT_TRUE(pbrMaterials->deterministicQualification);
        EXPECT_EQ(pbrMaterials->captureTicks, expectedTicks);
        EXPECT_EQ(pbrMaterials->cameraBookmarks.size(), 3u);
        EXPECT_EQ(pbrMaterials->roiMetricPolicy.regionsOfInterest.size(), 2u);
        ASSERT_EQ(pbrMaterials->requiredCapabilities.size(), 3u);
        EXPECT_EQ(pbrMaterials->requiredCapabilities[0].GetValue(),
                  "RENDER.CAPABILITY.STANDARD_LIT_PBR");
        EXPECT_EQ(pbrMaterials->requiredCapabilities[1].GetValue(),
                  "RENDER.CAPABILITY.MATERIAL_V2_EXTENSIONS");
        EXPECT_EQ(pbrMaterials->requiredCapabilities[2].GetValue(),
                  "RENDER.CAPABILITY.TEXTURE_IBL");

        // The factor cube has a shared sphere extent of +/-3.45 metres.
        for (const RVX::SampleCameraBookmark& bookmark :
             pbrMaterials->cameraBookmarks)
        {
            const auto& position = bookmark.position;
            EXPECT_TRUE(position[0] < -3.45f || position[0] > 3.45f ||
                        position[1] < -3.45f || position[1] > 3.45f ||
                        position[2] < -3.45f || position[2] > 3.45f);
            EXPECT_GT(bookmark.nearPlane, 0.0f);
            EXPECT_GT(bookmark.farPlane, bookmark.nearPlane);
        }
    }

    TEST(SampleManifestValidation, RejectsSyntaxDuplicateKeysAndUnknownFields)
    {
        ExpectRejected("{", "JSON parse error");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"schemaVersion\": 1,",
                        "\"schemaVersion\": 1, \"schemaVersion\": 1,"),
            "Duplicate JSON object key");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"schemaVersion\": 1,",
                        "\"schemaVersion\": 1, \"unexpected\": true,"),
            "unknown field");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"intent\": \"regression\",",
                        "\"intent\": \"regression\", \"typo\": 1,"),
            "unknown field");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"position\": [0.0, 1.5, 4.0],",
                        "\"position\": [0.0, 1.5, 4.0], \"up\": [0,1,0],"),
            "unknown field");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"metrics\": [",
                        "\"metrics\": [], \"tolerance\": 1, \"ignored\": ["),
            "unknown field");
    }

    TEST(SampleManifestValidation, RejectsWrongSchemaAndUnknownEnums)
    {
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "RVX.SampleManifest",
                                   "RVX.SampleManifest.Future"),
                       "schema must equal");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"schemaVersion\": 1",
                                   "\"schemaVersion\": 2"),
                       "schemaVersion");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"domain\": \"rendering\"",
                                   "\"domain\": \"gameplay\""),
                       "known SampleDomain");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"intent\": \"regression\"",
                                   "\"intent\": \"marketing\""),
                       "known SampleIntent");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"defaultRecipe\": \"forward-plus\"",
                                   "\"defaultRecipe\": \"magic\""),
                       "known SamplePipelineRecipe");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"defaultQualityProfile\": \"high\"",
                                   "\"defaultQualityProfile\": \"cinematic\""),
                       "known SampleQualityTier");
    }

    TEST(SampleManifestValidation, RejectsInvalidAndAmbiguousCapabilities)
    {
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "RENDER.CAPABILITY.DIRECTIONAL_CSM",
                        "render.directional-csm"),
            "stable AssessmentCode");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"RENDER.CAPABILITY.DIRECTIONAL_CSM\",\n        \"RENDER.CAPABILITY.TRANSPARENT_PASS\"",
                        "\"RENDER.CAPABILITY.DIRECTIONAL_CSM\",\n        \"RENDER.CAPABILITY.DIRECTIONAL_CSM\""),
            "duplicate code");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "RENDER.CAPABILITY.POINT_SHADOW",
                        "RENDER.CAPABILITY.DIRECTIONAL_CSM"),
            "must be disjoint");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"RENDER.FALLBACK.SOFTWARE_ADAPTER\"",
                        "\"RENDER.FALLBACK.SOFTWARE_ADAPTER\",\n        \"RENDER.FALLBACK.SOFTWARE_ADAPTER\""),
            "duplicate code");
    }

    TEST(SampleManifestValidation, RejectsPathLikeAndDuplicateLogicalIds)
    {
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"crytek-sponza\"",
                                   "\"../crytek-sponza\""),
                       "logical id");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"kloofendal-sky\"",
                                   "\"crytek-sponza\""),
                       "duplicate id");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"id\": \"interior-rendering\"",
                                   "\"id\": \"Interior Rendering\""),
                       "logical id");
    }

    TEST(SampleManifestValidation, RejectsInvalidCameraContracts)
    {
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"target\": [0.0, 1.0, 0.0]",
                                   "\"target\": [0.0, 1.5, 4.0]"),
                       "must be distinct");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"verticalFieldOfViewDegrees\": 45.0",
                                   "\"verticalFieldOfViewDegrees\": 179.0"),
                       "must be in");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"nearPlane\": 0.05",
                                   "\"nearPlane\": 0.0"),
                       "nearPlane");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"farPlane\": 1000.0",
                                   "\"farPlane\": 0.01"),
                       "farPlane");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"exposure\": 1.25",
                                   "\"exposure\": 0.0"),
                       "exposure");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"position\": [0.0, 1.5, 4.0]",
                                   "\"position\": [0.0, 1.5]"),
                       "exactly three");
    }

    TEST(SampleManifestValidation, RejectsInvalidCaptureTicks)
    {
        ExpectRejected(ReplaceOnce(MakeManifest(), "[8, 16]", "[0, 16]"),
                       "strictly increasing");
        ExpectRejected(ReplaceOnce(MakeManifest(), "[8, 16]", "[8, 8]"),
                       "strictly increasing");
        ExpectRejected(ReplaceOnce(MakeManifest(), "[8, 16]", "[16, 8]"),
                       "strictly increasing");
        ExpectRejected(ReplaceOnce(MakeManifest(), "[8, 16]", "[8.5]"),
                       "unsigned integer");
    }

    TEST(SampleManifestValidation,
         ParsesExactFixedTimeAndCanonicalizesTheRationalStep)
    {
        std::string text = ReplaceOnce(
            MakeManifest(),
            "\"numerator\": 1,\n        \"denominator\": 60",
            "\"numerator\": 2,\n        \"denominator\": 120");
        text = ReplaceOnce(std::move(text),
                           "\"deterministicSeed\": 4294967295",
                           "\"deterministicSeed\": 0");

        RVX::SampleManifestCatalog catalog;
        std::string error;
        ASSERT_TRUE(RVX::SampleManifestParser::Parse(text, catalog, &error))
            << error;
        const RVX::SampleScenarioManifest* sample =
            catalog.Find("interior-rendering");
        ASSERT_NE(sample, nullptr);
        EXPECT_EQ(sample->fixedTime.numerator, 1u);
        EXPECT_EQ(sample->fixedTime.denominator, 60u);
        EXPECT_EQ(sample->fixedTime.deterministicSeed, 0u);

        const std::string smallestStep = ReplaceOnce(
            MakeManifest(),
            "\"denominator\": 60",
            "\"denominator\": 4294967295");
        ASSERT_TRUE(RVX::SampleManifestParser::Parse(
            smallestStep, catalog, &error)) << error;
        ASSERT_NE(catalog.Find("interior-rendering"), nullptr);
        EXPECT_EQ(catalog.Find("interior-rendering")->fixedTime.denominator,
                  std::numeric_limits<RVX::uint32>::max());
    }

    TEST(SampleManifestValidation, RejectsInvalidFixedTimeContracts)
    {
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"numerator\": 1",
                                   "\"numerator\": 0"),
                       "must both be positive");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"denominator\": 60",
                                   "\"denominator\": 0"),
                       "must both be positive");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"numerator\": 1,\n        \"denominator\": 60",
                        "\"numerator\": 2,\n        \"denominator\": 1"),
            "must describe a finite step in (0, 1] seconds");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"numerator\": 1",
                                   "\"numerator\": -1"),
                       "unsigned integer");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"denominator\": 60",
                                   "\"denominator\": 60.0"),
                       "unsigned integer");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"numerator\": 1",
                                   "\"numerator\": 4294967296"),
                       "exceeds uint32 range");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"deterministicSeed\": 4294967295",
                                   "\"deterministicSeed\": 4294967296"),
                       "exceeds uint32 range");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"deterministicSeed\": 4294967295",
                        "\"deterministicSeed\": 4294967295,\n        \"tickRate\": 60"),
            "unknown field");
        ExpectRejected(
            ReplaceOnce(
                MakeManifest(),
                "      \"fixedTime\": {\n        \"numerator\": 1,\n        \"denominator\": 60,\n        \"deterministicSeed\": 4294967295\n      },\n",
                ""),
            "missing required field 'fixedTime'");
    }

    TEST(SampleManifestValidation, RejectsInvalidROIsAndMetricPolicies)
    {
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"x\": 0.2,",
                                   "\"x\": 0.8,"),
                       "normalized rectangle");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"regionOfInterestId\": \"subject\"",
                                   "\"regionOfInterestId\": \"missing\""),
                       "does not reference");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"minimum\": 8.0,\n            \"maximum\": null",
                                   "\"minimum\": null,\n            \"maximum\": null"),
                       "at least one metric bound");
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"minimum\": 0.1,\n            \"maximum\": 1.0",
                                   "\"minimum\": 2.0,\n            \"maximum\": 1.0"),
                       "must not exceed");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"code\": \"VISUAL.METRIC.SUBJECT_CONTRAST\",\n            \"regionOfInterestId\": \"subject\"",
                        "\"code\": \"VISUAL.METRIC.LUMA_RANGE\",\n            \"regionOfInterestId\": \"frame\""),
            "duplicate code/ROI pair");
    }

    TEST(SampleManifestValidation,
         DeterministicQualificationRequiresCompleteVisualEvidence)
    {
        ExpectRejected(ReplaceOnce(MakeManifest(),
                                   "\"visualContractVersion\": 3",
                                   "\"visualContractVersion\": 0"),
                       "must be positive");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"requiredCapabilities\": [\n        \"RENDER.CAPABILITY.DIRECTIONAL_CSM\",\n        \"RENDER.CAPABILITY.TRANSPARENT_PASS\"\n      ]",
                        "\"requiredCapabilities\": []"),
            "must not be empty");
        ExpectRejected(
            ReplaceOnce(MakeManifest(),
                        "\"cameraBookmarks\": [\n        {",
                        "\"cameraBookmarks\": [],\n      \"discardedCameraBookmarks\": [\n        {"),
            "unknown field");

        std::string nondeterministic = MakeManifest();
        nondeterministic = ReplaceOnce(
            std::move(nondeterministic),
            "\"deterministicQualification\": true",
            "\"deterministicQualification\": false");
        nondeterministic = ReplaceOnce(std::move(nondeterministic),
                                       "\"visualContractVersion\": 3",
                                       "\"visualContractVersion\": 0");
        nondeterministic = ReplaceOnce(
            std::move(nondeterministic),
            "\"requiredCapabilities\": [\n        \"RENDER.CAPABILITY.DIRECTIONAL_CSM\",\n        \"RENDER.CAPABILITY.TRANSPARENT_PASS\"\n      ]",
            "\"requiredCapabilities\": []");
        RVX::SampleManifestCatalog catalog;
        std::string error;
        EXPECT_TRUE(RVX::SampleManifestParser::Parse(
            nondeterministic, catalog, &error)) << error;
    }

    TEST(SampleManifestValidation, LoadsFilesAndRejectsMissingOrOversizedInput)
    {
        const std::filesystem::path validPath = MakeTemporaryPath("valid.json");
        {
            std::ofstream stream(validPath, std::ios::binary);
            ASSERT_TRUE(stream.good());
            stream << MakeManifest();
        }

        RVX::SampleManifestCatalog catalog;
        std::string error;
        EXPECT_TRUE(
            RVX::SampleManifestParser::LoadFile(validPath, catalog, &error))
            << error;
        EXPECT_NE(catalog.Find("interior-rendering"), nullptr);

        std::error_code removeError;
        std::filesystem::remove(validPath, removeError);

        const std::filesystem::path missingPath =
            MakeTemporaryPath("missing.json");
        EXPECT_FALSE(RVX::SampleManifestParser::LoadFile(
            missingPath, catalog, &error));
        EXPECT_NE(error.find("Failed to open"), std::string::npos);

        const std::filesystem::path oversizedPath =
            MakeTemporaryPath("oversized.json");
        {
            std::ofstream stream(oversizedPath, std::ios::binary);
            ASSERT_TRUE(stream.good());
            const std::string oversized(
                RVX::SampleManifestParser::MaximumDocumentBytes + 1u, ' ');
            stream.write(oversized.data(),
                         static_cast<std::streamsize>(oversized.size()));
        }
        EXPECT_FALSE(RVX::SampleManifestParser::LoadFile(
            oversizedPath, catalog, &error));
        EXPECT_NE(error.find("1 MiB"), std::string::npos);
        std::filesystem::remove(oversizedPath, removeError);
    }
} // namespace
