/** @file SHA256.cpp  @brief Streaming SHA-256 content hash implementation */

#include "Core/Hash/SHA256.h"

#include <algorithm>
#include <cstring>

namespace RVX::Hash
{
namespace
{
    constexpr std::array<uint32, 64> s_roundConstants{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    constexpr uint32 RotateRight(uint32 value, uint32 bits)
    {
        return (value >> bits) | (value << (32u - bits));
    }

    constexpr uint32 Choose(uint32 x, uint32 y, uint32 z)
    {
        return (x & y) ^ (~x & z);
    }

    constexpr uint32 Majority(uint32 x, uint32 y, uint32 z)
    {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    constexpr uint32 BigSigma0(uint32 value)
    {
        return RotateRight(value, 2) ^ RotateRight(value, 13) ^ RotateRight(value, 22);
    }

    constexpr uint32 BigSigma1(uint32 value)
    {
        return RotateRight(value, 6) ^ RotateRight(value, 11) ^ RotateRight(value, 25);
    }

    constexpr uint32 SmallSigma0(uint32 value)
    {
        return RotateRight(value, 7) ^ RotateRight(value, 18) ^ (value >> 3u);
    }

    constexpr uint32 SmallSigma1(uint32 value)
    {
        return RotateRight(value, 17) ^ RotateRight(value, 19) ^ (value >> 10u);
    }
} // namespace

SHA256Hasher::SHA256Hasher()
    : m_state{0x6a09e667u,
              0xbb67ae85u,
              0x3c6ef372u,
              0xa54ff53au,
              0x510e527fu,
              0x9b05688cu,
              0x1f83d9abu,
              0x5be0cd19u}
{
}

void SHA256Hasher::Update(const void* data, size_t byteCount)
{
    if (byteCount == 0)
    {
        return;
    }

    const auto* bytes = static_cast<const uint8*>(data);
    if (bytes == nullptr)
    {
        return;
    }

    m_totalBytes += static_cast<uint64>(byteCount);
    while (byteCount > 0)
    {
        const size_t copySize = std::min(byteCount, m_buffer.size() - m_bufferSize);
        std::memcpy(m_buffer.data() + m_bufferSize, bytes, copySize);
        m_bufferSize += copySize;
        bytes += copySize;
        byteCount -= copySize;

        if (m_bufferSize == m_buffer.size())
        {
            Transform(m_buffer.data());
            m_bufferSize = 0;
        }
    }
}

void SHA256Hasher::Update(std::string_view value)
{
    Update(value.data(), value.size());
}

SHA256Digest SHA256Hasher::FinalizeDigest() const
{
    SHA256Hasher copy = *this;
    return copy.FinalizeInPlace();
}

std::string SHA256Hasher::FinalizeHex() const
{
    return FormatSHA256Digest(FinalizeDigest());
}

void SHA256Hasher::Transform(const uint8 block[64])
{
    std::array<uint32, 64> words{};
    for (size_t index = 0; index < 16; ++index)
    {
        const size_t offset = index * 4;
        words[index] = (static_cast<uint32>(block[offset]) << 24u) |
                       (static_cast<uint32>(block[offset + 1]) << 16u) |
                       (static_cast<uint32>(block[offset + 2]) << 8u) |
                       static_cast<uint32>(block[offset + 3]);
    }
    for (size_t index = 16; index < words.size(); ++index)
    {
        words[index] = SmallSigma1(words[index - 2]) + words[index - 7] +
                       SmallSigma0(words[index - 15]) + words[index - 16];
    }

    uint32 a = m_state[0];
    uint32 b = m_state[1];
    uint32 c = m_state[2];
    uint32 d = m_state[3];
    uint32 e = m_state[4];
    uint32 f = m_state[5];
    uint32 g = m_state[6];
    uint32 h = m_state[7];
    for (size_t index = 0; index < words.size(); ++index)
    {
        const uint32 temp1 = h + BigSigma1(e) + Choose(e, f, g) +
                             s_roundConstants[index] + words[index];
        const uint32 temp2 = BigSigma0(a) + Majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

SHA256Digest SHA256Hasher::FinalizeInPlace()
{
    const uint64 bitCount = m_totalBytes * 8u;
    const uint8 padding = 0x80u;
    Update(&padding, 1);

    const uint8 zero = 0;
    while (m_bufferSize != 56)
    {
        Update(&zero, 1);
    }

    std::array<uint8, 8> lengthBytes{};
    for (size_t index = 0; index < lengthBytes.size(); ++index)
    {
        const uint32 shift = static_cast<uint32>((lengthBytes.size() - 1 - index) * 8);
        lengthBytes[index] = static_cast<uint8>(bitCount >> shift);
    }
    Update(lengthBytes.data(), lengthBytes.size());

    SHA256Digest digest{};
    for (size_t index = 0; index < m_state.size(); ++index)
    {
        const size_t offset = index * 4;
        digest[offset] = static_cast<uint8>(m_state[index] >> 24u);
        digest[offset + 1] = static_cast<uint8>(m_state[index] >> 16u);
        digest[offset + 2] = static_cast<uint8>(m_state[index] >> 8u);
        digest[offset + 3] = static_cast<uint8>(m_state[index]);
    }
    return digest;
}

std::string FormatSHA256Digest(const SHA256Digest& digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (size_t index = 0; index < digest.size(); ++index)
    {
        result[index * 2] = digits[digest[index] >> 4u];
        result[index * 2 + 1] = digits[digest[index] & 0x0fu];
    }
    return result;
}

SHA256Digest ComputeSHA256(const void* data, size_t byteCount)
{
    SHA256Hasher hasher;
    hasher.Update(data, byteCount);
    return hasher.FinalizeDigest();
}

SHA256Digest ComputeSHA256(std::string_view value)
{
    return ComputeSHA256(value.data(), value.size());
}
} // namespace RVX::Hash
