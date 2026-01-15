#include "Platform/Window.h"

namespace RVX
{
    class NullWindow final : public Window
    {
    public:
        explicit NullWindow(const WindowDesc& desc) : m_desc(desc) {}

        void PollEvents() override {}
        bool ShouldClose() const override { return false; }
        void GetFramebufferSize(uint32& width, uint32& height) const override
        {
            width = m_desc.width;
            height = m_desc.height;
        }
        float GetDpiScale() const override { return 1.0f; }
        void* GetNativeHandle() const override { return nullptr; }

    private:
        WindowDesc m_desc;
    };

    std::unique_ptr<Window> CreateWindow(const WindowDesc& desc)
    {
        return std::make_unique<NullWindow>(desc);
    }
} // namespace RVX
