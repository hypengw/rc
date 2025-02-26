module;
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <limits>
#include <new>

export module rstd.rc:rc;

namespace rstd::rc
{

namespace detail
{
template<typename Allocator, typename T>
using rebind_alloc = typename std::allocator_traits<Allocator>::template rebind_alloc<T>;

void increase_count(std::size_t& count) {
    if (count == std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("reference count overflow");
    }
    ++count;
}

template<typename T>
constexpr auto compute_alignment() -> std::size_t {
    return std::max(alignof(std::remove_extent_t<T>), alignof(std::size_t));
}

template<typename T>
[[nodiscard]] constexpr auto layout_for_value(std::size_t align) -> std::size_t {
    const auto size = sizeof(T);
    return (size + align - 1) & ~(align - 1);
}

template<typename T>
[[nodiscard]] constexpr auto layout_for_value() -> std::size_t {
    return layout_for_value<T>(alignof(T));
}

enum class DeleteType
{
    Value = 0,
    Self,
};

} // namespace detail

enum class StoragePolicy : std::uint32_t
{
    Embed = 0,
    Separate,
    SeparateWithDeleter
};

template<typename T>
struct alignas(std::size_t) RcInner {
    using value_t = std::remove_extent_t<T>;

    std::size_t strong { 1 };
    std::size_t weak { 1 };
    value_t*    value { nullptr };

    RcInner() noexcept {}
    // not need virtual here
    ~RcInner() = default;

    virtual void do_delete(detail::DeleteType t) {
        auto self = this;
        if (t == detail::DeleteType::Value) {
            delete self->value;
        } else {
            delete self;
        }
    }

    void inc_strong() { detail::increase_count(strong); }
    void dec_strong() { --strong; }
    void inc_weak() { detail::increase_count(weak); }
    void dec_weak() { --weak; }
};
namespace detail
{

template<typename T, StoragePolicy P, typename ValueDeleter = void>
struct RcInnerImpl {
    static_assert(false);
};

template<typename T>
struct RcInnerImpl<T, StoragePolicy::Embed> : RcInner<T> {
    static_assert(! std::is_array_v<T>);

    alignas(T) std::byte storage[sizeof(T)];

    RcInnerImpl() noexcept: RcInner<T>() {}

    void do_delete(detail::DeleteType t) override {
        auto self = this;
        if (t == detail::DeleteType::Value) {
            self->value->~T();
            self->value = nullptr;
        } else {
            delete self;
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto ptr    = new (storage) T(std::forward<Args>(args)...);
        this->value = ptr;
    }
};

template<typename T>
struct RcInnerImpl<T, StoragePolicy::Separate> : RcInner<T> {
    RcInnerImpl() noexcept: RcInner<T>() {}

    void do_delete(detail::DeleteType t) override {
        auto self = this;
        if (t == detail::DeleteType::Value) {
            delete self->value;
            self->value = nullptr;
        } else {
            delete self;
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto ptr    = new T(std::forward<Args>(args)...);
        this->value = ptr;
    }
};

template<typename T>
struct RcInnerArrayImpl : RcInner<T> {
    const std::size_t size;

    RcInnerArrayImpl(std::size_t n) noexcept: RcInner<T>(), size(n) {}
};

template<typename T>
struct RcInnerImpl<T[], StoragePolicy::Separate> : RcInnerArrayImpl<T[]> {
    using value_t = std::remove_extent_t<T>;

    RcInnerImpl(std::size_t n) noexcept: RcInnerArrayImpl<T[]>(n) {}

