#pragma once

/**
 * @file ResourceContentIdentity.h
 * @brief Backend-neutral identity and verification receipt for consumed asset bytes.
 */

#include "Core/Types.h"

#include <string>

namespace RVX::Resource
{
    inline constexpr uint32 RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION = 1;

    enum class ResourceContentIdentityDomain : uint8
    {
        Unknown = 0,
        Source,
        /** @brief Verified cooked product or deterministic cooked product closure. */
        CookedArtifact,
        /** @brief The exact bytes of an RVX cook manifest. */
        CookManifest,
    };

    enum class ResourceContentIdentityScope : uint8
    {
        Unknown = 0,
        SelfContainedArtifact,
        DependencyClosure,
    };

    enum class ResourceContentHashAlgorithm : uint8
    {
        None = 0,
        SHA256,
    };

    /**
     * @brief Schema-versioned identity for the exact bytes consumed by a loader.
     *
     * For the Source domain, a self-contained identity is the raw SHA-256 of
     * one consumed source file. A dependency-closure identity is a
     * schema-defined SHA-256 over normalized, URI-sorted consumed file records.
     * CookedArtifact and CookManifest use their owning schema's canonical
     * closure/raw-byte rules, but retain the same explicit scope distinction.
     */
    struct ResourceContentIdentity
    {
        uint32 schemaVersion = 0;
        ResourceContentIdentityDomain domain = ResourceContentIdentityDomain::Unknown;
        ResourceContentIdentityScope scope = ResourceContentIdentityScope::Unknown;
        ResourceContentHashAlgorithm algorithm = ResourceContentHashAlgorithm::None;
        std::string digest;
        uint64 byteCount = 0;
        uint32 fileCount = 0;

        [[nodiscard]] bool IsEmpty() const noexcept;
        [[nodiscard]] bool IsValid() const noexcept;

        [[nodiscard]] bool operator==(const ResourceContentIdentity& other) const;
        [[nodiscard]] bool operator!=(const ResourceContentIdentity& other) const
        {
            return !(*this == other);
        }
    };

    enum class ResourceContentVerificationStatus : uint8
    {
        NotRequested = 0,
        Observed,
        Verified,
    };

    /** @brief Immutable owner-thread receipt attached to a successfully published root. */
    struct ResourceContentVerificationReceipt
    {
        ResourceContentVerificationStatus status =
            ResourceContentVerificationStatus::NotRequested;
        ResourceContentIdentity expected;
        ResourceContentIdentity observed;

        [[nodiscard]] bool IsVerified() const noexcept
        {
            return status == ResourceContentVerificationStatus::Verified;
        }
    };

    [[nodiscard]] const char* GetResourceContentIdentityDomainName(
        ResourceContentIdentityDomain domain);
    [[nodiscard]] const char* GetResourceContentIdentityScopeName(
        ResourceContentIdentityScope scope);
    [[nodiscard]] const char* GetResourceContentHashAlgorithmName(
        ResourceContentHashAlgorithm algorithm);
    [[nodiscard]] const char* GetResourceContentVerificationStatusName(
        ResourceContentVerificationStatus status);
} // namespace RVX::Resource
