#include "ShaderCompiler/ShaderCacheManager.h"
#include "ShaderCompiler/ShaderCacheFormat.h"
#include "Core/Log.h"
#include <fstream>
#include <cstring>

namespace RVX
{
    // =========================================================================
    // Shader Cache Utilities
    // =========================================================================
    namespace ShaderCacheUtils
    {
        // =====================================================================
        // CRC64 Implementation for content hashing
        // =====================================================================
        namespace
        {
            constexpr uint64 CRC64_TABLE[256] = {
                0x0000000000000000ULL, 0x42F0E1EBA9EA3693ULL, 0x85E1C3D753D46D26ULL, 0xC711223CFA3E5BB5ULL,
                0x493366450E42ECDFULL, 0x0BC387AEA7A8DA4CULL, 0xCCD2A5925D9681F9ULL, 0x8E224479F47CB76AULL,
                0x9266CC8A1C85D9BEULL, 0xD0962D61B56FEF2DULL, 0x17870F5D4F51B498ULL, 0x5577EEB6E6BB820BULL,
                0xDB55AACF12C73561ULL, 0x99A54B24BB2D03F2ULL, 0x5EB4691841135847ULL, 0x1C4488F3E8F96ED4ULL,
                0x663D78FF90E185EFULL, 0x24CD9914390BB37CULL, 0xE3DCBB28C335E8C9ULL, 0xA12C5AC36ADFDE5AULL,
                0x2F0E1EBA9EA36930ULL, 0x6DFEFF5137495FA3ULL, 0xAAEFDD6DCD770416ULL, 0xE81F3C86649D3285ULL,
                0xF45BB4758C645C51ULL, 0xB6AB559E258E6AC2ULL, 0x71BA77A2DFB03177ULL, 0x334A9649765A07E4ULL,
                0xBD68D2308226B08EULL, 0xFF9833DB2BCC861DULL, 0x388911E7D1F2DDA8ULL, 0x7A79F00C7818EB3BULL,
                0xCC7AF1FF21C30BDEULL, 0x8E8A101488293D4DULL, 0x499B3228721766F8ULL, 0x0B6BD3C3DBFD506BULL,
                0x854997BA2F81E701ULL, 0xC7B975D350D10992ULL, 0x00A8573D9A3E5227ULL, 0x4258B6D6339FB4B4ULL,
                0x5E1C3D753D46D260ULL, 0x1CECDC9E94ACE4F3ULL, 0xDBFDFEA26E92BF46ULL, 0x990D1F49C77889D5ULL,
                0x172F5B3033043EBFULL, 0x55DFBADB9AEE082CULL, 0x92CE98E760D05399ULL, 0xD03E790CC93A650AULL,
                0xAA478900B1228E31ULL, 0xE8B768EB18C8B8A2ULL, 0x2FA64AD7E2F6E317ULL, 0x6D56AB3C4B1CD584ULL,
                0xE374EF45BF6062EEULL, 0xA1840EAE168A547DULL, 0x66952C92ECB40FC8ULL, 0x2465CD79455E395BULL,
                0x3821458AADA7578FULL, 0x7AD1A461044D611CULL, 0xBDC0865DFE733AA9ULL, 0xFF3067B657990C3AULL,
                0x711223CFA3E5BB50ULL, 0x33E2C2240A0F8DC3ULL, 0xF4F3E018F031D676ULL, 0xB60301F359DBE0E5ULL,
                0xDA050215EA6C212FULL, 0x98F5E3FE438617BCULL, 0x5FE4C1C2B9B84C09ULL, 0x1D14202910527A9AULL,
                0x93366450E42ECDF0ULL, 0xD1C685BB4DC4FB63ULL, 0x16D7A787B7FAA0D6ULL, 0x5427466C1E109645ULL,
                0x4863CE9FF6E9F891ULL, 0x0A932F745F03CE02ULL, 0xCD820D48A53D95B7ULL, 0x8F72ECA30CD7A324ULL,
                0x0150A8DAF8AB144EULL, 0x43A04931514122DDULL, 0x84B16B0DAB7F7968ULL, 0xC6418AE602954FFBULL,
                0xBC387AEA7A8DA4C0ULL, 0xFEC89B01D3679253ULL, 0x39D9B93D2959C9E6ULL, 0x7B2958D680B3FF75ULL,
                0xF50B1CAF74CF481FULL, 0xB7FBFD44DD257E8CULL, 0x70EADF78271B2539ULL, 0x321A3E938EF113AAULL,
                0x2E5EB66066087D7EULL, 0x6CAE578BCFE24BEDULL, 0xABBF75B735DC1058ULL, 0xE94F945C9C3626CBULL,
                0x676DD025684A91A1ULL, 0x259D31CEC1A0A732ULL, 0xE28C13F23B9EFC87ULL, 0xA07CF2199274CA14ULL,
                0x167FF3EACBAF2AF1ULL, 0x548F120162451C62ULL, 0x939E303D987B47D7ULL, 0xD16ED1D631917144ULL,
                0x5F4C95AFC5EDC62EULL, 0x1DBC74446C07F0BDULL, 0xDAB585A2ED33F408ULL, 0x9845646BC2C7C29BULL,
                0x8401EDB865C5F04FULL, 0xC6F10C49AA2C3DDCULL, 0x01E02E755FA0D069ULL, 0x4310CFFDAA5CE6FAULL,
                0xCD328B5B17A76390ULL, 0x8FC26AB9B6B0D503ULL, 0x48D348857E8A8EB6ULL, 0x0A23A96EA7859125ULL,
                0x704A5D3F579B571EULL, 0x32BABCDB1C2594D2ULL, 0xF5AB9EC84E02C267ULL, 0xB75B7F11A68BF1F4ULL,
                0x39793B5DC7AAC69EULL, 0x7B89DA1462510E0DULL, 0xBCA0F85C5C3942B8ULL, 0xFE5019E57E4F552BULL,
                0xE214F8CAE1DA92FFULL, 0xA0E4199373F27B6CULL, 0x67F53B4FAF2E7FD9ULL, 0x2505DA1E41665B4AULL,
                0xAB279E6A38E47C20ULL, 0xE9D77F81E3634FB3ULL, 0x2EC65D8D12690906ULL, 0x6C36BC94A1DDBF95ULL,
                0x9EA83A44FEC2B9DAULL, 0xDC58DBAF2BD2F149ULL, 0x1B49F93B95C97DFCULL, 0x59B918BA3C8A0D6FULL,
                0xD79B5C93E3AA9805ULL, 0x956BBD682E58AE96ULL, 0x52789F54939A3823ULL, 0x10887E19D11A0EB0ULL,
                0x0CCC0E4E2B4FA464ULL, 0x4E3CEFA58F90A5F7ULL, 0x892DCDB98D4DEFE2ULL, 0xCBDD2C7266A63171ULL,
                0x45FF681F1FE66C1BULL, 0x070F89F452C8B288ULL, 0xC01EABC82F9C7C3DULL, 0x82EE4A238670F8AEULL,
                0xF8B51AB86E99E395ULL, 0xBA45FB6327EFD506ULL, 0x7D54D99DB48F08B3ULL, 0x3FA4385B3A6E2B20ULL,
                0xB1867C227B9C0C4AULL, 0xF3769D3D2AD0DAD9ULL, 0x3467BF01B6E93B6CULL, 0x76975EEA1F19CADFULL,
                0x6AD3D6F69D0ABE0BULL, 0x2823371C1069E798ULL, 0xEF321530BD57D02DULL, 0xADC2F4DBF4E14DBEULL,
                0x23E0B0684E3B05D4ULL, 0x6110519E4F6CC247ULL, 0xA601739B556479F2ULL, 0xE4F1927EFEE8AB61ULL,
                0x985E6C6B59F2395AULL, 0xDA0E8DC09FC18EC9ULL, 0x1D1FAF36AC82457CULL, 0x5FEF4E7DBD85ADEFULL,
                0xD1CD0AC1FA141A85ULL, 0x933DEB05B93D2C16ULL, 0x542CC909D7E61EA3ULL, 0x16DC28427E2C7930ULL,
                0x0A98A1E1B71217E4ULL, 0x4868405A20A9F077ULL, 0x8F7962343C01ABC2ULL, 0xCD898339D5FABD51ULL,
                0x43ABC7A6AC2C423BULL, 0x015B264D57F40CA8ULL, 0xC64A0451695A911DULL, 0x84BAE5BAC0B0D88EULL,
                0xFEC3BF4ED6BEABB5ULL, 0xBC335E4A7FC16726ULL, 0x7B227C7648B33C93ULL, 0x39D29DB7E1590600ULL,
                0xB7F0D961F0FDB16AULL, 0xF50038DB0818E7F9ULL, 0x32111AE522E5994CULL, 0x70E1FB4F8B5A5ADFULL,
                0x6CA573C0BA65B30BULL, 0x2E55928B72C20598ULL, 0xE944B0A7FFA85E2DULL, 0xABB4514C5350B0BEULL,
                0x259615353571A9D4ULL, 0x6766F4BC9EBB9F47ULL, 0xA077D6A86425AEF2ULL, 0xE2873734CBCF9461ULL,
                0x7EC341ECFA93BCDAULL, 0x3C33A05339D6FE49ULL, 0xFB22827FE3E80FECULL, 0xB9D26330C5F24F7FULL,
                0x37F02759CD65FB15ULL, 0x7500C6A4F4E94086ULL, 0xB211E49BAEB21B33ULL, 0xF0E1050177AE2DA0ULL,
                0xECA5AF70DF6D7C74ULL, 0xAE554E71C08370E7ULL, 0x69447C4DB5D59F52ULL, 0x2BB49D9D1CE13DC1ULL,
                0xA596D9E48CB08AABULL, 0xE766380F1E0A4838ULL, 0x20771A33B43313ADULL, 0x6287FBD832C5D63EULL,
                0x18E80B41C90B7205ULL, 0x5A18EAA2609E4496ULL, 0x9D09C8B8DA3C1F23ULL, 0xDFF929717317D9B0ULL,
                0x51DB6D08F5C6CADAULL, 0x132B8CE3ED9C6E49ULL, 0xD43AAEFFA54B37FCULL, 0x96CA4F14AD0E546FULL,
                0x8A8EC5A9DDBAB3BBULL, 0xC87E245448EC5128ULL, 0x0F6F065774BFAA9DULL, 0x4D9FE7BC9FEFE60EULL,
                0xC3BDA3A3E616FE64ULL, 0x814D4248A8F2BAF7ULL, 0x465C6053C8CDFD42ULL, 0x04AC81B8D157E9D1ULL,
            };

