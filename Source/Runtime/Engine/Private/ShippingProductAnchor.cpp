/**
 * @file ShippingProductAnchor.cpp
 * @brief Buildable Shipping product anchor for the runtime-only composition.
 *
 * The product target intentionally contains no separate composition logic.
 * Its public dependency is the one `RVX::EngineRuntimeOnlyComposition`
 * implementation, while this translation unit preserves a concrete target
 * for Shipping target-closure and consumer validation.
 */

namespace RVX
{
    namespace
    {
        [[maybe_unused]] constexpr int ShippingProductAnchor = 0;
    }
} // namespace RVX