    void do_delete(detail::DeleteType t) override {
        auto self = this;
        if (t == detail::DeleteType::Value) {
            auto ptr = const_cast<std::remove_const_t<value_t>*>(self->value);
            for (std::size_t i = 0; i < this->size; i++) {
                (ptr + i)->~value_t();
            }
            ::operator delete(ptr,
                              sizeof(value_t) * this->size,
                              std::align_val_t { std::alignment_of_v<value_t> });
            self->value = nullptr;
        } else {
            delete self;
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto* ptr   = static_cast<value_t*>(::operator new[](
            sizeof(value_t) * this->size, std::align_val_t { std::alignment_of_v<value_t> }));
        this->value = ptr;
        for (std::size_t i = 0; i < this->size; i++) {
            new (ptr + i) value_t(std::forward<Args>(args)...);
        }
    }
};

template<typename T, typename ValueDeleter>
struct RcInnerImpl<T, StoragePolicy::SeparateWithDeleter, ValueDeleter> : RcInner<T> {
    ValueDeleter value_deletor;

    RcInnerImpl(T* p, ValueDeleter d): RcInner<T>(), value_deletor(std::move(d)) {
        this->value = p;
    }

    void do_delete(detail::DeleteType t) override {
        auto self = this;

        if (t == detail::DeleteType::Value) {
            self->value_deletor(self->value);
            self->value = nullptr;
        } else {
            delete self;
        }
    }
};

template<typename T, typename Allocator, StoragePolicy P, typename ValueDeleter = void>
struct RcInnerAllocImpl {
    static_assert(false);
};

template<typename T, typename Allocator>
struct RcInnerAllocImpl<T, Allocator, StoragePolicy::Embed> : RcInnerImpl<T, StoragePolicy::Embed> {
    using base_t = RcInnerImpl<T, StoragePolicy::Embed>;
    Allocator allocator;

