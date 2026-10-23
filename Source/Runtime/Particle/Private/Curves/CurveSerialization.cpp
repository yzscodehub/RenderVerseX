#include "Particle/Curves/CurveSerialization.h"
#include <cstring>
#include <sstream>
#include <iomanip>

namespace RVX::Particle
{

// ============================================================================
// Binary Serialization
// ============================================================================

// File format version
constexpr uint32 CURVE_FORMAT_VERSION = 1;
constexpr uint32 GRADIENT_FORMAT_VERSION = 1;

// Magic numbers
constexpr uint32 CURVE_MAGIC = 0x43555256; // "CURV"
constexpr uint32 GRADIENT_MAGIC = 0x47524144; // "GRAD"

std::vector<uint8> SerializeCurve(const AnimationCurve& curve)
{
    const auto& keys = curve.GetKeys();
    
    // Calculate size
    size_t headerSize = sizeof(uint32) * 2 + sizeof(uint32); // magic + version + keyCount
    size_t keySize = sizeof(float) * 4; // time, value, inTangent, outTangent
    size_t totalSize = headerSize + keys.size() * keySize;
    
    std::vector<uint8> data(totalSize);
    uint8* ptr = data.data();
    
    // Write header
    std::memcpy(ptr, &CURVE_MAGIC, sizeof(uint32));
    ptr += sizeof(uint32);
    
    std::memcpy(ptr, &CURVE_FORMAT_VERSION, sizeof(uint32));
    ptr += sizeof(uint32);
    
    uint32 keyCount = static_cast<uint32>(keys.size());
    std::memcpy(ptr, &keyCount, sizeof(uint32));
    ptr += sizeof(uint32);
    
    // Write keyframes
    for (const auto& key : keys)
    {
        std::memcpy(ptr, &key.time, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(ptr, &key.value, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(ptr, &key.inTangent, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(ptr, &key.outTangent, sizeof(float));
        ptr += sizeof(float);
    }
    
    return data;
}

AnimationCurve DeserializeCurve(const uint8* data, size_t size)
{
    AnimationCurve curve;
    
    if (size < sizeof(uint32) * 3)
        return curve;
    
    const uint8* ptr = data;
    
    // Read header
    uint32 magic;
    std::memcpy(&magic, ptr, sizeof(uint32));
    ptr += sizeof(uint32);
    
    if (magic != CURVE_MAGIC)
        return curve;
    
    uint32 version;
    std::memcpy(&version, ptr, sizeof(uint32));
    ptr += sizeof(uint32);
    
    if (version > CURVE_FORMAT_VERSION)
        return curve;
    
    uint32 keyCount;
    std::memcpy(&keyCount, ptr, sizeof(uint32));
    ptr += sizeof(uint32);
    
    // Read keyframes
    for (uint32 i = 0; i < keyCount; ++i)
    {
        CurveKeyframe key;
        std::memcpy(&key.time, ptr, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(&key.value, ptr, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(&key.inTangent, ptr, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(&key.outTangent, ptr, sizeof(float));
        ptr += sizeof(float);
        
        curve.AddKey(key);
    }
    
    return curve;
}

std::vector<uint8> SerializeGradient(const GradientCurve& gradient)
{
    const auto& keys = gradient.GetKeys();
    
    // Calculate size
    size_t headerSize = sizeof(uint32) * 2 + sizeof(uint32); // magic + version + keyCount
    size_t keySize = sizeof(float) * 5; // time, r, g, b, a
    size_t totalSize = headerSize + keys.size() * keySize;
    
    std::vector<uint8> data(totalSize);
    uint8* ptr = data.data();
    
    // Write header
    std::memcpy(ptr, &GRADIENT_MAGIC, sizeof(uint32));
    ptr += sizeof(uint32);
    
    std::memcpy(ptr, &GRADIENT_FORMAT_VERSION, sizeof(uint32));
    ptr += sizeof(uint32);
    
    uint32 keyCount = static_cast<uint32>(keys.size());
    std::memcpy(ptr, &keyCount, sizeof(uint32));
    ptr += sizeof(uint32);
    
    // Write keys
    for (const auto& key : keys)
    {
        std::memcpy(ptr, &key.time, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(ptr, &key.color.x, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(ptr, &key.color.y, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(ptr, &key.color.z, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(ptr, &key.color.w, sizeof(float));
        ptr += sizeof(float);
    }
    
    return data;
}

GradientCurve DeserializeGradient(const uint8* data, size_t size)
{
    GradientCurve gradient;
    
    if (size < sizeof(uint32) * 3)
        return gradient;
    
    const uint8* ptr = data;
    
    // Read header
    uint32 magic;
    std::memcpy(&magic, ptr, sizeof(uint32));
    ptr += sizeof(uint32);
    
    if (magic != GRADIENT_MAGIC)
        return gradient;
    
    uint32 version;
    std::memcpy(&version, ptr, sizeof(uint32));
    ptr += sizeof(uint32);
    
    if (version > GRADIENT_FORMAT_VERSION)
        return gradient;
    
    uint32 keyCount;
    std::memcpy(&keyCount, ptr, sizeof(uint32));
    ptr += sizeof(uint32);
    
    // Read keys
    for (uint32 i = 0; i < keyCount; ++i)
    {
        GradientKey key;
        std::memcpy(&key.time, ptr, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(&key.color.x, ptr, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(&key.color.y, ptr, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(&key.color.z, ptr, sizeof(float));
        ptr += sizeof(float);
        std::memcpy(&key.color.w, ptr, sizeof(float));
        ptr += sizeof(float);
        
        gradient.AddKey(key);
    }
    
    return gradient;
}

// ============================================================================
// JSON Serialization
// ============================================================================

std::string CurveToJson(const AnimationCurve& curve)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);
    
    oss << "{\n";
    oss << "  \"type\": \"AnimationCurve\",\n";
    oss << "  \"keys\": [\n";
    
    const auto& keys = curve.GetKeys();
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const auto& key = keys[i];
        oss << "    {\n";
        oss << "      \"time\": " << key.time << ",\n";
        oss << "      \"value\": " << key.value << ",\n";
        oss << "      \"inTangent\": " << key.inTangent << ",\n";
        oss << "      \"outTangent\": " << key.outTangent << "\n";
        oss << "    }";
        if (i < keys.size() - 1) oss << ",";
        oss << "\n";
    }
    
    oss << "  ]\n";
    oss << "}";
    
    return oss.str();
}

AnimationCurve CurveFromJson(const std::string& json)
{
    AnimationCurve curve;
    
    // Simple parser - find keys array
    size_t keysStart = json.find("\"keys\"");
    if (keysStart == std::string::npos)
        return curve;
    
    // Find array start
    size_t arrayStart = json.find('[', keysStart);
    if (arrayStart == std::string::npos)
        return curve;
    
    // Parse each key object
    size_t pos = arrayStart + 1;
    while (pos < json.size())
    {
        // Find next object
        size_t objStart = json.find('{', pos);
        if (objStart == std::string::npos)
            break;
        
        size_t objEnd = json.find('}', objStart);
        if (objEnd == std::string::npos)
            break;
        
        std::string obj = json.substr(objStart, objEnd - objStart + 1);
        
        CurveKeyframe key;
        
        // Parse time
        size_t timePos = obj.find("\"time\"");
        if (timePos != std::string::npos)
        {
            size_t colonPos = obj.find(':', timePos);
            size_t commaPos = obj.find(',', colonPos);
            std::string val = obj.substr(colonPos + 1, commaPos - colonPos - 1);
            key.time = std::stof(val);
        }
        
        // Parse value
        size_t valuePos = obj.find("\"value\"");
        if (valuePos != std::string::npos)
        {
            size_t colonPos = obj.find(':', valuePos);
            size_t commaPos = obj.find(',', colonPos);
            std::string val = obj.substr(colonPos + 1, commaPos - colonPos - 1);
            key.value = std::stof(val);
        }
        
        // Parse inTangent
        size_t inTanPos = obj.find("\"inTangent\"");
        if (inTanPos != std::string::npos)
        {
            size_t colonPos = obj.find(':', inTanPos);
            size_t commaPos = obj.find(',', colonPos);
            if (commaPos == std::string::npos) commaPos = obj.find('\n', colonPos);
            std::string val = obj.substr(colonPos + 1, commaPos - colonPos - 1);
            key.inTangent = std::stof(val);
        }
        
        // Parse outTangent
        size_t outTanPos = obj.find("\"outTangent\"");
        if (outTanPos != std::string::npos)
        {
            size_t colonPos = obj.find(':', outTanPos);
            size_t endPos = obj.find_first_of(",}\n", colonPos);
            std::string val = obj.substr(colonPos + 1, endPos - colonPos - 1);
            key.outTangent = std::stof(val);
        }
        
        curve.AddKey(key);
        pos = objEnd + 1;
    }
    
    return curve;
}

std::string GradientToJson(const GradientCurve& gradient)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);
    
    oss << "{\n";
    oss << "  \"type\": \"GradientCurve\",\n";
    oss << "  \"keys\": [\n";
    
    const auto& keys = gradient.GetKeys();
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const auto& key = keys[i];
        oss << "    {\n";
        oss << "      \"time\": " << key.time << ",\n";
        oss << "      \"color\": [" 
            << key.color.x << ", " 
            << key.color.y << ", " 
            << key.color.z << ", " 
            << key.color.w << "]\n";
        oss << "    }";
        if (i < keys.size() - 1) oss << ",";
        oss << "\n";
    }
    
    oss << "  ]\n";
    oss << "}";
    
    return oss.str();
}

GradientCurve GradientFromJson(const std::string& json)
{
    GradientCurve gradient;
    
    // Simple parser - find keys array
    size_t keysStart = json.find("\"keys\"");
    if (keysStart == std::string::npos)
        return gradient;
    
    // Find array start
    size_t arrayStart = json.find('[', keysStart);
    if (arrayStart == std::string::npos)
        return gradient;
    
    // Parse each key object
    size_t pos = arrayStart + 1;
    while (pos < json.size())
    {
        // Find next object
        size_t objStart = json.find('{', pos);
        if (objStart == std::string::npos)
            break;
        
        size_t objEnd = json.find('}', objStart);
        if (objEnd == std::string::npos)
            break;
        
        std::string obj = json.substr(objStart, objEnd - objStart + 1);
        
        GradientKey key;
        
        // Parse time
        size_t timePos = obj.find("\"time\"");
        if (timePos != std::string::npos)
        {
            size_t colonPos = obj.find(':', timePos);
            size_t commaPos = obj.find(',', colonPos);
            std::string val = obj.substr(colonPos + 1, commaPos - colonPos - 1);
            key.time = std::stof(val);
        }
        
        // Parse color array
        size_t colorPos = obj.find("\"color\"");
        if (colorPos != std::string::npos)
        {
            size_t arrStart = obj.find('[', colorPos);
            size_t arrEnd = obj.find(']', arrStart);
            std::string arr = obj.substr(arrStart + 1, arrEnd - arrStart - 1);
            
            // Parse 4 float values
            std::istringstream iss(arr);
            char comma;
            iss >> key.color.x >> comma >> key.color.y >> comma 
                >> key.color.z >> comma >> key.color.w;
        }
        
        gradient.AddKey(key);
        pos = objEnd + 1;
    }
    
    return gradient;
}

} // namespace RVX::Particle
