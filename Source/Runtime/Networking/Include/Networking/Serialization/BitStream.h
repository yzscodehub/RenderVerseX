#pragma once

/**
 * @file BitStream.h
 * @brief Bit-level read/write stream for network serialization
 * 
 * Provides efficient bit-packing for network data, allowing
 * sub-byte values to be serialized without wasting bandwidth.
 */

#include "Core/Types.h"

#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <cstring>
#include <stdexcept>

namespace RVX::Net
{
    /**
     * @brief Read-only bit stream for deserializing network data
     */
    class BitReader
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================
        
        BitReader() = default;
        
        /**
         * @brief Create reader from data buffer
         * @param data Pointer to data
         * @param sizeBytes Size in bytes
         */
        BitReader(const uint8* data, uint32 sizeBytes);
        
        /**
         * @brief Create reader from span
         */
        explicit BitReader(std::span<const uint8> data);
        
        // =====================================================================
        // Bit Operations
        // =====================================================================
        
        /**
         * @brief Read a single bit
         * @return true if bit is 1, false if 0
         */
        bool ReadBit();
        
        /**
         * @brief Read multiple bits (up to 32)
         * @param numBits Number of bits to read (1-32)
         * @return Value containing the bits
         */
        uint32 ReadBits(uint32 numBits);
        
        /**
         * @brief Read bits into a 64-bit value
         */
        uint64 ReadBits64(uint32 numBits);
        
        // =====================================================================
        // Byte-Aligned Reads
        // =====================================================================
        
        /**
         * @brief Align to next byte boundary
         */
        void AlignToByte();
        
        /**
         * @brief Read a single byte (aligned)
         */
        uint8 ReadByte();
        
        /**
         * @brief Read bytes into buffer
         */
        void ReadBytes(uint8* dest, uint32 count);
        
        /**
         * @brief Read bytes as span
         */
        std::span<const uint8> ReadBytesSpan(uint32 count);
        
        // =====================================================================
        // Typed Reads
        // =====================================================================
        
        bool ReadBool();
        int8 ReadInt8();
        uint8 ReadUInt8();
        int16 ReadInt16();
        uint16 ReadUInt16();
        int32 ReadInt32();
        uint32 ReadUInt32();
        int64 ReadInt64();
        uint64 ReadUInt64();
        float32 ReadFloat32();
        float64 ReadFloat64();
        
        /**
         * @brief Read a string (length-prefixed)
         */
        std::string ReadString();
        
        /**
         * @brief Read a fixed-length string
         */
        std::string ReadFixedString(uint32 maxLength);
        
        /**
         * @brief Read a variable-length encoded integer
         */
        uint32 ReadVarInt();
        uint64 ReadVarInt64();
        
        /**
         * @brief Read a compressed float in range [0, 1]
         */
        float32 ReadNormalizedFloat(uint32 bits);
        
        /**
         * @brief Read a compressed float in range [min, max]
         */
        float32 ReadRangedFloat(float32 min, float32 max, uint32 bits);
        
        /**
         * @brief Read a compressed integer in range [min, max]
         */
        int32 ReadRangedInt(int32 min, int32 max);
        
        // =====================================================================
        // State
        // =====================================================================
        
        /**
         * @brief Get current bit position
         */
        uint32 GetBitPosition() const { return m_bitPosition; }
        
        /**
         * @brief Get total bits in stream
         */
        uint32 GetTotalBits() const { return m_totalBits; }
        
        /**
         * @brief Get remaining bits
         */
        uint32 GetRemainingBits() const { return m_totalBits - m_bitPosition; }
        
        /**
         * @brief Check if stream has been read past end
         */
        bool HasOverflowed() const { return m_overflow; }
        
        /**
         * @brief Check if at end of stream
         */
        bool IsAtEnd() const { return m_bitPosition >= m_totalBits; }
        
        /**
         * @brief Reset to beginning
         */
        void Reset();
        