    RcInnerAllocImpl(Allocator a): base_t(), allocator(a) {}
    void do_delete(detail::DeleteType t) override {
        auto self = this;
        if (t == detail::DeleteType::Value) {
            self->value->~T();
            self->value     = nullptr;
            self->has_value = false;
        } else {
            auto self_allocator =
                detail::rebind_alloc<Allocator, RcInnerAllocImpl>(self->allocator);
            self_allocator.deallocate(self, 1);
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        base_t::template allocate_value<Args...>(std::forward<Args>(args)...);
    }
};

template<typename T, typename Allocator>
struct RcInnerAllocImpl<T, Allocator, StoragePolicy::Separate>
    : RcInnerImpl<T, StoragePolicy::Separate> {
    static_assert(! std::is_array_v<T>);
    using base_t = RcInnerImpl<T, StoragePolicy::Separate>;
    Allocator allocator;

    RcInnerAllocImpl(Allocator a): base_t(), allocator(a) {}
    void do_delete(detail::DeleteType t) override {
        auto self = this;
        if (t == detail::DeleteType::Value) {
            self->value->~T();
            self->allocator.deallocate(self->value, 1);
            self->value = nullptr;
        } else {
            auto self_allocator =
                detail::rebind_alloc<Allocator, RcInnerAllocImpl>(self->allocator);
            self_allocator.deallocate(self, 1);
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto* ptr = allocator.allocate(1);
        new (ptr) T(std::forward<Args>(args)...);
        this->value = ptr;
    }
};

template<typename T, typename Allocator>
struct RcInnerAllocImpl<T[], Allocator, StoragePolicy::Separate>
    : RcInnerImpl<T[], StoragePolicy::Separate> {
    using base_t  = RcInnerImpl<T[], StoragePolicy::Separate>;
    using value_t = std::remove_extent_t<T>;
    Allocator allocator;

    RcInnerAllocImpl(Allocator a, std::size_t n): base_t(n), allocator(a) {}
    void do_delete(detail::DeleteType t) override {
        auto self = this;
        if (t == detail::DeleteType::Value) {
            auto n = base_t::size;
            for (std::size_t i = 0; i < n; i++) {
                (self->value + i)->~value_t();
            }
            auto p = const_cast<std::remove_const_t<value_t>*>(self->value);
            self->allocator.deallocate(p, n);
            self->value = nullptr;
        } else {
            auto self_allocator =
                detail::rebind_alloc<Allocator, RcInnerAllocImpl>(self->allocator);
            self_allocator.deallocate(self, 1);
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto  n     = this->size;
        auto* ptr   = allocator.allocate(n);
        this->value = ptr;
        for (std::size_t i = 0; i < n; i++) {
            new (ptr + i) value_t(std::forward<Args>(args)...);
        }
    }
};

template<typename T, typename Allocator, typename ValueDeleter>
struct RcInnerAllocImpl<T, Allocator, StoragePolicy::SeparateWithDeleter, ValueDeleter>
    : RcInnerImpl<T, StoragePolicy::SeparateWithDeleter, ValueDeleter> {
    using base_t = RcInnerImpl<T, StoragePolicy::Separate>;
    Allocator allocator;

    RcInnerAllocImpl(Allocator a, T* p, ValueDeleter d): base_t(p, std::move(d)), allocator(a) {}

    void do_delete(detail::DeleteType t) override {
        auto self = this;

        if (t == detail::DeleteType::Value) {
            base_t::value_deletor(self->value);
            self->value = nullptr;
        } else {
            auto self_allocator =
                detail::rebind_alloc<Allocator, RcInnerAllocImpl>(self->allocator);
            self_allocator.deallocate(self, 1);
        }
    }
};

} // namespace detail

export template<typename T>
class Rc;

export template<typename T>
class Weak {
    friend class Rc<T>;
    RcInner<T>* m_ptr;

    explicit Weak(RcInner<T>* p) noexcept: m_ptr(p) {}

public:
    Weak() noexcept: m_ptr(nullptr) {}

    Weak(const Weak& other) noexcept: Weak(other.clone()) {}

    Weak(Weak&& other) noexcept: m_ptr(other.m_ptr) { other.m_ptr = nullptr; }

    ~Weak() {
        if (m_ptr) {
            m_ptr->dec_weak();
            if (m_ptr->weak == 0) {
                m_ptr->do_delete(detail::DeleteType::Self);
            }
        }
    }

    auto clone() const noexcept -> Weak {
        if (m_ptr) m_ptr->inc_weak();
        return Weak(m_ptr);
    }

    auto upgrade() const -> std::optional<Rc<T>> {
        if (! m_ptr || m_ptr->strong == 0) return std::nullopt;
        m_ptr->inc_strong();
        return { Rc<T>(m_ptr) };
    }

    auto strong_count() const -> std::size_t { return m_ptr ? m_ptr->strong : 0; }

    auto weak_count() const -> std::size_t { return m_ptr ? m_ptr->weak - 1 : 0; }
};

template<typename T>
class Rc {
    using value_t = std::remove_extent_t<T>;

    friend class Weak<T>;
    RcInner<T>* m_ptr;

    explicit Rc(RcInner<T>* p) noexcept: m_ptr(p) {}

public:
    template<StoragePolicy Sp = StoragePolicy::Separate, typename U = T, typename... Args>
        requires(! std::is_array_v<U>)
    static auto make(Args&&... args) -> Rc {
        auto inner = new detail::RcInnerImpl<T, Sp>();
        inner->allocate_value(std::forward<Args>(args)...);
        return Rc(inner);
    }

    template<StoragePolicy Sp = StoragePolicy::Separate, typename U = T>
        requires std::is_array_v<U>
    static auto make(std::size_t n, const value_t& t) -> Rc<T> {
        auto inner = new detail::RcInnerImpl<T, Sp>(n);
        inner->allocate_value(t);
        return Rc(inner);
    }

    template<StoragePolicy Sp = StoragePolicy::Separate, typename U = T, typename Allocator,
             typename... Args>
        requires(! std::is_array_v<U>)
    static auto allocate_make(const Allocator& alloc, Args&&... args) -> Rc {
        using inner_t       = detail::RcInnerAllocImpl<T, Allocator, Sp>;
        auto self_allocator = detail::rebind_alloc<Allocator, inner_t>(alloc);

        auto mem   = (std::byte*)self_allocator.allocate(1);
        auto inner = new (mem) inner_t(alloc);
        inner->allocate_value(std::forward<Args>(args)...);
        return Rc(inner);
    }

    template<StoragePolicy Sp = StoragePolicy::Separate, typename U = T, typename Allocator>
        requires std::is_array_v<U>
    static auto allocate_make(const Allocator& alloc, std::size_t n, const value_t& t) -> Rc {
        using inner_t       = detail::RcInnerAllocImpl<T, Allocator, Sp>;
        auto self_allocator = detail::rebind_alloc<Allocator, inner_t>(alloc);

        auto mem   = (std::byte*)self_allocator.allocate(1);
        auto inner = new (mem) inner_t(alloc, n);
        inner->allocate_value(t);
        return Rc(inner);
    }

    Rc(): m_ptr(nullptr) {}

    Rc(const Rc& other) noexcept: Rc(other.clone()) {}

    Rc(Rc&& other) noexcept: m_ptr(other.m_ptr) { other.m_ptr = nullptr; }

    explicit Rc(T* p): Rc(p, std::default_delete<T>()) {}
    template<typename Deleter>
    Rc(T* p, Deleter d)
        : m_ptr(new detail::RcInnerImpl<T, StoragePolicy::SeparateWithDeleter, Deleter>(
              p, std::move(d))) {}

    template<typename Deleter, typename Allocator>
    Rc(T* p, Deleter d, Allocator alloc): m_ptr(nullptr) {
        using inner_t =
            detail::RcInnerAllocImpl<T, Allocator, StoragePolicy::SeparateWithDeleter, Deleter>;
        auto self_allocator = detail::rebind_alloc<Allocator, inner_t>(alloc);
        auto mem            = (std::byte*)self_allocator.allocate(1);
        m_ptr               = new (mem) inner_t(alloc, p, std::move(d));
    }

    ~Rc() {
        if (m_ptr) {
            m_ptr->dec_strong();
            if (m_ptr->strong == 0) {
                m_ptr->do_delete(detail::DeleteType::Value);
                m_ptr->dec_weak();
                if (m_ptr->weak == 0) {
                    m_ptr->do_delete(detail::DeleteType::Self);
                }
            }
        }
    }

    Rc& operator=(const Rc& other) noexcept {
        if (this != &other) {
            Rc(other.clone()).swap(*this);
        }
        return *this;
    }

    Rc& operator=(Rc&& other) noexcept {
        Rc(std::move(other)).swap(*this);
        return *this;
    }

    auto clone() const noexcept -> Rc {
        if (m_ptr) m_ptr->inc_strong();
        return Rc(m_ptr);
    }

    void swap(Rc& other) noexcept { std::swap(m_ptr, other.m_ptr); }

    auto get() -> value_t* { return m_ptr ? m_ptr->value : nullptr; }
    auto get() const -> const value_t* { return m_ptr ? m_ptr->value : nullptr; }

    auto operator*() -> value_t& { return *m_ptr->value; }
    auto operator*() const -> const value_t& { return *m_ptr->value; }
    auto operator->() -> value_t* { return m_ptr->value; }
    auto operator->() const -> const value_t* { return m_ptr->value; }

    auto strong_count() const -> std::size_t { return m_ptr ? m_ptr->strong : 0; }

    auto weak_count() const -> std::size_t { return m_ptr ? m_ptr->weak - 1 : 0; }

    auto is_unique() const -> bool { return strong_count() == 1 && weak_count() == 0; }

    auto downgrade() const -> Weak<T> {
        m_ptr->inc_weak();
        return Weak<T>(m_ptr);
    }

    auto size() const -> std::size_t {
        if constexpr (std::is_array_v<T>) {
            auto p = static_cast<const detail::RcInnerArrayImpl<T>*>(m_ptr);
            return p->size;
        } else {
            return 1;
        }
    }

    explicit operator bool() const noexcept { return m_ptr != nullptr; }
};

// Helper functions
export template<typename T, StoragePolicy P = StoragePolicy::Separate, typename... Args>
auto make_rc(Args&&... args) -> Rc<T> {
    return Rc<T>::template make<P>(std::forward<Args>(args)...);
}

// Non-member functions
export template<typename T>
void swap(Rc<T>& lhs, Rc<T>& rhs) noexcept {
    lhs.swap(rhs);
}

} // namespace rstd::rc