            uint64 ComputeCRC64(const void* data, size_t size)
            {
                uint64 crc = 0xFFFFFFFFFFFFFFFFULL;
                const uint8* bytes = static_cast<const uint8*>(data);
                for (size_t i = 0; i < size; ++i)
                {
                    crc = CRC64_TABLE[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
                }
                return crc ^ 0xFFFFFFFFFFFFFFFFULL;
            }
        }

        // =====================================================================
        // Cache File Operations
        // =====================================================================

        /** @brief Validate cache header */
        bool ValidateCacheHeader(const ShaderCacheHeader& header)
        {
            if (header.magic != RVX_SHADER_CACHE_MAGIC)
            {
                RVX_CORE_WARN("ShaderCache: Invalid magic number");
                return false;
            }

            if (header.version > RVX_SHADER_CACHE_VERSION)
            {
                RVX_CORE_WARN("ShaderCache: Version mismatch (file: {}, current: {})",
                    header.version, RVX_SHADER_CACHE_VERSION);
                return false;
            }

            return true;
        }

        /** @brief Compute content hash for cache validation */
        uint64 ComputeContentHash(const std::vector<uint8>& bytecode)
        {
            return ComputeCRC64(bytecode.data(), bytecode.size());
        }

        /** @brief Read cache file header */
        bool ReadCacheHeader(const std::filesystem::path& path, ShaderCacheHeader& outHeader)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
            {
                return false;
            }

