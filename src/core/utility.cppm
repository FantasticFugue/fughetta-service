export module utility;

import std;
export import httpant.exception;

export namespace util {
    using http::exceptions::current_exception;
    using http::exceptions::make_exception_ptr;
    using std::rethrow_exception;

    // Retains an error's concrete type across a module boundary. httpant's
    // exception module captures through exception_capture; the capture runs in
    // this module unit, so clang records the copy constructor in the throw
    // metadata on the Microsoft ABI (see httpant.exception).
    template <class Error> struct retained_error : http::exceptions::exception_capture {
        auto capture() const noexcept -> std::exception_ptr override { return http::exceptions::capture(static_cast<const Error&>(*this)); }
    };

    // A move-only, type-erased callable with a small buffer, for the places the
    // scheduler and the service layers hand a callback to somebody and forget
    // the concrete type (a post to a worker, a notification sink, a gate
    // policy). A callable that fits in three pointers — a `this`-capturing
    // lambda, a plain function pointer and a small closure — is stored inline,
    // so wiring one up does not allocate; anything larger falls back to the
    // heap.
    //
    // This exists once. libc++ has no std::move_only_function for this target
    // (__cpp_lib_move_only_function is undefined), and the two core modules used
    // to export their own identical copy of this class, which is a redefinition
    // waiting for one translation unit to import both.
    template <class Signature> class move_only_function;

    template <class R, class... A> class move_only_function<R(A...)> {
        static constexpr std::size_t buffer_size = 3 * sizeof(void*);

    public:
        move_only_function() noexcept = default;
        move_only_function(std::nullptr_t) noexcept {}

        template <class F>
            requires(!std::same_as<std::remove_cvref_t<F>, move_only_function> && std::invocable<std::decay_t<F>&, A...>)
        move_only_function(F&& function) {
            emplace<std::decay_t<F>>(std::forward<F>(function));
        }

        move_only_function(move_only_function&& other) noexcept { other.move_into(*this); }
        auto operator=(move_only_function&& other) noexcept -> move_only_function& {
            if (this != &other) {
                reset();
                other.move_into(*this);
            }
            return *this;
        }
        move_only_function(const move_only_function&) = delete;
        auto operator=(const move_only_function&) -> move_only_function& = delete;
        ~move_only_function() { reset(); }

        auto operator()(A... args) -> R { return table_->call(object(), std::forward<A>(args)...); }

        explicit operator bool() const noexcept { return table_ != nullptr; }

        void reset() noexcept {
            if (table_ != nullptr) {
                table_->destroy(object());
                table_ = nullptr;
                object_ = nullptr;
            }
        }

    private:
        struct table {
            R (*call)(void*, A&&...);
            // Move the callable out of |source| into |buffer| and return where it
            // now lives: |buffer| for an inline callable, |source| for a heap one.
            void* (*move)(void* source, void* buffer) noexcept;
            void (*destroy)(void*) noexcept;
        };

        template <class F, bool Inline> static auto table_for() noexcept -> const table* {
            static constexpr table value{
                .call = [](void* object, A&&... args) -> R { return std::invoke(*static_cast<F*>(object), std::forward<A>(args)...); },
                .move = [](void* source, void* buffer) noexcept -> void* {
                    if constexpr (Inline) {
                        auto* moved = ::new (buffer) F(std::move(*static_cast<F*>(source)));
                        static_cast<F*>(source)->~F();
                        return moved;
                    } else {
                        return source;
                    }
                },
                .destroy =
                    [](void* object) noexcept {
                        if constexpr (Inline)
                            static_cast<F*>(object)->~F();
                        else
                            delete static_cast<F*>(object);
                    },
            };
            return &value;
        }

        template <class F, class... Args> void emplace(Args&&... args) {
            constexpr bool inline_stored = sizeof(F) <= buffer_size && alignof(F) <= alignof(std::max_align_t) && std::is_nothrow_move_constructible_v<F>;
            if constexpr (inline_stored) {
                object_ = ::new (static_cast<void*>(buffer_.data())) F(std::forward<Args>(args)...);
                table_ = table_for<F, true>();
            } else {
                object_ = new F(std::forward<Args>(args)...);
                table_ = table_for<F, false>();
            }
        }

        void move_into(move_only_function& destination) noexcept {
            if (table_ == nullptr)
                return;
            destination.table_ = table_;
            destination.object_ = table_->move(object(), destination.buffer_.data());
            // The callable now belongs to |destination|: this one owns nothing,
            // so its destructor must not destroy it a second time (for the heap
            // case, that would be a double free of the same object).
            table_ = nullptr;
            object_ = nullptr;
        }

        [[nodiscard]] auto object() const noexcept -> void* { return object_; }

        alignas(std::max_align_t) std::array<std::byte, buffer_size> buffer_{};
        void* object_{nullptr};
        table const* table_{nullptr};
    };

