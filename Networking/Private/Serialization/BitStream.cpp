#include "Networking/Serialization/BitStream.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace RVX::Net
{
    // =========================================================================
    // BitReader Implementation
    // =========================================================================

    BitReader::BitReader(const uint8* data, uint32 sizeBytes)
        : m_data(data)
        , m_totalBits(sizeBytes * 8)
        , m_bitPosition(0)
        , m_overflow(false)
    {
    }

    BitReader::BitReader(std::span<const uint8> data)
        : BitReader(data.data(), static_cast<uint32>(data.size()))
    {
    }

    bool BitReader::ReadBit()
    {
        if (m_bitPosition >= m_totalBits)
        {
            m_overflow = true;
            return false;
        }

        uint32 byteIndex = m_bitPosition / 8;
        uint32 bitIndex = m_bitPosition % 8;
        m_bitPosition++;

        return (m_data[byteIndex] >> bitIndex) & 1;
    }

    uint32 BitReader::ReadBits(uint32 numBits)
    {
        if (numBits == 0) return 0;
        if (numBits > 32) numBits = 32;

        if (m_bitPosition + numBits > m_totalBits)
        {
            m_overflow = true;
            return 0;
        }

        uint32 result = 0;
        for (uint32 i = 0; i < numBits; ++i)
        {
            if (ReadBit())
            {
                result |= (1u << i);
            }
        }
        return result;
    }

    uint64 BitReader::ReadBits64(uint32 numBits)
    {
        if (numBits == 0) return 0;
        if (numBits > 64) numBits = 64;

        if (numBits <= 32)
        {
            return ReadBits(numBits);
        }

        uint64 low = ReadBits(32);
        uint64 high = ReadBits(numBits - 32);
        return low | (high << 32);
    }

    void BitReader::AlignToByte()
    {
        uint32 remainder = m_bitPosition % 8;
        if (remainder != 0)
        {
            m_bitPosition += (8 - remainder);
        }
    }

    uint8 BitReader::ReadByte()
    {
        AlignToByte();
        return static_cast<uint8>(ReadBits(8));
    }

    void BitReader::ReadBytes(uint8* dest, uint32 count)
    {
        AlignToByte();

        uint32 byteIndex = m_bitPosition / 8;
        if (byteIndex + count > m_totalBits / 8)
        {
            m_overflow = true;
            return;
        }

        std::memcpy(dest, m_data + byteIndex, count);
        m_bitPosition += count * 8;
    }

    std::span<const uint8> BitReader::ReadBytesSpan(uint32 count)
    {
        AlignToByte();

        uint32 byteIndex = m_bitPosition / 8;
        if (byteIndex + count > m_totalBits / 8)
        {
            m_overflow = true;
            return {};
        }

        m_bitPosition += count * 8;
        return { m_data + byteIndex, count };
    }

    bool BitReader::ReadBool()
    {
        return ReadBit();
    }

    int8 BitReader::ReadInt8()
    {
        return static_cast<int8>(ReadBits(8));
    }

    uint8 BitReader::ReadUInt8()
    {
        return static_cast<uint8>(ReadBits(8));
    }

    int16 BitReader::ReadInt16()
    {
        return static_cast<int16>(ReadBits(16));
    }

    uint16 BitReader::ReadUInt16()
    {
        return static_cast<uint16>(ReadBits(16));
    }

    int32 BitReader::ReadInt32()
    {
        return static_cast<int32>(ReadBits(32));
    }

    uint32 BitReader::ReadUInt32()
    {
        return ReadBits(32);
    }

    int64 BitReader::ReadInt64()
    {
        return static_cast<int64>(ReadBits64(64));
    }

    uint64 BitReader::ReadUInt64()
    {
        return ReadBits64(64);
    }

    float32 BitReader::ReadFloat32()
    {
        uint32 bits = ReadBits(32);
        float32 result;
        std::memcpy(&result, &bits, sizeof(float32));
        return result;
    }

    float64 BitReader::ReadFloat64()
    {
        uint64 bits = ReadBits64(64);
        float64 result;
        std::memcpy(&result, &bits, sizeof(float64));
        return result;
    }

    std::string BitReader::ReadString()
    {
        uint32 length = ReadVarInt();
        if (m_overflow || length == 0)
            return {};

        if (length > 65535)
        {
            m_overflow = true;
            return {};
        }

        AlignToByte();
        uint32 byteIndex = m_bitPosition / 8;
        
        if (byteIndex + length > m_totalBits / 8)
        {
            m_overflow = true;
            return {};
        }

        std::string result(reinterpret_cast<const char*>(m_data + byteIndex), length);
        m_bitPosition += length * 8;
        return result;
    }

    std::string BitReader::ReadFixedString(uint32 maxLength)
    {
        AlignToByte();
        uint32 byteIndex = m_bitPosition / 8;

        if (byteIndex + maxLength > m_totalBits / 8)
        {
            m_overflow = true;
            return {};
        }

        // Find null terminator or end of fixed length
        uint32 actualLength = 0;
        while (actualLength < maxLength && m_data[byteIndex + actualLength] != 0)
        {
            actualLength++;
        }

        std::string result(reinterpret_cast<const char*>(m_data + byteIndex), actualLength);
        m_bitPosition += maxLength * 8; // Always consume full fixed length
        return result;
    }

    uint32 BitReader::ReadVarInt()
    {
        uint32 result = 0;
        uint32 shift = 0;

        while (shift < 35)
        {
            uint8 byte = ReadUInt8();
            if (m_overflow) return 0;

            result |= static_cast<uint32>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0)
                break;
            shift += 7;
        }

        return result;
    }

    uint64 BitReader::ReadVarInt64()
    {
        uint64 result = 0;
        uint32 shift = 0;

        while (shift < 70)
        {
            uint8 byte = ReadUInt8();
            if (m_overflow) return 0;

            result |= static_cast<uint64>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0)
                break;
            shift += 7;
        }

        return result;
    }

    float32 BitReader::ReadNormalizedFloat(uint32 bits)
    {
        uint32 maxValue = (1u << bits) - 1;
        uint32 value = ReadBits(bits);
        return static_cast<float32>(value) / static_cast<float32>(maxValue);
    }

    float32 BitReader::ReadRangedFloat(float32 min, float32 max, uint32 bits)
    {
        float32 normalized = ReadNormalizedFloat(bits);
        return min + normalized * (max - min);
    }

    int32 BitReader::ReadRangedInt(int32 min, int32 max)
    {
        uint32 range = static_cast<uint32>(max - min);
        uint32 bitsNeeded = BitsRequired(range + 1);
        uint32 value = ReadBits(bitsNeeded);
        return min + static_cast<int32>(std::min(value, range));
    }

    void BitReader::Reset()
    {
        m_bitPosition = 0;
        m_overflow = false;
    }

    // =========================================================================
    // BitWriter Implementation
    // =========================================================================

    BitWriter::BitWriter(uint32 initialCapacityBytes)
        : m_ownsBuffer(true)
        , m_overflow(false)
    {
        m_ownedBuffer.resize(initialCapacityBytes);
        m_buffer = m_ownedBuffer.data();
        m_capacityBits = initialCapacityBytes * 8;
        std::memset(m_buffer, 0, initialCapacityBytes);
    }

    BitWriter::BitWriter(uint8* buffer, uint32 capacityBytes)
        : m_buffer(buffer)
        , m_capacityBits(capacityBytes * 8)
        , m_bitPosition(0)
        , m_ownsBuffer(false)
        , m_overflow(false)
    {
        std::memset(m_buffer, 0, capacityBytes);
    }

    void BitWriter::EnsureCapacity(uint32 additionalBits)
    {
        uint32 requiredBits = m_bitPosition + additionalBits;
        if (requiredBits <= m_capacityBits)
            return;

        if (!m_ownsBuffer)
        {
            m_overflow = true;
            return;
        }

        // Grow by 2x or to required size, whichever is larger
        uint32 newCapacityBytes = std::max(
            (m_capacityBits / 8) * 2,
            (requiredBits + 7) / 8
        );
        
        m_ownedBuffer.resize(newCapacityBytes);
        m_buffer = m_ownedBuffer.data();
        m_capacityBits = newCapacityBytes * 8;
    }

    void BitWriter::WriteBit(bool value)
    {
        EnsureCapacity(1);
        if (m_overflow) return;

        if (value)
        {
            uint32 byteIndex = m_bitPosition / 8;
            uint32 bitIndex = m_bitPosition % 8;
            m_buffer[byteIndex] |= (1u << bitIndex);
        }
        m_bitPosition++;
    }

    void BitWriter::WriteBits(uint32 value, uint32 numBits)
    {
        if (numBits == 0) return;
        if (numBits > 32) numBits = 32;

        EnsureCapacity(numBits);
        if (m_overflow) return;

        for (uint32 i = 0; i < numBits; ++i)
        {
            WriteBit((value >> i) & 1);
        }
    }

    void BitWriter::WriteBits64(uint64 value, uint32 numBits)
    {
        if (numBits == 0) return;
        if (numBits > 64) numBits = 64;

        if (numBits <= 32)
        {
            WriteBits(static_cast<uint32>(value), numBits);
            return;
        }

        WriteBits(static_cast<uint32>(value), 32);
        WriteBits(static_cast<uint32>(value >> 32), numBits - 32);
    }

    void BitWriter::AlignToByte()
    {
        uint32 remainder = m_bitPosition % 8;
        if (remainder != 0)
        {
            m_bitPosition += (8 - remainder);
        }
    }

    void BitWriter::WriteByte(uint8 value)
    {
        AlignToByte();
        WriteBits(value, 8);
    }

    void BitWriter::WriteBytes(const uint8* src, uint32 count)
    {
        AlignToByte();
        EnsureCapacity(count * 8);
        if (m_overflow) return;

        uint32 byteIndex = m_bitPosition / 8;
        std::memcpy(m_buffer + byteIndex, src, count);
        m_bitPosition += count * 8;
    }

    void BitWriter::WriteBytes(std::span<const uint8> data)
    {
        WriteBytes(data.data(), static_cast<uint32>(data.size()));
    }

    void BitWriter::WriteBool(bool value)
    {
        WriteBit(value);
    }

    void BitWriter::WriteInt8(int8 value)
    {
        WriteBits(static_cast<uint8>(value), 8);
    }

    void BitWriter::WriteUInt8(uint8 value)
    {
        WriteBits(value, 8);
    }

    void BitWriter::WriteInt16(int16 value)
    {
        WriteBits(static_cast<uint16>(value), 16);
    }

    void BitWriter::WriteUInt16(uint16 value)
    {
        WriteBits(value, 16);
    }

    void BitWriter::WriteInt32(int32 value)
    {
        WriteBits(static_cast<uint32>(value), 32);
    }

    void BitWriter::WriteUInt32(uint32 value)
    {
        WriteBits(value, 32);
    }

    void BitWriter::WriteInt64(int64 value)
    {
        WriteBits64(static_cast<uint64>(value), 64);
    }

    void BitWriter::WriteUInt64(uint64 value)
    {
        WriteBits64(value, 64);
    }

    void BitWriter::WriteFloat32(float32 value)
    {
        uint32 bits;
        std::memcpy(&bits, &value, sizeof(float32));
        WriteBits(bits, 32);
    }

    void BitWriter::WriteFloat64(float64 value)
    {
        uint64 bits;
        std::memcpy(&bits, &value, sizeof(float64));
        WriteBits64(bits, 64);
    }

    void BitWriter::WriteString(std::string_view str)
    {
        WriteVarInt(static_cast<uint32>(str.size()));
        if (!str.empty())
        {
            AlignToByte();
            WriteBytes(reinterpret_cast<const uint8*>(str.data()),
                       static_cast<uint32>(str.size()));
        }
    }

    void BitWriter::WriteFixedString(std::string_view str, uint32 maxLength)
    {
        AlignToByte();
        EnsureCapacity(maxLength * 8);
        if (m_overflow) return;

        uint32 byteIndex = m_bitPosition / 8;
        uint32 copyLen = std::min(static_cast<uint32>(str.size()), maxLength);
        
        std::memcpy(m_buffer + byteIndex, str.data(), copyLen);
        std::memset(m_buffer + byteIndex + copyLen, 0, maxLength - copyLen);
        
        m_bitPosition += maxLength * 8;
    }

    void BitWriter::WriteVarInt(uint32 value)
    {
        while (value >= 0x80)
        {
            WriteUInt8(static_cast<uint8>(value | 0x80));
            value >>= 7;
        }
        WriteUInt8(static_cast<uint8>(value));
    }

    void BitWriter::WriteVarInt64(uint64 value)
    {
        while (value >= 0x80)
        {
            WriteUInt8(static_cast<uint8>(value | 0x80));
            value >>= 7;
        }
        WriteUInt8(static_cast<uint8>(value));
    }

    void BitWriter::WriteNormalizedFloat(float32 value, uint32 bits)
    {
        value = std::clamp(value, 0.0f, 1.0f);
        uint32 maxValue = (1u << bits) - 1;
        uint32 intValue = static_cast<uint32>(value * static_cast<float32>(maxValue) + 0.5f);
        WriteBits(std::min(intValue, maxValue), bits);
    }

    void BitWriter::WriteRangedFloat(float32 value, float32 min, float32 max, uint32 bits)
    {
        float32 normalized = (value - min) / (max - min);
        WriteNormalizedFloat(normalized, bits);
    }

    void BitWriter::WriteRangedInt(int32 value, int32 min, int32 max)
    {
        uint32 range = static_cast<uint32>(max - min);
        uint32 bitsNeeded = BitsRequired(range + 1);
        uint32 clampedValue = static_cast<uint32>(std::clamp(value, min, max) - min);
        WriteBits(clampedValue, bitsNeeded);
    }

    const uint8* BitWriter::GetData() const
    {
        return m_buffer;
    }

    std::span<const uint8> BitWriter::GetSpan() const
    {
        return { m_buffer, GetBytesWritten() };
    }

    std::vector<uint8> BitWriter::ToVector() const
    {
        return { m_buffer, m_buffer + GetBytesWritten() };
    }

    void BitWriter::Reset()
    {
        m_bitPosition = 0;
        if (m_buffer)
        {
            std::memset(m_buffer, 0, (m_capacityBits + 7) / 8);
        }
        m_overflow = false;
    }

    void BitWriter::Clear()
    {
        if (m_ownsBuffer)
        {
            m_ownedBuffer.clear();
            m_ownedBuffer.resize(256);
            m_buffer = m_ownedBuffer.data();
            m_capacityBits = 256 * 8;
        }
        m_bitPosition = 0;
        m_overflow = false;
        if (m_buffer)
        {
            std::memset(m_buffer, 0, (m_capacityBits + 7) / 8);
        }
    }

} // namespace RVX::Net