    private:
        const uint8* m_data = nullptr;
        uint32 m_totalBits = 0;
        uint32 m_bitPosition = 0;
        bool m_overflow = false;
    };

    /**
     * @brief Write-only bit stream for serializing network data
     */
    class BitWriter
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================
        
        /**
         * @brief Create writer with initial capacity
         */
        explicit BitWriter(uint32 initialCapacityBytes = 256);
        
        /**
         * @brief Create writer that writes to external buffer
         */
        BitWriter(uint8* buffer, uint32 capacityBytes);
        
        // =====================================================================
        // Bit Operations
        // =====================================================================
        
        /**
         * @brief Write a single bit
         */
        void WriteBit(bool value);
        
        /**
         * @brief Write multiple bits (up to 32)
         * @param value Value containing the bits
         * @param numBits Number of bits to write (1-32)
         */
        void WriteBits(uint32 value, uint32 numBits);
        
        /**
         * @brief Write bits from 64-bit value
         */
        void WriteBits64(uint64 value, uint32 numBits);
        
        // =====================================================================
        // Byte-Aligned Writes
        // =====================================================================
        
        /**
         * @brief Pad with zeros to next byte boundary
         */
        void AlignToByte();
        
        /**
         * @brief Write a single byte
         */
        void WriteByte(uint8 value);
        
        /**
         * @brief Write bytes from buffer
         */
        void WriteBytes(const uint8* src, uint32 count);
        
        /**
         * @brief Write bytes from span
         */
        void WriteBytes(std::span<const uint8> data);
        
        // =====================================================================
        // Typed Writes
        // =====================================================================
        
        void WriteBool(bool value);
        void WriteInt8(int8 value);
        void WriteUInt8(uint8 value);
        void WriteInt16(int16 value);
        void WriteUInt16(uint16 value);
        void WriteInt32(int32 value);
        void WriteUInt32(uint32 value);
        void WriteInt64(int64 value);
        void WriteUInt64(uint64 value);
        void WriteFloat32(float32 value);
        void WriteFloat64(float64 value);
        
        /**
         * @brief Write a string (length-prefixed)
         */
        void WriteString(std::string_view str);
        
        /**
         * @brief Write a fixed-length string (padded/truncated)
         */
        void WriteFixedString(std::string_view str, uint32 maxLength);
        
        /**
         * @brief Write a variable-length encoded integer
         */
        void WriteVarInt(uint32 value);
        void WriteVarInt64(uint64 value);
        
        /**
         * @brief Write a normalized float [0, 1] with specified precision
         */
        void WriteNormalizedFloat(float32 value, uint32 bits);
        
        /**
         * @brief Write a float in range [min, max] with specified precision
         */
        void WriteRangedFloat(float32 value, float32 min, float32 max, uint32 bits);
        
        /**
         * @brief Write an integer in range [min, max] (uses minimal bits)
         */
        void WriteRangedInt(int32 value, int32 min, int32 max);
        
        // =====================================================================
        // State
        // =====================================================================
        
        /**
         * @brief Get current bit position
         */
        uint32 GetBitPosition() const { return m_bitPosition; }
        
        /**
         * @brief Get bytes written (rounded up)
         */
        uint32 GetBytesWritten() const { return (m_bitPosition + 7) / 8; }
        
        /**
         * @brief Get the data buffer
         */
        const uint8* GetData() const;
        
        /**
         * @brief Get data as span
         */
        std::span<const uint8> GetSpan() const;
        
        /**
         * @brief Get data as vector (copies if using external buffer)
         */
        std::vector<uint8> ToVector() const;
        
        /**
         * @brief Check if buffer overflowed
         */
        bool HasOverflowed() const { return m_overflow; }
        
        /**
         * @brief Reset writer to beginning
         */
        void Reset();
        
        /**
         * @brief Clear and reset to empty
         */
        void Clear();
        
    private:
        void EnsureCapacity(uint32 additionalBits);
        
        std::vector<uint8> m_ownedBuffer;
        uint8* m_buffer = nullptr;
        uint32 m_capacityBits = 0;
        uint32 m_bitPosition = 0;
        bool m_ownsBuffer = true;
        bool m_overflow = false;
    };

    // =========================================================================
    // Helper Functions
    // =========================================================================

    /**
     * @brief Calculate bits needed to represent a range
     */
    constexpr uint32 BitsRequired(uint32 range)
    {
        if (range <= 1) return 0;
        uint32 bits = 0;
        uint32 val = range - 1;
        while (val > 0)
        {
            val >>= 1;
            bits++;
        }
        return bits;
    }

    /**
     * @brief Calculate bits needed for a maximum value
     */
    constexpr uint32 BitsForMax(uint32 maxValue)
    {
        return BitsRequired(maxValue + 1);
    }

} // namespace RVX::Net
