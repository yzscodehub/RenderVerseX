#include "ResourceRuntime/ResourceLoaderCatalog.h"
#include "ResourceRuntime/ResourceManagerDependencies.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    class MatchingLoader final : public RVX::Resource::IResourceLoader
    {
    public:
        MatchingLoader(
            RVX::ResourceType type,
            RVX::ResourceLoadDomain domain,
            std::string suffix)
            : m_type(type),
              m_domain(domain),
              m_suffix(std::move(suffix))
        {
        }

        RVX::ResourceType GetResourceType() const override { return m_type; }
        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {m_suffix};
        }
        bool Prepare(
            const RVX::Resource::ResourceLoadPreparationContext&,
            RVX::Resource::PreparedResourceBundle&,
            RVX::Resource::ResourceLoadError&) override
        {
            return false;
        }
        bool CapturePreparationState(
            RVX::uint64 requestedHash,
            RVX::Resource::ResourceLoadPreparationStateRef& outState,
            RVX::uint64& outCanonicalHash,
            RVX::Resource::ResourceLoadError&) const override
        {
            outState.reset();
            outCanonicalHash = requestedHash;
            return true;
        }
        bool ValidatePreparationState(
            RVX::uint64 requestedHash,
            RVX::Resource::ResourceLoadPreparationStateRef,
            RVX::Resource::ResourceLoadPreparationStateRef& outState,
            RVX::uint64& outCanonicalHash,
            RVX::Resource::ResourceLoadError&) const override
        {
            outState.reset();
            outCanonicalHash = requestedHash;
            return true;
        }
        bool Supports(
            const RVX::ResourceLoaderRequest& request) const noexcept override
        {
            return request.type == m_type &&
                request.domain == m_domain &&
                request.resolvedPath.ends_with(m_suffix);
        }

    private:
        RVX::ResourceType m_type = RVX::ResourceType::Unknown;
        RVX::ResourceLoadDomain m_domain =
            RVX::ResourceLoadDomain::Unknown;
        std::string m_suffix;
    };

    void AddLoader(
        RVX::ResourceLoaderCatalog& catalog,
        std::unique_ptr<RVX::Resource::IResourceLoader> loader)
    {
        std::string error;
        ASSERT_TRUE(catalog.Add(std::move(loader), error)) << error;
    }

    void Seal(RVX::ResourceLoaderCatalog& catalog)
    {
        std::string error;
        ASSERT_TRUE(catalog.Seal(error)) << error;
    }
} // namespace

static_assert(!std::is_copy_constructible_v<RVX::ResourceLoaderCatalog>);
static_assert(std::is_move_constructible_v<RVX::ResourceLoaderCatalog>);
static_assert(!std::is_copy_constructible_v<RVX::ResourceManagerDependencies>);
static_assert(std::is_move_constructible_v<RVX::ResourceManagerDependencies>);

TEST(ResourceLoaderCatalogValidation, ResolvesExactlyOneTypeDomainAndPathMatch)
{
    RVX::ResourceLoaderCatalog catalog;
    AddLoader(catalog, std::make_unique<MatchingLoader>(
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Source,
        ".png"));
    AddLoader(catalog, std::make_unique<MatchingLoader>(
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Cooked,
        ".rva"));
    Seal(catalog);

    const RVX::ResourceLoaderResolveResult source = catalog.Resolve({
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Source,
        "Assets/Color.png"});
    ASSERT_TRUE(source.Succeeded()) << source.diagnostic;
    EXPECT_EQ(source.matchCount, 1u);

    const RVX::ResourceLoaderResolveResult cooked = catalog.Resolve({
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Cooked,
        "Cooked/Color.rva"});
    ASSERT_TRUE(cooked.Succeeded()) << cooked.diagnostic;
    EXPECT_EQ(cooked.matchCount, 1u);
    EXPECT_NE(source.loader, cooked.loader);
}

TEST(ResourceLoaderCatalogValidation, MissingMatchFailsClosed)
{
    RVX::ResourceLoaderCatalog catalog;
    AddLoader(catalog, std::make_unique<MatchingLoader>(
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Source,
        ".png"));
    Seal(catalog);

    const RVX::ResourceLoaderResolveResult result = catalog.Resolve({
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Package,
        "Base/Color.rva"});
    EXPECT_EQ(result.status, RVX::ResourceLoaderResolveStatus::NotFound);
    EXPECT_EQ(result.matchCount, 0u);
    EXPECT_EQ(result.loader, nullptr);
    EXPECT_FALSE(result.diagnostic.empty());
}

TEST(ResourceLoaderCatalogValidation, MultipleMatchesFailClosed)
{
    RVX::ResourceLoaderCatalog catalog;
    AddLoader(catalog, std::make_unique<MatchingLoader>(
        RVX::ResourceType::Shader,
        RVX::ResourceLoadDomain::Cooked,
        ".rvxshader"));
    AddLoader(catalog, std::make_unique<MatchingLoader>(
        RVX::ResourceType::Shader,
        RVX::ResourceLoadDomain::Cooked,
        ".rvxshader"));
    Seal(catalog);

    const RVX::ResourceLoaderResolveResult result = catalog.Resolve({
        RVX::ResourceType::Shader,
        RVX::ResourceLoadDomain::Cooked,
        "Cooked/program.rvxshader"});
    EXPECT_EQ(result.status, RVX::ResourceLoaderResolveStatus::Ambiguous);
    EXPECT_EQ(result.matchCount, 2u);
    EXPECT_EQ(result.loader, nullptr);
    EXPECT_FALSE(result.diagnostic.empty());
}

TEST(ResourceLoaderCatalogValidation, InvalidAndUnknownLoaderContractsAreRejected)
{
    RVX::ResourceLoaderCatalog catalog;
    std::string error;
    EXPECT_FALSE(catalog.Add(nullptr, error));
    EXPECT_FALSE(error.empty());

    EXPECT_FALSE(catalog.Add(
        std::make_unique<MatchingLoader>(
            RVX::ResourceType::Unknown,
            RVX::ResourceLoadDomain::Source,
            ".unknown"),
        error));
    EXPECT_FALSE(error.empty());

    Seal(catalog);
    const RVX::ResourceLoaderResolveResult invalid = catalog.Resolve({});
    EXPECT_EQ(
        invalid.status,
        RVX::ResourceLoaderResolveStatus::InvalidRequest);
    EXPECT_EQ(invalid.loader, nullptr);
}

TEST(ResourceLoaderCatalogValidation, SealedCatalogRejectsMutation)
{
    RVX::ResourceLoaderCatalog catalog;
    AddLoader(catalog, std::make_unique<MatchingLoader>(
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Source,
        ".png"));
    Seal(catalog);

    std::string error;
    EXPECT_FALSE(catalog.Add(
        std::make_unique<MatchingLoader>(
            RVX::ResourceType::Texture,
            RVX::ResourceLoadDomain::Source,
            ".jpg"),
        error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(catalog.Seal(error));
}
