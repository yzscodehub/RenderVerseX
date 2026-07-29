#include "RHI_BackendFactory/RHIBackendFactory.h"

namespace
{
    using FactoryFunction = std::unique_ptr<RVX::IRHIDevice> (*)(
        RVX::RHIBackendType,
        const RVX::RHIDeviceDesc&);

    FactoryFunction volatile g_factory = &RVX::CreateRHIDevice;
} // namespace

int main()
{
    return g_factory == nullptr ? 1 : 0;
}
