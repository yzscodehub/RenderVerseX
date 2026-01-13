#pragma once

#include "Core/Types.h"
#include <atomic>
#include <type_traits>

namespace RVX
{
    // =============================================================================
    // Forward Declarations
    // =============================================================================
    class RefCounted;

    // =============================================================================
    // Deferred Deleter Interface
    // Used to decouple Core layer from RenderLayer (avoids circular dependency)
    // =============================================================================
    class IDeferredDeleter
    {
    public:
        virtual ~IDeferredDeleter() = default;
        virtual void DeferredDelete(const RefCounted* object) = 0;
    };

    // =============================================================================
    // Deferred Deleter Registry
    // Global registration point for the deferred deletion system
    // =============================================================================
    class DeferredDeleterRegistry
    {
    public:
        static void Register(IDeferredDeleter* deleter) { s_deleter = deleter; }
        static void Unregister() { s_deleter = nullptr; }
        static IDeferredDeleter* Get() { return s_deleter; }

    private:
        static inline IDeferredDeleter* s_deleter = nullptr;
    };

    // =============================================================================
    // Intrusive Reference Counted Base Class
    // =============================================================================
    class RefCounted
    {
    public:
        RefCounted() = default;
        virtual ~RefCounted() = default;

        // Non-copyable
        RefCounted(const RefCounted&) = delete;
        RefCounted& operator=(const RefCounted&) = delete;

        // Reference counting
        void AddRef() const
        {
            m_refCount.fetch_add(1, std::memory_order_relaxed);
        }

        // Returns true if reference count reached zero (object should be deleted)
        bool Release() const
        {
            return m_refCount.fetch_sub(1, std::memory_order_acq_rel) == 1;
        }

        uint32 GetRefCount() const
        {
            return m_refCount.load(std::memory_order_relaxed);
        }

    private:
        mutable std::atomic<uint32> m_refCount{0};
    };

    // =============================================================================
    // Smart Pointer for RefCounted Objects
    // =============================================================================
    template<typename T>
    class Ref
    {
        static_assert(std::is_base_of_v<RefCounted, T>,
                      "T must inherit from RefCounted");

    public:
        // Constructors
        Ref() = default;
        Ref(std::nullptr_t) : m_ptr(nullptr) {}

        explicit Ref(T* ptr) : m_ptr(ptr)
        {
            if (m_ptr)
                m_ptr->AddRef();
        }

        Ref(const Ref& other) : m_ptr(other.m_ptr)
        {
            if (m_ptr)
                m_ptr->AddRef();
        }

        template<typename U>
        Ref(const Ref<U>& other) : m_ptr(other.Get())
        {
            static_assert(std::is_base_of_v<T, U> || std::is_base_of_v<U, T>,
                          "Incompatible types");
            if (m_ptr)
                m_ptr->AddRef();
        }

        Ref(Ref&& other) noexcept : m_ptr(other.m_ptr)
        {
            other.m_ptr = nullptr;
        }

        template<typename U>
        Ref(Ref<U>&& other) noexcept : m_ptr(other.Get())
        {
            static_assert(std::is_base_of_v<T, U> || std::is_base_of_v<U, T>,
                          "Incompatible types");
            other.Detach();
        }

        ~Ref()
        {
            TryDelete();
        }

        // Assignment operators
        Ref& operator=(const Ref& other)
        {
            if (this != &other)
            {
                TryDelete();
                m_ptr = other.m_ptr;
                if (m_ptr)
                    m_ptr->AddRef();
            }
            return *this;
        }

        Ref& operator=(Ref&& other) noexcept
        {
            if (this != &other)
            {
                TryDelete();
                m_ptr = other.m_ptr;
                other.m_ptr = nullptr;
            }
            return *this;
        }

        Ref& operator=(std::nullptr_t)
        {
            Reset();
            return *this;
        }

        // Accessors
        T* Get() const { return m_ptr; }
        T* operator->() const { return m_ptr; }
        T& operator*() const { return *m_ptr; }

        explicit operator bool() const { return m_ptr != nullptr; }

        bool operator==(const Ref& other) const { return m_ptr == other.m_ptr; }
        bool operator!=(const Ref& other) const { return m_ptr != other.m_ptr; }
        bool operator==(std::nullptr_t) const { return m_ptr == nullptr; }
        bool operator!=(std::nullptr_t) const { return m_ptr != nullptr; }

        // Reset the pointer
        void Reset(T* ptr = nullptr)
        {
            TryDelete();
            m_ptr = ptr;
            if (m_ptr)
                m_ptr->AddRef();
        }

        // Detach without releasing (transfers ownership)
        T* Detach()
        {
            T* ptr = m_ptr;
            m_ptr = nullptr;
            return ptr;
        }

    private:
        void TryDelete()
        {
            if (m_ptr && m_ptr->Release())
            {
                // Reference count reached zero, defer deletion
                if (auto* deleter = DeferredDeleterRegistry::Get())
                {
                    deleter->DeferredDelete(m_ptr);
                }
                else
                {
                    // No deferred deleter registered, delete immediately
                    // (useful for testing or tools)
                    delete m_ptr;
                }
                m_ptr = nullptr;
            }
        }

        T* m_ptr = nullptr;

        template<typename U>
        friend class Ref;
    };

    // =============================================================================
    // Helper function to create Ref
    // =============================================================================
    template<typename T, typename... Args>
    Ref<T> MakeRef(Args&&... args)
    {
        return Ref<T>(new T(std::forward<Args>(args)...));
    }

} // namespace RVX