    // A borrowed callable for a call that finishes before the callable goes away: two pointers, no allocation. It is for a
    // callback that crosses a module implementation unit, where a template cannot go; libc++ has no std::function_ref.
    template <class Signature> class function_ref;

    template <class R, class... A> class function_ref<R(A...)> {
    public:
        template <class F>
            requires(!std::same_as<std::remove_cvref_t<F>, function_ref> && std::invocable<F&, A...>)
        function_ref(F&& function) noexcept
            : object_(const_cast<void*>(static_cast<const void*>(std::addressof(function)))),
              call_([](void* object, A&&... args) -> R { return std::invoke(*static_cast<std::remove_reference_t<F>*>(object), std::forward<A>(args)...); }) {}

        auto operator()(A... args) const -> R { return call_(object_, std::forward<A>(args)...); }

    private:
        void* object_;
        R (*call_)(void*, A&&...);
    };

    // C++23 std::generator<Ref, V> without the allocator parameter and ranges::elements_of, which libc++ does not ship yet.
    // The body runs only while the caller iterates, so a reader that `co_yield`s a span of its own buffer hands each piece
    // over without copying it; an exception from the body propagates out of the iterator step that ran it.
    template <class Ref, class V = void> class generator : public std::ranges::view_interface<generator<Ref, V>> {
        using value = std::conditional_t<std::is_void_v<V>, std::remove_cvref_t<Ref>, V>;
        using reference = std::conditional_t<std::is_void_v<V>, Ref&&, Ref>;

    public:
        using yielded = std::conditional_t<std::is_reference_v<reference>, reference, const reference&>;

        struct promise_type {
            std::add_pointer_t<yielded> current = nullptr;
            auto get_return_object() noexcept -> generator { return generator{std::coroutine_handle<promise_type>::from_promise(*this)}; }
            auto initial_suspend() const noexcept -> std::suspend_always { return {}; }
            auto final_suspend() const noexcept -> std::suspend_always { return {}; }
            auto yield_value(yielded value) noexcept -> std::suspend_always {
                current = std::addressof(value);
                return {};
            }
            // An lvalue yielded through an rvalue reference is copied into the frame for the suspension.
            auto yield_value(const std::remove_reference_t<yielded>& value)
                requires std::is_rvalue_reference_v<yielded> && std::constructible_from<std::remove_cvref_t<yielded>, const std::remove_reference_t<yielded>&>
            {
                struct copy {
                    std::remove_cvref_t<yielded> value;
                    auto await_ready() const noexcept -> bool { return false; }
                    void await_suspend(std::coroutine_handle<promise_type> handle) noexcept { handle.promise().current = std::addressof(value); }
                    void await_resume() const noexcept {}
                };
                return copy{value};
            }
            template <class U> auto await_transform(U&&) -> std::suspend_never = delete;
            void return_void() const noexcept {}
            void unhandled_exception() { throw; }
        };

        class iterator {
        public:
            using value_type = value;
            using difference_type = std::ptrdiff_t;
            iterator(iterator&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
            auto operator=(iterator&& other) noexcept -> iterator& {
                handle_ = std::exchange(other.handle_, {});
                return *this;
            }
            auto operator*() const noexcept(std::is_nothrow_copy_constructible_v<reference>) -> reference {
                return static_cast<reference>(*handle_.promise().current);
            }
            auto operator++() -> iterator& {
                handle_.resume();
                return *this;
            }
            void operator++(int) { ++*this; }
            friend auto operator==(const iterator& it, std::default_sentinel_t) noexcept -> bool { return it.handle_.done(); }

        private:
            friend generator;
            explicit iterator(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}
            std::coroutine_handle<promise_type> handle_;
        };

        generator(const generator&) = delete;
        generator(generator&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
        ~generator() {
            if (handle_)
                handle_.destroy();
        }
        auto operator=(generator other) noexcept -> generator& {
            std::swap(handle_, other.handle_);
            return *this;
        }

        auto begin() -> iterator {
            handle_.resume();
            return iterator{handle_};
        }
        auto end() const noexcept -> std::default_sentinel_t { return {}; }

    private:
        explicit generator(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}
        std::coroutine_handle<promise_type> handle_;
    };

    // A hasher for `std::unordered_map`/`std::unordered_set` keyed by `std::string` that also accepts a
    // `std::string_view` lookup, so a query does not allocate a temporary `std::string`. Pair it with
    // `std::equal_to<>`; the marker member opts into the library's heterogeneous lookup.
    struct transparent_string_hash {
        using is_transparent = void;
        auto operator()(std::string_view s) const noexcept -> std::size_t { return std::hash<std::string_view>{}(s); }
        auto operator()(const std::string& s) const noexcept -> std::size_t { return std::hash<std::string_view>{}(s); }
    };
} // namespace util
