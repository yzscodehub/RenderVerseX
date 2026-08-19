/** @file ResourceContentIdentity.cpp  @brief Consumed-byte identity contract helpers */

#include "Resource/ResourceContentIdentity.h"

#include <cctype>

namespace RVX::Resource
{
bool ResourceContentIdentity::IsEmpty() const noexcept
{
    return schemaVersion == 0 &&
           domain == ResourceContentIdentityDomain::Unknown &&
           scope == ResourceContentIdentityScope::Unknown &&
           algorithm == ResourceContentHashAlgorithm::None &&
           digest.empty() && byteCount == 0 && fileCount == 0;
}

bool ResourceContentIdentity::IsValid() const noexcept
{
    if (schemaVersion != RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION ||
        (domain != ResourceContentIdentityDomain::Source &&
         domain != ResourceContentIdentityDomain::CookedArtifact &&
         domain != ResourceContentIdentityDomain::CookManifest) ||
        (scope != ResourceContentIdentityScope::SelfContainedArtifact &&
         scope != ResourceContentIdentityScope::DependencyClosure) ||
        algorithm != ResourceContentHashAlgorithm::SHA256 ||
        digest.size() != 64 ||
        (scope == ResourceContentIdentityScope::SelfContainedArtifact && fileCount != 1) ||
        (scope == ResourceContentIdentityScope::DependencyClosure && fileCount < 2))
    {
        return false;
    }

    for (const unsigned char character : digest)
    {
        if (!std::isdigit(character) && (character < 'a' || character > 'f'))
        {
            return false;
        }
    }
    return true;
}

bool ResourceContentIdentity::operator==(const ResourceContentIdentity& other) const
{
    return schemaVersion == other.schemaVersion &&
           domain == other.domain &&
           scope == other.scope &&
           algorithm == other.algorithm &&
           digest == other.digest &&
           byteCount == other.byteCount &&
           fileCount == other.fileCount;
}

const char* GetResourceContentIdentityDomainName(ResourceContentIdentityDomain domain)
{
    switch (domain)
    {
        case ResourceContentIdentityDomain::Source: return "source";
        case ResourceContentIdentityDomain::CookedArtifact: return "cooked-artifact";
        case ResourceContentIdentityDomain::CookManifest: return "cook-manifest";
        default: return "unknown";
    }
}

const char* GetResourceContentIdentityScopeName(ResourceContentIdentityScope scope)
{
    switch (scope)
    {
        case ResourceContentIdentityScope::SelfContainedArtifact: return "self-contained-artifact";
        case ResourceContentIdentityScope::DependencyClosure: return "dependency-closure";
        default: return "unknown";
    }
}

const char* GetResourceContentHashAlgorithmName(ResourceContentHashAlgorithm algorithm)
{
    switch (algorithm)
    {
        case ResourceContentHashAlgorithm::SHA256: return "sha256";
        default: return "none";
    }
}

const char* GetResourceContentVerificationStatusName(ResourceContentVerificationStatus status)
{
    switch (status)
    {
        case ResourceContentVerificationStatus::NotRequested: return "not-requested";
        case ResourceContentVerificationStatus::Observed: return "observed";
        case ResourceContentVerificationStatus::Verified: return "verified";
        default: return "unknown";
    }
}
} // namespace RVX::Resource
