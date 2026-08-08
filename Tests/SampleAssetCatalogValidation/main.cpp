#include "Samples/SampleAssetCatalog.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    class TemporaryCatalog
    {
    public:
        TemporaryCatalog()
        {
            const auto suffix =
                std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = std::filesystem::temp_directory_path() /
                     ("RVX_SampleAssetCatalog_" + std::to_string(suffix));
            std::filesystem::create_directories(m_root / "models");
        }

        ~TemporaryCatalog()
        {
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
        }

        void Write(const std::filesystem::path& relative,
                   const std::string& contents) const
        {
            const std::filesystem::path path = m_root / relative;
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << contents;
        }

        const std::filesystem::path& Root() const { return m_root; }
        std::filesystem::path CatalogPath() const
        {
            return m_root / "catalog.json";
        }

    private:
        std::filesystem::path m_root;
    };

    std::string MakeCatalog(const std::string& entries)
    {
        return "{\n"
               "  \"schemaId\": \"RVX.SampleAssetCatalog\",\n"
               "  \"schemaVersion\": 1,\n"
               "  \"assets\": [" +
               entries + "]\n"
                         "}\n";
    }

    std::string MakeEntry(const std::string& id,
                          const std::string& path)
    {
        return "{"
               "\"id\":\"" + id + "\","
               "\"kind\":\"model\","
               "\"path\":\"" + path + "\","
               "\"license\":{"
               "\"spdxId\":\"LicenseRef-Test\","
               "\"file\":\"license.txt\"},"
               "\"source\":{"
               "\"name\":\"generated fixture\","
               "\"uri\":\"repo://fixture\","
               "\"author\":\"RenderVerseX tests\"},"
               "\"redistributable\":true"
               "}";
    }

    TEST(SampleAssetCatalogValidation, LoadsCompleteCatalogDeterministically)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/zeta.gltf", "zeta");
        fixture.Write("models/alpha.gltf", "alpha");
        fixture.Write("license.txt", "test license");
        fixture.Write("catalog.json",
                      MakeCatalog(MakeEntry("zeta", "models/zeta.gltf") +
                                  "," +
                                  MakeEntry("alpha", "models/alpha.gltf")));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error))
            << error;
        const auto entries = catalog.List();
        ASSERT_EQ(entries.size(), 2u);
        EXPECT_EQ(entries[0].id, "alpha");
        EXPECT_EQ(entries[1].id, "zeta");

        const RVX::SampleAssetEntry* alpha = catalog.Find("alpha");
        ASSERT_NE(alpha, nullptr);
        EXPECT_EQ(alpha->kind, RVX::SampleAssetKind::Model);
        EXPECT_EQ(alpha->license.spdxId, "LicenseRef-Test");
        EXPECT_EQ(alpha->source.uri, "repo://fixture");
        EXPECT_TRUE(alpha->redistributable);
        EXPECT_TRUE(std::filesystem::is_regular_file(alpha->resolvedPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(
            alpha->license.resolvedFile));
    }

    TEST(SampleAssetCatalogValidation, RejectsTraversalAndMissingFiles)
    {
        TemporaryCatalog fixture;
        fixture.Write("license.txt", "test license");
        fixture.Write("catalog.json",
                      MakeCatalog(MakeEntry("escape", "../outside.gltf")));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("traversal-free"), std::string::npos) << error;

        fixture.Write("catalog.json",
                      MakeCatalog(MakeEntry("missing", "models/missing.gltf")));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("Missing asset file"), std::string::npos) << error;
    }

    TEST(SampleAssetCatalogValidation, RejectsDuplicateAndIncompleteProvenance)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/model.gltf", "model");
        fixture.Write("license.txt", "test license");
        const std::string entry = MakeEntry("model", "models/model.gltf");
        fixture.Write("catalog.json", MakeCatalog(entry + "," + entry));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("Duplicate"), std::string::npos);

        fixture.Write(
            "catalog.json",
            MakeCatalog(
                "{\"id\":\"model\",\"kind\":\"model\","
                "\"path\":\"models/model.gltf\","
                "\"redistributable\":true}"));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("requires license, source"), std::string::npos);
    }

    TEST(SampleAssetCatalogValidation, LoadsEnvironmentEntriesByDeclaredKind)
    {
        TemporaryCatalog fixture;
        fixture.Write("environments/studio.hdr", "hdr fixture");
        fixture.Write("license.txt", "test license");
        fixture.Write(
            "catalog.json",
            MakeCatalog(
                "{\"id\":\"studio\",\"kind\":\"environment\","
                "\"path\":\"environments/studio.hdr\","
                "\"license\":{\"spdxId\":\"CC0-1.0\","
                "\"file\":\"license.txt\"},"
                "\"source\":{\"name\":\"studio fixture\","
                "\"uri\":\"repo://studio\","
                "\"author\":\"RenderVerseX tests\"},"
                "\"redistributable\":true}"));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error))
            << error;
        const RVX::SampleAssetEntry* environment = catalog.Find("studio");
        ASSERT_NE(environment, nullptr);
        EXPECT_EQ(environment->kind, RVX::SampleAssetKind::Environment);
        EXPECT_TRUE(environment->redistributable);
    }
} // namespace
