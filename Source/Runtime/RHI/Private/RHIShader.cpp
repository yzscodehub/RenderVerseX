/**
 * @file RHIShader.cpp
 * @brief Compact owned shader-interface implementation.
 */

#include "RHI/RHIShader.h"

#include <algorithm>
#include <tuple>
#include <type_traits>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr uint64 kFnvOffsetBasis = 0xcbf29ce484222325ull;
        constexpr uint64 kFnvPrime = 0x100000001b3ull;

        void HashBytes(uint64& hash, const void* data, size_t size)
        {
            const uint8* bytes = static_cast<const uint8*>(data);
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= kFnvPrime;
            }
        }

        template<typename T>
        void HashValue(uint64& hash, T value)
        {
            static_assert(std::is_integral_v<T> || std::is_enum_v<T>);

            if constexpr (std::is_same_v<T, bool>)
            {
                const uint8 encoded = value ? 1 : 0;
                HashBytes(hash, &encoded, sizeof(encoded));
            }
            else if constexpr (std::is_enum_v<T>)
            {
                using Underlying = std::underlying_type_t<T>;
                using Unsigned = std::make_unsigned_t<Underlying>;
                const Unsigned encoded =
                    static_cast<Unsigned>(
                        static_cast<Underlying>(value));
                for (size_t byteIndex = 0;
                     byteIndex < sizeof(encoded);
                     ++byteIndex)
                {
                    const uint8 byte = static_cast<uint8>(
                        encoded >> (byteIndex * 8));
                    HashBytes(hash, &byte, sizeof(byte));
                }
            }
            else
            {
                using Unsigned = std::make_unsigned_t<T>;
                const Unsigned encoded =
                    static_cast<Unsigned>(value);
                for (size_t byteIndex = 0;
                     byteIndex < sizeof(encoded);
                     ++byteIndex)
                {
                    const uint8 byte = static_cast<uint8>(
                        encoded >> (byteIndex * 8));
                    HashBytes(hash, &byte, sizeof(byte));
                }
            }
        }

        void HashString(uint64& hash, const std::string& value)
        {
            const uint32 size = static_cast<uint32>(value.size());
            HashValue(hash, size);
            HashBytes(hash, value.data(), value.size());
        }

        bool VariableLess(const RHIShaderInterfaceVariable& left,
                          const RHIShaderInterfaceVariable& right)
        {
            return std::tie(left.systemValue,
                            left.location,
                            left.semanticName,
                            left.semanticIndex,
                            left.format) <
                   std::tie(right.systemValue,
                            right.location,
                            right.semanticName,
                            right.semanticIndex,
                            right.format);
        }

        void HashVariables(
            uint64& hash,
            const std::vector<RHIShaderInterfaceVariable>& variables)
        {
            HashValue(hash, static_cast<uint32>(variables.size()));
            for (const RHIShaderInterfaceVariable& variable : variables)
            {
                HashValue(hash, variable.location);
                HashValue(hash, variable.format);
                HashValue(hash, variable.systemValue);
                HashString(hash, variable.semanticName);
                HashValue(hash, variable.semanticIndex);
            }
        }
    } // namespace

    RHIShaderInterface FinalizeRHIShaderInterface(
        RHIShaderInterface shaderInterface)
    {
        std::sort(shaderInterface.inputs.begin(),
                  shaderInterface.inputs.end(),
                  VariableLess);
        std::sort(shaderInterface.outputs.begin(),
                  shaderInterface.outputs.end(),
                  VariableLess);
        std::sort(
            shaderInterface.bindings.begin(),
            shaderInterface.bindings.end(),
            [](const RHIShaderInterfaceBinding& left,
               const RHIShaderInterfaceBinding& right)
            {
                return std::tie(left.set,
                                left.binding,
                                left.type,
                                left.count) <
                       std::tie(right.set,
                                right.binding,
                                right.type,
                                right.count);
            });
        std::sort(
            shaderInterface.pushConstants.begin(),
            shaderInterface.pushConstants.end(),
            [](const RHIShaderInterfacePushConstantRange& left,
               const RHIShaderInterfacePushConstantRange& right)
            {
                return std::tie(left.offset, left.size) <
                       std::tie(right.offset, right.size);
            });

        uint64 hash = kFnvOffsetBasis;
        HashValue(hash, shaderInterface.schemaVersion);
        HashValue(hash, shaderInterface.available);
        HashValue(hash, shaderInterface.stage);
        HashVariables(hash, shaderInterface.inputs);
        HashVariables(hash, shaderInterface.outputs);
        HashValue(hash,
                  static_cast<uint32>(shaderInterface.bindings.size()));
        for (const RHIShaderInterfaceBinding& binding :
             shaderInterface.bindings)
        {
            HashValue(hash, binding.set);
            HashValue(hash, binding.binding);
            HashValue(hash, binding.type);
            HashValue(hash, binding.count);
        }
        HashValue(
            hash,
            static_cast<uint32>(shaderInterface.pushConstants.size()));
        for (const RHIShaderInterfacePushConstantRange& range :
             shaderInterface.pushConstants)
        {
            HashValue(hash, range.offset);
            HashValue(hash, range.size);
        }
        shaderInterface.hash = hash;
        return shaderInterface;
    }

    RHIShader::RHIShader(const RHIShaderDesc& desc)
        : m_entryPoint(desc.entryPoint ? desc.entryPoint : "main")
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
        if (desc.shaderInterface)
        {
            m_interface =
                FinalizeRHIShaderInterface(*desc.shaderInterface);
        }
    }
} // namespace RVX