            file.read(reinterpret_cast<char*>(&outHeader), sizeof(ShaderCacheHeader));
            return file.good() && ValidateCacheHeader(outHeader);
        }

        /** @brief Get cache statistics */
        struct CacheStats
        {
            uint64 totalFiles = 0;
            uint64 totalSize = 0;
            uint64 oldestTimestamp = UINT64_MAX;
            uint64 newestTimestamp = 0;
        };

        CacheStats GetCacheDirectoryStats(const std::filesystem::path& cacheDir)
        {
            CacheStats stats;
            std::error_code ec;

            for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec))
            {
                if (!entry.is_regular_file())
                    continue;

                if (entry.path().extension() != ".rvxs")
                    continue;

                stats.totalFiles++;
                stats.totalSize += entry.file_size(ec);

                ShaderCacheHeader header;
                if (ReadCacheHeader(entry.path(), header))
                {
                    stats.oldestTimestamp = std::min(stats.oldestTimestamp, header.timestamp);
                    stats.newestTimestamp = std::max(stats.newestTimestamp, header.timestamp);
                }
            }

            return stats;
        }

        /** @brief Generate cache filename from key */
        std::string GenerateCacheFilename(uint64 key)
        {
            std::ostringstream oss;
            oss << std::hex << std::setfill('0') << std::setw(16) << key << ".rvxs";
            return oss.str();
        }

    } // namespace ShaderCacheUtils

} // namespace RVX
