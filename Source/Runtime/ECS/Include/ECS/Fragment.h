#pragma once

#include <concepts>
#include <type_traits>

namespace RVX::ECS
{
    /**
     * @brief Constraint for data-only fragments stored by the P1 sparse-set ECS.
     *
     * Fragment values are copied into transaction staging state, so polymorphic
     * and ownership-bearing component types deliberately do not fit this layer.
     */
    template<typename T>
    concept Fragment = std::is_standard_layout_v<T> &&
                       std::is_trivially_copyable_v<T> &&
                       !std::is_polymorphic_v<T>;

    /** @brief Empty fragment type used by AddTag/HasTag for marker membership. */
    template<typename T>
    struct Tag
    {
    };

    /** @brief Declares read-only fragment access for a Query. */
    template<Fragment T>
    struct Read
    {
        using FragmentType = T;
        static constexpr bool IsWritable = false;
    };

    /** @brief Declares mutable fragment access and advances its dirty version. */
    template<Fragment T>
    struct Write
    {
        using FragmentType = T;
        static constexpr bool IsWritable = true;
    };

    /** @brief Requires fragment membership without passing it to the callback. */
    template<Fragment T>
    struct With
    {
        using FragmentType = T;
    };

    /** @brief Excludes enabled fragment membership from a query. */
    template<Fragment T>
    struct Without
    {
        using FragmentType = T;
    };

    /** @brief Query access traits. Bare fragment types remain read-only. */
    template<typename T>
    struct QueryAccess;

    template<Fragment T>
    struct QueryAccess<T>
    {
        using FragmentType = T;
        static constexpr bool IsWritable = false;
        static constexpr bool IsAccess = true;
        static constexpr bool RequiresPresent = true;
        static constexpr bool Excludes = false;
    };

    template<Fragment T>
    struct QueryAccess<Read<T>>
    {
        using FragmentType = T;
        static constexpr bool IsWritable = false;
        static constexpr bool IsAccess = true;
        static constexpr bool RequiresPresent = true;
        static constexpr bool Excludes = false;
    };

    template<Fragment T>
    struct QueryAccess<Write<T>>
    {
        using FragmentType = T;
        static constexpr bool IsWritable = true;
        static constexpr bool IsAccess = true;
        static constexpr bool RequiresPresent = true;
        static constexpr bool Excludes = false;
    };

    template<Fragment T>
    struct QueryAccess<With<T>>
    {
        using FragmentType = T;
        static constexpr bool IsWritable = false;
        static constexpr bool IsAccess = false;
        static constexpr bool RequiresPresent = true;
        static constexpr bool Excludes = false;
    };

    template<Fragment T>
    struct QueryAccess<Without<T>>
    {
        using FragmentType = T;
        static constexpr bool IsWritable = false;
        static constexpr bool IsAccess = false;
        static constexpr bool RequiresPresent = false;
        static constexpr bool Excludes = true;
    };

    template<typename T>
    concept QueryTerm = requires
    {
        typename QueryAccess<T>::FragmentType;
        { QueryAccess<T>::IsWritable } -> std::convertible_to<bool>;
        { QueryAccess<T>::IsAccess } -> std::convertible_to<bool>;
        { QueryAccess<T>::RequiresPresent } -> std::convertible_to<bool>;
        { QueryAccess<T>::Excludes } -> std::convertible_to<bool>;
    };
} // namespace RVX::ECS
