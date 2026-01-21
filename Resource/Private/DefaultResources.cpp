#include "Resource/DefaultResources.h"
#include "Scene/Material.h"
#include "Core/Log.h"
#include <functional>

namespace RVX::Resource
{
    // Static member initialization
    bool DefaultResources::s_initialized = false;
    std::unique_ptr<TextureResource> DefaultResources::s_whiteTexture;
    std::unique_ptr<TextureResource> DefaultResources::s_normalTexture;
    std::unique_ptr<TextureResource> DefaultResources::s_blackTexture;
    std::unique_ptr<TextureResource> DefaultResources::s_errorTexture;
    std::unique_ptr<MaterialResource> DefaultResources::s_defaultMaterial;
    std::unique_ptr<MaterialResource> DefaultResources::s_errorMaterial;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void DefaultResources::Initialize()
    {
        if (s_initialized)
            return;

        // Create default textures
        s_whiteTexture.reset(CreateSolidTexture("__default_white__", 255, 255, 255, 255, true));
        s_normalTexture.reset(CreateSolidTexture("__default_normal__", 128, 128, 255, 255, false));
        s_blackTexture.reset(CreateSolidTexture("__default_black__", 0, 0, 0, 255, true));
        s_errorTexture.reset(CreateCheckerTexture("__default_error__", 255, 0, 255, 0, 0, 0));

        // Create default material
        s_defaultMaterial = std::make_unique<MaterialResource>();
        s_defaultMaterial->SetId(std::hash<std::string>{}("__default_material__"));
        s_defaultMaterial->SetPath("__default_material__");
        s_defaultMaterial->SetName("DefaultMaterial");
        
        auto defaultMat = std::make_shared<Material>("DefaultMaterial");
        defaultMat->SetBaseColor(Vec4(0.8f, 0.8f, 0.8f, 1.0f));
        defaultMat->SetMetallicFactor(0.0f);
        defaultMat->SetRoughnessFactor(0.5f);
        s_defaultMaterial->SetMaterialData(defaultMat);
        s_defaultMaterial->NotifyLoaded();

        // Create error material
        s_errorMaterial = std::make_unique<MaterialResource>();
        s_errorMaterial->SetId(std::hash<std::string>{}("__error_material__"));
        s_errorMaterial->SetPath("__error_material__");
        s_errorMaterial->SetName("ErrorMaterial");
        
        auto errorMat = std::make_shared<Material>("ErrorMaterial");
        errorMat->SetBaseColor(Vec4(1.0f, 0.0f, 1.0f, 1.0f));  // Magenta
        errorMat->SetMetallicFactor(0.0f);
        errorMat->SetRoughnessFactor(1.0f);
        errorMat->SetEmissiveColor(Vec3(0.5f, 0.0f, 0.5f));
        s_errorMaterial->SetMaterialData(errorMat);
        s_errorMaterial->NotifyLoaded();

        s_initialized = true;
        RVX_CORE_INFO("DefaultResources initialized");
    }

    void DefaultResources::Shutdown()
    {
        if (!s_initialized)
            return;

        s_whiteTexture.reset();
        s_normalTexture.reset();
        s_blackTexture.reset();
        s_errorTexture.reset();
        s_defaultMaterial.reset();
        s_errorMaterial.reset();

        s_initialized = false;
        RVX_CORE_INFO("DefaultResources shutdown");
    }

    bool DefaultResources::IsInitialized()
    {
        return s_initialized;
    }

    // =========================================================================
    // Default Textures
    // =========================================================================

    TextureResource* DefaultResources::WhiteTexture()
    {
        if (!s_initialized)
            Initialize();
        return s_whiteTexture.get();
    }

    TextureResource* DefaultResources::NormalTexture()
    {
        if (!s_initialized)
            Initialize();
        return s_normalTexture.get();
    }

    TextureResource* DefaultResources::BlackTexture()
    {
        if (!s_initialized)
            Initialize();
        return s_blackTexture.get();
    }

    TextureResource* DefaultResources::ErrorTexture()
    {
        if (!s_initialized)
            Initialize();
        return s_errorTexture.get();
    }

    TextureResource* DefaultResources::GetDefaultTexture(TextureUsage usage)
    {
        switch (usage)
        {
            case TextureUsage::Normal:
                return NormalTexture();
            case TextureUsage::Color:
                return WhiteTexture();
            case TextureUsage::Data:
                return WhiteTexture();
            default:
                return WhiteTexture();
        }
    }

    // =========================================================================
    // Default Materials
    // =========================================================================

    MaterialResource* DefaultResources::DefaultMaterial()
    {
        if (!s_initialized)
            Initialize();
        return s_defaultMaterial.get();
    }

    MaterialResource* DefaultResources::ErrorMaterial()
    {
        if (!s_initialized)
            Initialize();
        return s_errorMaterial.get();
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    TextureResource* DefaultResources::CreateSolidTexture(const std::string& name,
                                                            uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                                            bool isSRGB)
    {
        auto* texture = new TextureResource();
        
        texture->SetId(std::hash<std::string>{}(name));
        texture->SetPath(name);
        texture->SetName(name);

        std::vector<uint8_t> pixels = { r, g, b, a };

        TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.format = TextureFormat::RGBA8;
        metadata.mipLevels = 1;
        metadata.isSRGB = isSRGB;
        metadata.usage = isSRGB ? TextureUsage::Color : TextureUsage::Normal;

        texture->SetData(std::move(pixels), metadata);
        texture->NotifyLoaded();

        return texture;
    }

    TextureResource* DefaultResources::CreateCheckerTexture(const std::string& name,
                                                              uint8_t r1, uint8_t g1, uint8_t b1,
                                                              uint8_t r2, uint8_t g2, uint8_t b2)
    {
        auto* texture = new TextureResource();
        
        texture->SetId(std::hash<std::string>{}(name));
        texture->SetPath(name);
        texture->SetName(name);

        // 2x2 checkerboard pattern
        std::vector<uint8_t> pixels = {
            r1, g1, b1, 255,    r2, g2, b2, 255,
            r2, g2, b2, 255,    r1, g1, b1, 255
        };

        TextureMetadata metadata;
        metadata.width = 2;
        metadata.height = 2;
        metadata.format = TextureFormat::RGBA8;
        metadata.mipLevels = 1;
        metadata.isSRGB = true;
        metadata.usage = TextureUsage::Color;

        texture->SetData(std::move(pixels), metadata);
        texture->NotifyLoaded();

        return texture;
    }

} // namespace RVX::Resource
