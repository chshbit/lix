#pragma once

#include "overloaded.hh"

#include <coroutine>
#include <optional>
#include <utility>
#include <variant>

namespace nix {

template<typename T, typename Transform = std::identity>
struct Generator : private Generator<T, void>
{
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    explicit Generator(handle_type h) : Generator<T, void>{h, h.promise().state} {}

    using Generator<T, void>::operator bool;
    using Generator<T, void>::operator();

    operator Generator<T, void> &() &
    {
        return *this;
    }
    operator Generator<T, void>() &&
    {
        return std::move(*this);
    }
};

template<typename T>
struct Generator<T, void>
{
    template<typename T2, typename Transform>
    friend struct Generator<T2, Transform>::promise_type;

    struct promise_state;

    struct _link
    {
        std::coroutine_handle<> handle{};
        promise_state * state{};
    };

    struct promise_state
    {
        std::variant<_link, T> value{};
        std::exception_ptr exception{};
        _link parent{};
    };

    // NOTE coroutine handles are LiteralType, own a memory resource (that may
    // itself own unique resources), and are "typically TriviallyCopyable". we
    // need to take special care to wrap this into a less footgunny interface,
    // which mostly means move-only.
    Generator(Generator && other)
    {
        swap(other);
    }

    Generator & operator=(Generator && other)
    {
        Generator(std::move(other)).swap(*this);
        return *this;
    }

    ~Generator()
    {
        if (h) {
            h.destroy();
        }
    }

    explicit operator bool()
    {
        return ensure();
    }

    T operator()()
    {
        ensure();
        auto result = std::move(*current);
        current = nullptr;
        return result;
    }

protected:
    std::coroutine_handle<> h{};
    _link active{};
    T * current{};

    Generator(std::coroutine_handle<> h, promise_state & state) : h(h), active(h, &state) {}

    void swap(Generator & other)
    {
        std::swap(h, other.h);
        std::swap(active, other.active);
        std::swap(current, other.current);
    }

    bool ensure()
    {
        while (!current && active.handle) {
            active.handle.resume();
            auto & p = *active.state;
            if (p.exception) {
                std::rethrow_exception(p.exception);
            } else if (active.handle.done()) {
                active = p.parent;
            } else {
                std::visit(
                    overloaded{
                        [&](_link & inner) {
                            auto base = inner.state;
                            while (base->parent.handle) {
                                base = base->parent.state;
                            }
                            base->parent = active;
                            active = inner;
                        },
                        [&](T & value) { current = &value; },
                    },
                    p.value
                );
            }
        }
        return current;
    }
};

template<typename T, typename Transform>
struct Generator<T, Transform>::promise_type
{
    Generator<T, void>::promise_state state;
    Transform convert;
    std::optional<Generator<T, void>> inner;

    Generator get_return_object()
    {
        return Generator(handle_type::from_promise(*this));
    }
    std::suspend_always initial_suspend()
    {
        return {};
    }
    std::suspend_always final_suspend() noexcept
    {
        return {};
    }
    void unhandled_exception()
    {
        state.exception = std::current_exception();
    }

    template<typename From>
        requires requires(Transform t, From && f) {
            {
                t(std::forward<From>(f))
            } -> std::convertible_to<T>;
        }
    std::suspend_always yield_value(From && from)
    {
        state.value = convert(std::forward<From>(from));
        return {};
    }

    template<typename From>
        requires requires(Transform t, From f) { static_cast<Generator<T, void>>(t(std::move(f))); }
    std::suspend_always yield_value(From from)
    {
        inner = static_cast<Generator<T, void>>(convert(std::move(from)));
        state.value = inner->active;
        return {};
    }

    void return_void() {}
};

}
