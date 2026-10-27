export module index;
import std;
import codec;

export namespace nodes {
    template <class T> class persistent_index {
    public:
        struct node_data {
            std::string key;
            T item{};
            std::string value_digest, left, right, left_object, right_object;
            std::size_t count = 1, left_count = 0, right_count = 0;
            std::uint64_t mass = 1, left_mass = 0, right_mass = 0;
            bool bound = false, left_bound = true, right_bound = true;
            auto operator==(const node_data&) const -> bool = default;
        };
        struct backend {
            virtual ~backend() = default;
            virtual auto load(std::string_view digest) -> node_data = 0;
            virtual auto digest(const T& item) const -> std::string = 0;
            virtual auto store(const node_data& node) -> std::string = 0;
        };

    private:
        struct branch;
        using pointer = std::shared_ptr<const branch>;
        struct branch {
            mutable std::pair<std::string, T> item;
            mutable std::string priority, value_digest, digest, object;
            mutable pointer left, right;
            mutable std::shared_ptr<backend> storage;
            mutable std::size_t count = 1, left_count = 0, right_count = 0;
            mutable std::uint64_t mass = 1, left_mass = 0, right_mass = 0;
            mutable std::string left_digest, right_digest, left_object, right_object;
            mutable bool loaded = true, dirty = true, bound = false, left_bound = true, right_bound = true;
        };
        pointer root_;
        mutable std::shared_ptr<backend> storage_;
        mutable std::uint64_t flush_visits_ = 0;

        static auto branch_digest(std::string_view key, std::string_view left, std::string_view right, std::string_view value) -> std::string {
            return digest_of(codec::encode(
                value::map{{"key", std::string(key)}, {"left", std::string(left)}, {"right", std::string(right)}, {"value", std::string(value)}}));
        }
        static auto weight(const T& item) -> std::uint64_t {
            if constexpr (requires { item.count; })
                return item.count;
            else
                return 1;
        }
        static auto shell(std::string digest, std::string object, std::size_t count, bool bound, std::uint64_t mass, const std::shared_ptr<backend>& storage)
            -> pointer {
            if (digest.empty()) {
                if (count || mass || !object.empty())
                    fail(nodes::code::storage_failure, "empty index root has nonempty counts");
                return {};
            }
            if (!count || !mass ||
                (digest.size() != 64 || std::ranges::any_of(digest, [](char c) { return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')); })))
                fail(nodes::code::storage_failure, "index root counts or identity differ");
            auto out = std::make_shared<branch>();
            if (object.empty())
                fail(nodes::code::missing_objects, "authenticated index object is absent");
            out->digest = std::move(digest);
            out->object = std::move(object);
            out->count = count;
            out->mass = mass;
            out->bound = bound;
            out->storage = storage;
            out->loaded = false;
            out->dirty = false;
            return out;
        }
        static void ensure(const pointer& p) {
            if (!p || p->loaded)
                return;
            if (!p->storage)
                fail(nodes::code::missing_objects, "persistent index branch has no authenticated loader");
            auto data = p->storage->load(p->object);
            auto actual = p->storage->digest(data.item);
            if (data.count != p->count || data.mass != p->mass || data.key.empty() || data.value_digest.empty() || data.value_digest != actual || !data.count ||
                data.left_count + data.right_count + 1 != data.count || data.bound != (data.left_bound && data.right_bound) ||
                branch_digest(data.key, data.left, data.right, data.value_digest) != p->digest)
                fail(nodes::code::storage_failure, "persistent index branch or value differs from its authenticated digest");
            p->item = {std::move(data.key), std::move(data.item)};
            p->priority = digest_of(p->item.first);
            p->value_digest = std::move(data.value_digest);
            p->left_digest = data.left;
            p->left_object = data.left_object;
            p->right_digest = data.right;
            p->right_object = data.right_object;
            p->left_count = data.left_count;
            p->right_count = data.right_count;
            p->left_bound = data.left_bound;
            p->right_bound = data.right_bound;
            if (data.mass != data.left_mass + data.right_mass + weight(p->item.second) || (data.left.empty() != (data.left_count == 0)) ||
                (data.right.empty() != (data.right_count == 0)))
                fail(nodes::code::storage_failure, "persistent index scalar counts differ");
            p->mass = data.mass;
            p->left_mass = data.left_mass;
            p->right_mass = data.right_mass;
            p->left = shell(std::move(data.left), std::move(data.left_object), data.left_count, data.left_bound, data.left_mass, p->storage);
            p->right = shell(std::move(data.right), std::move(data.right_object), data.right_count, data.right_bound, data.right_mass, p->storage);
            p->count = data.count;
            p->bound = data.bound;
            p->loaded = true;
        }
        static auto make(std::string key, T value, std::string hash, pointer a = {}, pointer b = {}) -> pointer {
            auto out = std::make_shared<branch>();
            out->item = {std::move(key), std::move(value)};
            out->priority = digest_of(out->item.first);
            out->value_digest = std::move(hash);
            out->left = std::move(a);
            out->right = std::move(b);
            out->count = 1 + (out->left ? out->left->count : 0) + (out->right ? out->right->count : 0);
            out->left_count = out->left ? out->left->count : 0;
            out->right_count = out->right ? out->right->count : 0;
            out->left_mass = out->left ? out->left->mass : 0;
            out->right_mass = out->right ? out->right->mass : 0;
            out->mass = out->left_mass + out->right_mass + weight(out->item.second);
            out->left_digest = out->left ? out->left->digest : std::string{};
            out->right_digest = out->right ? out->right->digest : std::string{};
            out->left_bound = !out->left || out->left->bound;
            out->right_bound = !out->right || out->right->bound;
            out->bound = !out->value_digest.empty() && out->left_bound && out->right_bound;
            out->digest = branch_digest(out->item.first, out->left_digest, out->right_digest, out->value_digest);
            return out;
        }
        static auto rank(const pointer& p) {
            ensure(p);
            return std::tie(p->priority, p->item.first);
        }
        static auto split(pointer p, std::string_view key) -> std::pair<pointer, pointer> {
            if (!p)
                return {};
            ensure(p);
            if (p->item.first < key) {
                auto [a, b] = split(p->right, key);
                return {make(p->item.first, p->item.second, p->value_digest, p->left, std::move(a)), std::move(b)};
            }
            auto [a, b] = split(p->left, key);
            return {std::move(a), make(p->item.first, p->item.second, p->value_digest, std::move(b), p->right)};
        }
        static auto insert(pointer p, const std::string& key, const T& item, const std::string& hash, const std::string& priority) -> pointer {
            if (!p)
                return make(key, item, hash);
            ensure(p);
            if (key == p->item.first)
                return item == p->item.second && hash == p->value_digest ? p : make(key, item, hash, p->left, p->right);
            if (std::tie(priority, key) < rank(p)) {
                auto [a, b] = split(p, key);
                return make(key, item, hash, std::move(a), std::move(b));
            }
            if (key < p->item.first)
                return make(p->item.first, p->item.second, p->value_digest, insert(p->left, key, item, hash, priority), p->right);
            return make(p->item.first, p->item.second, p->value_digest, p->left, insert(p->right, key, item, hash, priority));
        }
        static auto merge(pointer a, pointer b) -> pointer {
            if (!a)
                return b;
            if (!b)
                return a;
            if (rank(a) < rank(b)) {
                ensure(a);
                return make(a->item.first, a->item.second, a->value_digest, a->left, merge(a->right, std::move(b)));
            }
            ensure(b);
            return make(b->item.first, b->item.second, b->value_digest, merge(std::move(a), b->left), b->right);
        }
        static auto remove(pointer p, std::string_view key) -> pointer {
            if (!p)
                return p;
            ensure(p);
            if (key == p->item.first)
                return merge(p->left, p->right);
            if (key < p->item.first) {
                auto next = remove(p->left, key);
                return next == p->left ? p : make(p->item.first, p->item.second, p->value_digest, std::move(next), p->right);
            }
            auto next = remove(p->right, key);
            return next == p->right ? p : make(p->item.first, p->item.second, p->value_digest, p->left, std::move(next));
        }
        template <class F> static void compare(pointer a, pointer b, F& f) {
            if (a == b || (a && b && !a->object.empty() && a->object == b->object))
                return;
            if (!a) {
                for (auto it = iterator{b}; it != iterator{}; ++it)
                    f(it->first, static_cast<const T*>(nullptr), &it->second);
                return;
            }
            if (!b) {
                for (auto it = iterator{a}; it != iterator{}; ++it)
                    f(it->first, &it->second, static_cast<const T*>(nullptr));
                return;
            }
            ensure(a);
            ensure(b);
            if (a->item.first == b->item.first) {
                compare(a->left, b->left, f);
                if (a->item.second != b->item.second)
                    f(a->item.first, &a->item.second, &b->item.second);
                compare(a->right, b->right, f);
                return;
            }
            if (rank(a) < rank(b)) {
                auto [left, right] = split(b, a->item.first);
                const T* value = nullptr;
                auto it = iterator{right, a->item.first};
                if (it != iterator{} && it->first == a->item.first)
                    value = &it->second;
                compare(a->left, left, f);
                if (!value || a->item.second != *value)
                    f(a->item.first, &a->item.second, value);
                compare(a->right, remove(right, a->item.first), f);
            } else {
                auto [left, right] = split(a, b->item.first);
                const T* value = nullptr;
                auto it = iterator{right, b->item.first};
                if (it != iterator{} && it->first == b->item.first)
                    value = &it->second;
                compare(left, b->left, f);
                if (!value || *value != b->item.second)
                    f(b->item.first, value, &b->item.second);
                compare(remove(right, b->item.first), b->right, f);
            }
        }
        static void flush(const pointer& p, const std::shared_ptr<backend>& storage, std::uint64_t& visits) {
            if (!p || (!p->dirty && p->storage == storage))
                return;
            ++visits;
            if (!p->loaded)
                ensure(p);
            flush(p->left, storage, visits);
            flush(p->right, storage, visits);
            if (p->value_digest.empty() || p->value_digest != storage->digest(p->item.second))
                fail(nodes::code::invalid_record, "persistent index value digest is not content-bound");
            p->storage = storage;
            p->left_object = p->left ? p->left->object : std::string{};
            p->right_object = p->right ? p->right->object : std::string{};
            p->object = storage->store({.key = p->item.first,
                                        .item = p->item.second,
                                        .value_digest = p->value_digest,
                                        .left = p->left_digest,
                                        .right = p->right_digest,
                                        .left_object = p->left_object,
                                        .right_object = p->right_object,
                                        .count = p->count,
                                        .left_count = p->left_count,
                                        .right_count = p->right_count,
                                        .mass = p->mass,
                                        .left_mass = p->left_mass,
                                        .right_mass = p->right_mass,
                                        .bound = p->bound,
                                        .left_bound = p->left_bound,
                                        .right_bound = p->right_bound});
            p->dirty = false;
        }
        static std::size_t loaded(const pointer& p) {
            if (!p || !p->loaded)
                return 0;
            return 1 + loaded(p->left) + loaded(p->right);
        }

    public:
        class iterator {
            std::vector<pointer> stack_;
            void descend(pointer p) {
                while (p) {
                    ensure(p);
                    stack_.push_back(p);
                    p = p->left;
                }
            }

        public:
            using value_type = std::pair<std::string, T>;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;
            iterator() = default;
            explicit iterator(pointer p) { descend(std::move(p)); }
            iterator(pointer p, std::string_view key) {
                while (p) {
                    ensure(p);
                    if (p->item.first >= key) {
                        stack_.push_back(p);
                        p = p->left;
                    } else
                        p = p->right;
                }
            }
            auto operator*() const -> const value_type& {
                ensure(stack_.back());
                return stack_.back()->item;
            }
            auto operator->() const -> const value_type* {
                ensure(stack_.back());
                return &stack_.back()->item;
            }
            auto operator++() -> iterator& {
                auto p = stack_.back();
                stack_.pop_back();
                ensure(p);
                descend(p->right);
                return *this;
            }
            auto operator++(int) -> iterator {
                auto old = *this;
                ++*this;
                return old;
            }
            auto operator==(const iterator& b) const -> bool {
                return (stack_.empty() ? nullptr : stack_.back().get()) == (b.stack_.empty() ? nullptr : b.stack_.back().get());
            }
        };

        auto begin() const -> iterator { return iterator{root_}; }
        auto end() const -> iterator { return {}; }
        auto lower_bound(std::string_view key) const -> iterator { return iterator{root_, key}; }
        auto find(std::string_view key) const -> iterator {
            auto found = lower_bound(key);
            return found != end() && found->first == key ? found : end();
        }
        auto contains(std::string_view key) const -> bool { return find(key) != end(); }
        auto size() const -> std::size_t { return root_ ? root_->count : 0; }
        auto empty() const -> bool { return !root_; }
        auto digest() const -> std::string { return root_ ? root_->digest : std::string{}; }
        auto object() const -> std::string { return root_ ? root_->object : std::string{}; }
        auto content_bound() const -> bool { return !root_ || root_->bound; }
        void adopt(std::string digest, std::string object, std::size_t count, bool bound, std::uint64_t mass, std::shared_ptr<backend> storage) {
            storage_ = std::move(storage);
            root_ = shell(std::move(digest), std::move(object), count, bound, mass, storage_);
        }
        auto mass() const -> std::uint64_t { return root_ ? root_->mass : 0; }
        auto floor(std::string_view key, bool strict = false) const -> iterator {
            auto p = root_;
            std::string found;
            bool have = false;
            while (p) {
                ensure(p);
                if (p->item.first < key || (!strict && p->item.first == key)) {
                    found = p->item.first;
                    have = true;
                    p = p->right;
                } else
                    p = p->left;
            }
            return have ? iterator{root_, found} : end();
        }
        auto at_mass(std::uint64_t offset) const -> std::pair<iterator, std::uint64_t> {
            auto p = root_;
            while (p) {
                ensure(p);
                if (offset < p->left_mass) {
                    p = p->left;
                    continue;
                }
                offset -= p->left_mass;
                if (offset < weight(p->item.second))
                    return {iterator{root_, p->item.first}, offset};
                offset -= weight(p->item.second);
                p = p->right;
            }
            fail(nodes::code::invalid_record, "sequence offset exceeds the authenticated scalar count");
        }
        auto mass_before(std::string_view key) const -> std::uint64_t {
            auto p = root_;
            std::uint64_t offset = 0;
            while (p) {
                ensure(p);
                if (key <= p->item.first)
                    p = p->left;
                else {
                    offset += p->left_mass + weight(p->item.second);
                    p = p->right;
                }
            }
            return offset;
        }
        void set(std::string key, T item, std::string hash = {}) {
            auto priority = digest_of(key);
            root_ = insert(root_, key, item, hash, priority);
        }
        void erase(std::string_view key) { root_ = remove(root_, key); }
        template <class F> void difference(const persistent_index& after, F f) const { compare(root_, after.root_, f); }
        void persist(std::shared_ptr<backend> storage) const {
            if (!storage || (storage_ && storage_ != storage))
                fail(nodes::code::invalid_record, "persistent index backend identity changed");
            storage_ = storage;
            flush(root_, storage, flush_visits_);
        }
        auto loaded_nodes() const -> std::size_t { return loaded(root_); }
        auto persistence_work() const -> std::uint64_t { return flush_visits_; }
    };

    struct causal_set {
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        auto contains(std::size_t n) const -> bool {
            auto it = std::upper_bound(ranges.begin(), ranges.end(), n, [](auto value, auto& range) { return value < range.first; });
            return it != ranges.begin() && n < std::prev(it)->second;
        }
        void unite(const causal_set& other) {
            std::vector<std::pair<std::size_t, std::size_t>> next;
            next.reserve(ranges.size() + other.ranges.size());
            auto a = ranges.begin();
            auto b = other.ranges.begin();
            while (a != ranges.end() || b != other.ranges.end()) {
                auto item = b == other.ranges.end() || (a != ranges.end() && *a < *b) ? *a++ : *b++;
                if (!next.empty() && item.first <= next.back().second)
                    next.back().second = std::max(next.back().second, item.second);
                else
                    next.push_back(item);
            }
            ranges = std::move(next);
        }
        void add(std::size_t n) {
            causal_set one;
            one.ranges.emplace_back(n, n + 1);
            unite(one);
        }
        auto without(const causal_set& other) const -> causal_set {
            causal_set out;
            std::size_t j = 0;
            for (auto [first, last] : ranges) {
                while (j < other.ranges.size() && other.ranges[j].second <= first)
                    ++j;
                for (auto k = j; k < other.ranges.size() && other.ranges[k].first < last; ++k) {
                    auto [lo, hi] = other.ranges[k];
                    if (first < lo)
                        out.ranges.emplace_back(first, std::min(lo, last));
                    first = std::max(first, hi);
                    if (first >= last)
                        break;
                }
                if (first < last)
                    out.ranges.emplace_back(first, last);
            }
            return out;
        }
        template <class F> void each(F f) const {
            for (auto [first, last] : ranges)
                for (auto i = first; i < last; ++i)
                    f(i);
        }
    };
} // namespace nodes
