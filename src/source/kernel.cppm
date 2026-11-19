module;
#include <roaring/roaring64.h>
export module kernel;
import std;
import content;
import codec;

export namespace nodes {
    struct element {
        std::string run;
        std::uint64_t offset = 0;
        auto operator<=>(const element&) const = default;
        auto encoded() const -> value { return value::map{{"offset", offset}, {"run", run}}; }
        static auto decode(const value& v) -> element {
            fields(v, {"offset", "run"});
            return {v.at("run").text(), v.at("offset").number()};
        }
    };
    struct fragment {
        std::string run;
        std::uint64_t first = 0, count = 0;
        auto operator==(const fragment&) const -> bool = default;
    };
    // In memory, the causal closure is a 64-bit bitmap. Persistence observes sorted half-open ranges.
    namespace causal_bitmap = roaring::api;
    class causal_set {
        using bitmap = std::unique_ptr<causal_bitmap::roaring64_bitmap_t, decltype(&causal_bitmap::roaring64_bitmap_free)>;
        bitmap bits_;

        explicit causal_set(causal_bitmap::roaring64_bitmap_t* bits) : bits_{bits, causal_bitmap::roaring64_bitmap_free} {
            if (!bits_)
                throw std::bad_alloc{};
        }

    public:
        causal_set() : causal_set(causal_bitmap::roaring64_bitmap_create()) {}
        causal_set(const causal_set& other) : causal_set(causal_bitmap::roaring64_bitmap_copy(other.bits_.get())) {}
        causal_set(causal_set&&) noexcept = default;
        auto operator=(causal_set&&) noexcept -> causal_set& = default;
        auto operator=(const causal_set& other) -> causal_set& {
            if (this != &other)
                *this = causal_set{other};
            return *this;
        }
        auto contains(std::size_t n) const -> bool { return causal_bitmap::roaring64_bitmap_contains(bits_.get(), n); }
        auto empty() const -> bool { return causal_bitmap::roaring64_bitmap_is_empty(bits_.get()); }
        auto size() const -> std::uint64_t { return causal_bitmap::roaring64_bitmap_get_cardinality(bits_.get()); }
        void unite(const causal_set& other) {
            if (this != &other)
                causal_bitmap::roaring64_bitmap_or_inplace(bits_.get(), other.bits_.get());
        }
        void add(std::size_t n) { causal_bitmap::roaring64_bitmap_add(bits_.get(), n); }
        void add_range(std::uint64_t first, std::uint64_t last) { causal_bitmap::roaring64_bitmap_add_range(bits_.get(), first, last); }
        auto without(const causal_set& other) const -> causal_set {
            return causal_set{causal_bitmap::roaring64_bitmap_andnot(bits_.get(), other.bits_.get())};
        }
        auto ranges() const -> std::vector<std::pair<std::uint64_t, std::uint64_t>> {
            std::unique_ptr<causal_bitmap::roaring64_iterator_t, decltype(&causal_bitmap::roaring64_iterator_free)> it{
                causal_bitmap::roaring64_iterator_create(bits_.get()), causal_bitmap::roaring64_iterator_free};
            if (!it)
                throw std::bad_alloc{};
            std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
            std::array<causal_bitmap::roaring64_range_closed_t, 32> block;
            while (const auto count = causal_bitmap::roaring64_iterator_read_ranges(it.get(), block.data(), block.size()))
                for (const auto& range : std::span{block}.first(count))
                    out.emplace_back(range.min, range.max + 1);
            return out;
        }
        template <class F> void each(F f) const {
            for (auto [first, last] : ranges())
                for (auto i = first; i < last; ++i)
                    f(i);
        }
    };
    struct source_file;
    struct source_state;
    // What a kernel reads from its node's storage, which outlives the kernel and everything it loads.
    struct causal_reader {
        virtual ~causal_reader() = default;
        virtual auto header(std::size_t index) -> value = 0;
        virtual auto locate(const std::string& id) -> std::optional<std::size_t> = 0;
        virtual auto records(const std::string& kind, const std::string& key) -> std::vector<value> = 0;
        virtual auto file_text(const source_file& file) -> std::string = 0;
        virtual auto projection(const std::vector<std::string>& heads) -> std::shared_ptr<const source_state> = 0;
        // The newest recorded manifest a version without one was written against, and its frontier.
        virtual auto base(const std::vector<std::string>& heads) -> std::pair<std::vector<std::string>, std::shared_ptr<const source_state>> = 0;
        virtual auto file_changed(const std::string& file, const causal_set& missing) -> bool = 0;
    };
    struct source_file {
        std::string id, path, kind = "text", digest;
        mutable std::string text;
        bool executable = false, present = true;
        mutable std::vector<fragment> fragments;
        mutable bool fragments_ready = true;
        std::uint64_t stored_bytes = 0, stored_scalars = 0;
        // Set while the content is still in storage, under `digest`.
        mutable causal_reader* stored = nullptr;
        auto content() const -> const std::string& {
            if (stored) {
                text = stored->file_text(*this);
                stored = nullptr;
            }
            return text;
        }
        auto bytes() const -> std::uint64_t { return stored ? stored_bytes : text.size(); }
        auto at(std::uint64_t offset) const -> element {
            for (auto& f : fragments) {
                if (offset < f.count)
                    return {f.run, f.first + offset};
                offset -= f.count;
            }
            fail(nodes::code::invalid_record, "text position outside visible source");
        }
        // The runs holding the `count` visible characters from `offset`, with adjacent pieces of one run joined.
        auto span(std::uint64_t offset, std::uint64_t count) const -> std::vector<fragment> {
            if (offset > size() || count > size() - offset)
                fail(nodes::code::invalid_record, "text range outside visible source");
            std::vector<fragment> pieces;
            for (auto& f : fragments) {
                if (!count)
                    break;
                if (offset >= f.count) {
                    offset -= f.count;
                    continue;
                }
                auto taken = std::min(count, f.count - offset);
                pieces.push_back({f.run, f.first + offset, taken});
                count -= taken;
                offset = 0;
            }
            std::vector<fragment> out;
            for (auto& f : pieces)
                if (!out.empty() && out.back().run == f.run && out.back().first + out.back().count == f.first)
                    out.back().count += f.count;
                else
                    out.push_back(std::move(f));
            return out;
        }
        auto size() const -> std::uint64_t {
            std::uint64_t n = 0;
            for (auto& f : fragments)
                n += f.count;
            return n;
        }
        // Where a character is in this file, or nothing when it is not visible here.
        auto position(const element& selected) const -> std::optional<std::uint64_t> {
            std::uint64_t n = 0;
            for (auto& f : fragments) {
                if (f.run == selected.run && selected.offset >= f.first && selected.offset - f.first < f.count)
                    return n + (selected.offset - f.first);
                n += f.count;
            }
            return std::nullopt;
        }
    };
    struct source_state {
        std::map<std::string, std::shared_ptr<const source_file>> files, hidden;
        std::map<std::string, std::shared_ptr<const std::set<std::string>>> paths;
        std::map<std::string, std::vector<std::string>> file_conflicts, path_conflicts, control_conflicts, dependency_conflicts;
        std::map<std::string, bool> activation, resolutions;
        causal_set causal;
        std::vector<std::string> conflicts;
        std::string digest;
        auto path(std::string_view path) const -> const source_file& {
            auto found = paths.find(std::string(path));
            if (found == paths.end() || found->second->empty())
                fail(nodes::code::invalid_record, "source path not found: " + std::string(path));
            if (found->second->size() != 1)
                fail(nodes::code::source_conflict, "path has multiple file identities");
            return *files.find(*found->second->begin())->second;
        }
    };
    // The version's deterministic state root: SHA-256 over the visible files, canonical sorted by (path, file_id).
    inline auto content_root(const std::map<std::string, std::shared_ptr<const source_file>>& files) -> std::string {
        std::vector<const source_file*> rows;
        rows.reserve(files.size());
        for (auto& [_, file] : files)
            if (file->present)
                rows.push_back(file.get());
        std::ranges::sort(rows, [](const source_file* a, const source_file* b) { return std::tie(a->path, a->id) < std::tie(b->path, b->id); });
        content::sha256 digest;
        for (auto* file : rows)
            digest.update(codec::encode(value::map{{"digest", file->digest},
                                                   {"executable", file->executable},
                                                   {"file", file->id},
                                                   {"kind", file->kind},
                                                   {"path", file->path},
                                                   {"size", file->bytes()}}));
        return content::to_hex(digest.finish()).str();
    }
    inline auto state_digest(std::string_view root, const std::vector<std::string>& conflicts) -> std::string {
        return digest_of(codec::encode(value::map{{"conflicts", strings(conflicts)}, {"files", std::string(root)}}));
    }
    inline auto state_digest(const source_state& state) -> std::string { return state_digest(content_root(state.files), state.conflicts); }

    class kernel {
        // The `count` characters of one insertion run from offset `first` that an erase removes.
        struct target {
            const std::string* run = nullptr;
            std::uint64_t first = 0, count = 0;
            auto operator<(const target& other) const -> bool { return std::tie(*run, first) < std::tie(*other.run, other.first); }
            auto covers(const element& other) const -> bool { return *run == other.run && other.offset >= first && other.offset - first < count; }
            auto encoded() const -> value { return value::map{{"count", count}, {"first", first}, {"run", *run}}; }
        };
        // Half-open offset ranges of one run, sorted and disjoint.
        using spans = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
        static void merge(spans& ranges) {
            std::ranges::sort(ranges);
            std::size_t kept = 0;
            for (auto& range : ranges)
                if (kept && range.first <= ranges[kept - 1].second)
                    ranges[kept - 1].second = std::max(ranges[kept - 1].second, range.second);
                else
                    ranges[kept++] = range;
            ranges.resize(kept);
        }
        // The first range of `ranges` that ends after `offset`.
        static auto after(const spans& ranges, std::uint64_t offset) { return std::ranges::upper_bound(ranges, offset, {}, &spans::value_type::second); }
        static auto covered(const spans& ranges, std::uint64_t offset) -> bool {
            auto found = after(ranges, offset);
            return found != ranges.end() && found->first <= offset;
        }
        struct run_less {
            auto operator()(const std::string* a, const std::string* b) const -> bool { return *a < *b; }
        };
        struct effect {
            std::string id, type, file, text;
            std::size_t event = 0, ordinal = 0;
            element left, right;
            std::vector<target> targets;
            std::vector<std::string> selections;
            value property;
            mutable std::u32string scalar_text;
            auto scalar_size() const -> std::size_t { return scalar_text.empty() ? text.size() : scalar_text.size(); }
            auto scalar_at(std::size_t offset) const -> char32_t {
                return scalar_text.empty() ? static_cast<unsigned char>(text[offset]) : scalar_text[offset];
            }
        };
        struct event {
            std::string id, author, operation;
            std::uint64_t clock = 0;
            causal_set ancestors;
            std::deque<effect> effects;
            bool complete = true;
            std::size_t retained_effects = 0;
            std::unordered_set<std::size_t> loaded_ordinals;
            std::vector<std::string> dependencies, created;
            std::string request_digest, effect_digest;
        };
        struct cached_file {
            std::vector<const effect*> effects;
            std::vector<bool> activation;
            std::shared_ptr<source_file> file;
            std::vector<std::string> conflicts;
        };
        mutable std::map<std::size_t, event> events_;
        std::size_t event_count_ = 0, retained_count_ = 0;
        causal_reader* reader_ = nullptr;
        mutable std::set<std::string> loaded_files_, loaded_effects_, loaded_controls_, loaded_dependencies_;
        mutable std::set<std::size_t> loaded_events_;
        mutable std::map<std::string, std::size_t> index_;
        mutable std::map<std::string, std::vector<const effect*>> effects_;
        mutable std::map<std::string, std::vector<std::size_t>> file_creators_;
        mutable std::map<std::string, std::vector<const effect*>> presence_;
        mutable std::map<std::string, std::vector<const effect*>> file_effects_;
        mutable std::vector<const effect*> control_effects_;
        mutable std::map<std::string, std::vector<const effect*>> activation_writes_;
        mutable std::map<std::string, std::vector<const effect*>> resolution_writes_;
        mutable std::map<std::string, std::vector<std::size_t>> dependency_events_;
        mutable std::map<std::string, std::set<std::string>> path_files_;
        mutable std::map<std::string, std::string> blobs_;
        mutable std::uint64_t projected_files_ = 0;
        mutable std::map<std::string, cached_file> cache_;
        mutable std::deque<std::pair<std::vector<std::string>, std::shared_ptr<const source_state>>> projected_;

        auto event_at(std::size_t index) const -> event& {
            if (auto found = events_.find(index); found != events_.end())
                return found->second;
            if (!reader_ || index >= event_count_)
                fail(nodes::code::missing_objects, "required causal event is absent");
            auto h = reader_->header(index);
            if (h.at("index").number() != index)
                fail(nodes::code::invalid_record, "causal event location differs");
            event e;
            e.id = h.at("id").text();
            e.author = h.at("author").text();
            e.operation = h.at("operation").text();
            e.clock = h.at("clock").number();
            e.request_digest = h.at("request_digest").text();
            e.effect_digest = h.at("effect_digest").text();
            e.dependencies = string_list(h.at("dependencies"));
            e.retained_effects = h.at("effects").number();
            e.complete = false;
            std::optional<std::uint64_t> previous_end;
            for (auto& r : h.at("ancestors").list()) {
                auto first = r.at("first").number(), last = r.at("last").number();
                if (first >= last || last > index || (previous_end && first <= *previous_end))
                    fail(nodes::code::invalid_record, "causal proof intervals differ");
                e.ancestors.add_range(first, last);
                previous_end = last;
            }
            if (!content::is_hash(e.id) || !content::is_hash(e.author) || !content::is_hash(e.operation) || !e.clock)
                fail(nodes::code::invalid_record, "causal event metadata differs");
            auto [found, inserted] = events_.emplace(index, std::move(e));
            if (!index_.emplace(found->second.id, index).second)
                fail(nodes::code::invalid_record, "duplicate indexed causal identity");
            auto& retained = found->second;
            for (auto& dep : retained.dependencies)
                dependency_events_[dep].push_back(index);
            return retained;
        }
        auto locate(const std::string& id) const -> std::map<std::string, std::size_t>::iterator {
            if (auto found = index_.find(id); found != index_.end())
                return found;
            if (reader_)
                if (auto index = reader_->locate(id)) {
                    if (event_at(*index).id != id)
                        fail(nodes::code::invalid_record, "causal identity index differs");
                }
            return index_.find(id);
        }
        void register_effect(const effect* e) const {
            auto& aliases = effects_[e->id];
            if (std::ranges::find(aliases, e) != aliases.end())
                return;
            aliases.push_back(e);
            if (e->type == "activate" || e->type == "resolve")
                control_effects_.push_back(e);
            else
                file_effects_[e->file].push_back(e);
            if (e->type == "activate")
                for (auto& target : e->selections)
                    activation_writes_[target].push_back(e);
            if (e->type == "resolve")
                for (auto& target : e->selections)
                    resolution_writes_[target].push_back(e);
            if (e->type == "presence")
                presence_[e->file].push_back(e);
            if (e->type == "path") {
                path_files_[e->property.text()].insert(e->file);
                auto& b = event_at(e->event);
                if (e->file.starts_with(b.author + ":" + b.operation + ":file:"))
                    file_creators_[e->file].push_back(e->event);
            }
        }
        void load_record(const value& record) const {
            auto index = record.at("event").number(), ordinal = record.at("ordinal").number();
            auto& e = event_at(index);
            if (!e.complete && ordinal >= e.retained_effects)
                fail(nodes::code::invalid_record, "causal effect ordinal differs");
            if (e.complete || e.loaded_ordinals.contains(ordinal))
                return;
            auto& input = record.at("effect");
            effect x{.id = operation_effect(e.author, e.operation, ordinal), .type = input.at("type").text(), .event = index, .ordinal = ordinal};
            if (x.type == "activate" || x.type == "resolve") {
                x.selections = string_list(input.at("selections"));
                x.property = input.at("value");
            } else {
                x.file = input.at("file").text();
                if (x.type == "insert") {
                    x.left = element::decode(input.at("left"));
                    x.right = element::decode(input.at("right"));
                    x.text = input.at("text").text();
                    for (unsigned char c : std::string_view{x.text})
                        if (c >= 128) {
                            x.scalar_text = scalars(x.text);
                            break;
                        }
                } else if (x.type == "erase") {
                    for (auto& target : input.at("targets").list()) {
                        auto run = target.at("run").text();
                        // The map owns anchor names even when their insertion is not loaded yet.
                        auto [found, _] = effects_.try_emplace(run);
                        x.targets.push_back({&found->first, target.at("first").number(), target.at("count").number()});
                    }
                } else {
                    x.property = input.at("value");
                    if (x.type == "binary") {
                        auto blob = record.at("binary").raw();
                        if (digest_of(blob) != x.property.text())
                            fail(nodes::code::storage_failure, "indexed binary content differs");
                        blobs_[x.property.text()] = std::move(blob);
                    }
                }
            }
            e.effects.push_back(std::move(x));
            e.loaded_ordinals.insert(ordinal);
            register_effect(&e.effects.back());
        }
        void load_records(const std::string& kind, const std::string& key) const {
            if (reader_)
                for (auto& record : reader_->records(kind, key))
                    load_record(record);
        }
        void load_file(const std::string& file) const {
            if (!reader_ || loaded_files_.contains(file))
                return;
            bool newly_created = file_creators_.contains(file) && !file_creators_.at(file).empty() &&
                                 std::ranges::all_of(file_creators_.at(file), [&](auto index) { return index >= retained_count_; });
            if (!newly_created)
                load_records("file", file);
            if (auto found = file_effects_.find(file); found != file_effects_.end())
                std::ranges::sort(found->second, [](auto* a, auto* b) { return std::tie(a->event, a->ordinal) < std::tie(b->event, b->ordinal); });
            loaded_files_.insert(file);
        }
        void load_presence(const std::string& file) const { load_records("presence", file); }
        // A record is loaded by its address; the cache names the addresses already read. `effect` and `control` share the
        // same `<effect id>/<event>/<ordinal>` key space, so a key is marked for the kind it was read as, not for both.
        void load_effect(const std::string& id) const {
            if (reader_ && !loaded_effects_.contains("effect/" + id)) {
                load_records("effect", id);
                loaded_effects_.insert("effect/" + id);
            }
        }
        void load_controls(const std::string& id) const {
            if (reader_ && !loaded_controls_.contains("control/" + id)) {
                load_records("control", id);
                loaded_controls_.insert("control/" + id);
            }
        }
        void load_dependents(const std::string& id) const {
            if (reader_ && !loaded_dependencies_.contains(id)) {
                for (auto& v : reader_->records("dependent", id))
                    (void)event_at(v.at("event").number());
                loaded_dependencies_.insert(id);
            }
        }
        void load_event(std::size_t index) const {
            auto& e = event_at(index);
            if (!e.complete) {
                load_records("event", std::format("{:016x}", index));
                if (e.effects.size() != e.retained_effects)
                    fail(nodes::code::missing_objects, "required causal effects are absent");
                std::set<std::string> created;
                for (auto& x : e.effects)
                    if (x.type == "path" && x.file.starts_with(e.author + ":" + e.operation + ":file:"))
                        created.insert(x.file);
                e.created = {created.begin(), created.end()};
                e.complete = true;
            }
        }
        void register_event(std::size_t index) {
            auto& event = event_at(index);
            index_.emplace(event.id, index);
            for (auto& dependency : event.dependencies)
                dependency_events_[dependency].push_back(index);
            for (auto& file : event.created)
                file_creators_[file].push_back(index);
            for (auto& effect : event.effects) {
                if (effect.type == "activate" || effect.type == "resolve")
                    control_effects_.push_back(&effect);
                else
                    file_effects_[effect.file].push_back(&effect);
                if (effect.type == "activate")
                    for (auto& target : effect.selections)
                        activation_writes_[target].push_back(&effect);
                if (effect.type == "resolve")
                    for (auto& target : effect.selections)
                        resolution_writes_[target].push_back(&effect);
                if (effect.type == "presence")
                    presence_[effect.file].push_back(&effect);
                if (effect.type == "path")
                    path_files_[effect.property.text()].insert(effect.file);
            }
        }
        auto before(std::size_t a, std::size_t b) const -> bool { return a != b && event_at(b).ancestors.contains(a); }
        auto before(const effect* a, const effect* b) const -> bool { return before(a->event, b->event); }
        static auto contains(const causal_set& bits, std::size_t n) -> bool { return bits.contains(n); }
        auto closure(const std::vector<std::string>& heads) const -> causal_set {
            causal_set out;
            for (auto& h : heads) {
                auto i = locate(h);
                if (i == index_.end()) {
                    error e{nodes::code::missing_objects, "unknown source event"};
                    e.missing.push_back(h);
                    throw e;
                }
                out.unite(event_at(i->second).ancestors);
                out.add(i->second);
            }
            return out;
        }
        auto rank(const effect* e) const {
            auto& b = event_at(e->event);
            return std::tuple{b.clock, std::string_view{b.author}, std::string_view{b.operation}, e->ordinal};
        }
        auto conflict(std::string type, std::vector<std::string> ids) const -> std::string {
            ids = canonical_set(std::move(ids));
            return type + ":" + digest_of(codec::encode(value::map{{"effects", strings(ids)}, {"type", type}}));
        }
        auto maximal(const std::vector<const effect*>& candidates) const -> std::vector<const effect*> {
            std::vector<const effect*> out;
            std::set<std::string> seen;
            for (auto* a : candidates)
                if (seen.insert(a->id).second &&
                    std::ranges::none_of(candidates, [&](auto* b) { return before(a, b) || (a->event == b->event && a->ordinal < b->ordinal); }))
                    out.push_back(a);
            return out;
        }
        auto valid_element(const std::string& run, std::uint64_t offset, const event& e, std::size_t ordinal, std::string_view file, bool sentinel = true) const
            -> const std::string* {
            if (run.empty()) {
                if (!sentinel || offset)
                    fail(nodes::code::invalid_record, "invalid text sentinel");
                return nullptr;
            }
            load_effect(run);
            auto found = effects_.find(run);
            if (found == effects_.end())
                fail(nodes::code::invalid_record, "unknown text element");
            const effect* source = nullptr;
            for (auto* alias : found->second)
                if (contains(e.ancestors, alias->event) || (alias->event == event_count_ - 1 && alias->ordinal < ordinal)) {
                    source = alias;
                    break;
                }
            if (!source)
                fail(nodes::code::invalid_record, "text reference outside parent closure");
            if (source->type != "insert" || source->file != file || offset >= source->scalar_size())
                fail(nodes::code::invalid_record, "invalid text element identity");
            if (source->event == event_count_ - 1) {
                if (source->ordinal >= ordinal)
                    fail(nodes::code::invalid_record, "forward effect reference");
            } else if (!contains(e.ancestors, source->event))
                fail(nodes::code::invalid_record, "text reference outside parent closure");
            return &found->first;
        }
        auto valid_element(const element& x, const event& e, std::size_t ordinal, std::string_view file, bool sentinel = true) const -> const std::string* {
            return valid_element(x.run, x.offset, e, ordinal, file, sentinel);
        }
        auto extend_text(const cached_file& previous, cached_file& next) const -> bool {
            auto count = previous.effects.size();
            if (!count || count >= next.effects.size() || !previous.conflicts.empty() || previous.file->kind != "text")
                return false;
            for (std::size_t i = 0; i < count; ++i)
                if (previous.effects[i] != next.effects[i] || previous.activation[i] != next.activation[i])
                    return false;
            auto event = next.effects[count]->event;
            for (std::size_t i = count; i < next.effects.size(); ++i) {
                auto* change = next.effects[i];
                if (change->event != event || !next.activation[i] || (change->type != "insert" && change->type != "erase") || !change->scalar_text.empty())
                    return false;
            }
            for (auto* old : previous.effects)
                if (old->event != event && !event_at(event).ancestors.contains(old->event))
                    return false;
            if (!previous.file->fragments_ready || previous.file->text.size() != previous.file->size())
                return false;
            next.file = std::make_shared<source_file>(*previous.file);
            auto& file = *next.file;
            auto position = [&](const element& selected) -> std::optional<std::uint64_t> {
                std::uint64_t offset = 0;
                for (auto& fragment : file.fragments) {
                    if (fragment.run == selected.run && selected.offset >= fragment.first && selected.offset - fragment.first < fragment.count)
                        return offset + selected.offset - fragment.first;
                    offset += fragment.count;
                }
                return {};
            };
            for (std::size_t i = count; i < next.effects.size(); ++i) {
                auto& change = *next.effects[i];
                if (change.type == "erase") {
                    std::map<std::string_view, spans> targets;
                    std::uint64_t required = 0, removed = 0, offset = 0;
                    for (auto& target : change.targets) {
                        targets[*target.run].emplace_back(target.first, target.first + target.count);
                        required += target.count;
                    }
                    for (auto& [_, ranges] : targets)
                        merge(ranges);
                    std::string text;
                    text.reserve(file.text.size());
                    std::vector<fragment> fragments;
                    auto append = [&](const fragment& original, std::uint64_t first, std::uint64_t length) {
                        if (!length)
                            return;
                        text.append(file.text, offset + first - original.first, length);
                        fragments.push_back({original.run, first, length});
                    };
                    for (auto& original : file.fragments) {
                        auto first = original.first, last = first + original.count;
                        if (auto found = targets.find(original.run); found != targets.end())
                            for (auto range = after(found->second, first); range != found->second.end() && range->first < last; ++range) {
                                auto from = std::max(range->first, first), to = std::min(range->second, last);
                                append(original, first, from - first);
                                removed += to - from;
                                first = to;
                            }
                        append(original, first, last - first);
                        offset += original.count;
                    }
                    if (removed != required)
                        return false;
                    file.text = std::move(text);
                    file.fragments = std::move(fragments);
                } else {
                    auto left = change.left.run.empty() ? std::optional<std::uint64_t>{0} : position(change.left);
                    if (left && !change.left.run.empty())
                        ++*left;
                    auto right = change.right.run.empty() ? std::optional<std::uint64_t>{file.text.size()} : position(change.right);
                    if (!left || !right || *left != *right)
                        return false;
                    std::vector<fragment> fragments;
                    std::uint64_t offset = 0;
                    bool inserted = false;
                    for (auto& original : file.fragments) {
                        if (!inserted && *left >= offset && *left <= offset + original.count) {
                            auto prefix = *left - offset;
                            if (prefix)
                                fragments.push_back({original.run, original.first, prefix});
                            fragments.push_back({change.id, 0, change.scalar_size()});
                            if (prefix < original.count)
                                fragments.push_back({original.run, original.first + prefix, original.count - prefix});
                            inserted = true;
                        } else
                            fragments.push_back(original);
                        offset += original.count;
                    }
                    if (!inserted)
                        fragments.push_back({change.id, 0, change.scalar_size()});
                    file.text.insert(*left, change.text);
                    file.fragments = std::move(fragments);
                }
            }
            file.digest = digest_of(file.text);
            return true;
        }
        auto render(const std::string& file_id, const std::vector<const effect*>& list, const std::map<std::string, bool>& active) const -> const cached_file& {
            auto is_active = [&](const effect* e) {
                auto i = active.find(e->id);
                return i == active.end() || i->second;
            };
            if (auto it = cache_.find(file_id); it != cache_.end() && it->second.effects == list) {
                bool equal = true;
                for (std::size_t i = 0; i < list.size(); ++i)
                    if (it->second.activation[i] != is_active(list[i])) {
                        equal = false;
                        break;
                    }
                if (equal)
                    return it->second;
            }
            cached_file out;
            out.effects = list;
            out.activation.reserve(list.size());
            for (auto* e : list)
                out.activation.push_back(is_active(e));
            if (auto previous = cache_.find(file_id); previous != cache_.end() && extend_text(previous->second, out)) {
                previous->second = std::move(out);
                return previous->second;
            }
            out.file = std::make_shared<source_file>();
            auto& f = *out.file;
            f.id = file_id;
            std::map<std::string, std::vector<const effect*>> registers;
            std::vector<const effect*> inserts, text_effects;
            inserts.reserve(list.size());
            text_effects.reserve(list.size());
            std::map<const std::string*, spans, run_less> deleted;
            std::unordered_set<std::string_view> seen_effects;
            seen_effects.reserve(list.size());
            for (auto* e : list) {
                if (!seen_effects.insert(e->id).second)
                    continue;
                if (e->type == "insert") {
                    inserts.push_back(e);
                    if (is_active(e))
                        text_effects.push_back(e);
                } else if (e->type == "erase") {
                    if (is_active(e)) {
                        text_effects.push_back(e);
                        for (auto& target : e->targets)
                            deleted[target.run].emplace_back(target.first, target.first + target.count);
                    }
                } else if (is_active(e))
                    registers[e->type].push_back(e);
            }
            for (auto& [_, ranges] : deleted)
                merge(ranges);
            for (auto& [type, values] : registers) {
                auto live = maximal(values);
                if (live.empty())
                    continue;
                auto selected = live.front()->property;
                if (std::ranges::any_of(live, [&](auto* e) { return e->property != selected; })) {
                    std::vector<std::string> ids;
                    for (auto* e : live)
                        ids.push_back(e->id);
                    out.conflicts.push_back(conflict(type, std::move(ids)));
                    std::ranges::sort(live, [&](auto* a, auto* b) { return rank(a) < rank(b); });
                    selected = live.front()->property;
                }
                if (type == "path")
                    f.path = selected.text();
                if (type == "presence")
                    f.present = selected.boolean();
                if (type == "mode")
                    f.executable = selected.boolean();
                if (type == "binary") {
                    f.kind = std::string{"binary"};
                    auto found = blobs_.find(selected.text());
                    if (found == blobs_.end())
                        fail(nodes::code::missing_objects, "binary content is unavailable");
                    f.text = found->second;
                }
            }
            if (f.path.empty())
                f.present = false;
            bool serial = std::adjacent_find(text_effects.begin(), text_effects.end(),
                                             [&](auto* a, auto* b) { return a->event != b->event && !before(a->event, b->event); }) == text_effects.end();
            if (!serial)
                for (auto* a : text_effects)
                    for (auto* b : text_effects) {
                        if (a->id >= b->id || a->event == b->event || before(a, b) || before(b, a))
                            continue;
                        bool overlap = false;
                        if (a->type == "insert" && b->type == "insert")
                            overlap = a->left == b->left && a->right == b->right;
                        if (a->type == "erase" && b->type == "erase") {
                            std::map<std::string_view, spans> erased;
                            for (auto& x : a->targets)
                                erased[*x.run].emplace_back(x.first, x.first + x.count);
                            for (auto& [_, ranges] : erased)
                                merge(ranges);
                            overlap = std::ranges::any_of(b->targets, [&](auto& x) {
                                auto found = erased.find(*x.run);
                                if (found == erased.end())
                                    return false;
                                auto range = after(found->second, x.first);
                                return range != found->second.end() && range->first < x.first + x.count;
                            });
                        }
                        auto crossing = [](const effect* ins, const effect* del) {
                            if (ins->type != "insert" || del->type != "erase")
                                return false;
                            return std::ranges::any_of(del->targets, [&](auto& target) { return target.covers(ins->left) || target.covers(ins->right); });
                        };
                        overlap = overlap || crossing(a, b) || crossing(b, a);
                        if (overlap)
                            out.conflicts.push_back(conflict("text_overlap", {a->id, b->id}));
                    }
            for (auto* a : list)
                if (a->type == "presence" && !a->property.boolean() && is_active(a))
                    for (auto* b : list) {
                        if (a->event != b->event && a->id != b->id && is_active(b) && !before(a, b) && !before(b, a))
                            out.conflicts.push_back(conflict("delete_change", {a->id, b->id}));
                    }
            if (f.kind == "text") {
                using branches = std::map<std::uint64_t, std::vector<const effect*>>;
                std::map<std::string_view, branches> children;
                std::vector<const effect*> roots;
                for (auto* e : inserts)
                    if (e->left.run.empty())
                        roots.push_back(e);
                    else
                        children[e->left.run][e->left.offset].push_back(e);
                auto sort_runs = [&](auto& runs) { std::ranges::sort(runs, [&](auto* a, auto* b) { return rank(a) > rank(b); }); };
                sort_runs(roots);
                for (auto& [_, anchors] : children)
                    for (auto& [offset, runs] : anchors)
                        sort_runs(runs);
                struct frame {
                    const effect* run;
                    std::uint64_t offset;
                    const branches* children;
                    const spans* deleted;
                    bool active;
                };
                std::vector<frame> work;
                auto push = [&](const std::vector<const effect*>& runs) {
                    for (auto i = runs.rbegin(); i != runs.rend(); ++i) {
                        auto child = children.find((*i)->id);
                        auto removed = deleted.find(&(*i)->id);
                        work.push_back(
                            {*i, 0, child == children.end() ? nullptr : &child->second, removed == deleted.end() ? nullptr : &removed->second, is_active(*i)});
                    }
                };
                push(roots);
                const effect* last_emitted = nullptr;
                while (!work.empty()) {
                    auto& top = work.back();
                    auto* e = top.run;
                    auto offset = top.offset++;
                    if (offset == e->scalar_size()) {
                        work.pop_back();
                        continue;
                    }
                    if (!top.children && !top.active) {
                        work.pop_back();
                        continue;
                    }
                    if (!top.children && top.deleted && top.deleted->size() == 1 && top.deleted->front() == spans::value_type{0, e->scalar_size()}) {
                        work.pop_back();
                        continue;
                    }
                    if (!top.children && e->text.size() == e->scalar_size() && (!top.deleted || top.deleted->back().second <= offset)) {
                        auto count = e->text.size() - offset;
                        f.text.append(e->text, offset, count);
                        if (last_emitted == e && f.fragments.back().first + f.fragments.back().count == offset)
                            f.fragments.back().count += count;
                        else
                            f.fragments.push_back({e->id, offset, count});
                        last_emitted = e;
                        work.pop_back();
                        continue;
                    }
                    auto* branches = top.children;
                    bool visible = top.active && (!top.deleted || !covered(*top.deleted, offset));
                    if (visible) {
                        append_scalar(f.text, e->scalar_at(offset));
                        if (last_emitted == e && f.fragments.back().first + f.fragments.back().count == offset)
                            ++f.fragments.back().count;
                        else
                            f.fragments.push_back({e->id, offset, 1});
                        last_emitted = e;
                    }
                    if (branches)
                        if (auto found = branches->find(offset); found != branches->end())
                            push(found->second);
                }
            }
            out.conflicts = canonical_set(std::move(out.conflicts));
            f.digest = digest_of(f.text);
            auto& cached = cache_[file_id];
            cached = std::move(out);
            return cached;
        }

    public:
        kernel() = default;
        kernel(const kernel&) = delete;
        auto operator=(const kernel&) -> kernel& = delete;
        kernel(kernel&&) = default;
        auto operator=(kernel&&) -> kernel& = default;
        auto has_reader() const -> bool { return reader_ != nullptr; }
        auto fully_loaded() const -> bool {
            if (events_.size() != event_count_)
                return false;
            for (auto& [_, event] : events_) {
                if (!event.complete)
                    return false;
                (void)event;
            }
            return true;
        }
        void attach_reader(causal_reader* reader) {
            reader_ = reader;
            retained_count_ = event_count_;
        }
        void demand(std::size_t count, causal_reader* reader) {
            if (event_count_ || !events_.empty())
                fail(nodes::code::invalid_record, "demand kernel must start empty");
            event_count_ = count;
            retained_count_ = count;
            reader_ = reader;
        }
        auto loaded_events() const -> std::size_t { return events_.size(); }
        auto loaded_files() const -> std::size_t { return loaded_files_.size() + cache_.size(); }
        auto event_header(std::size_t index) const -> value {
            auto& e = event_at(index);
            value::array ranges;
            for (auto [first, last] : e.ancestors.ranges())
                ranges.emplace_back(value::map{{"first", static_cast<std::uint64_t>(first)}, {"last", static_cast<std::uint64_t>(last)}});
            return value::map{{"index", static_cast<std::uint64_t>(index)},
                              {"id", e.id},
                              {"author", e.author},
                              {"operation", e.operation},
                              {"clock", e.clock},
                              {"ancestors", std::move(ranges)},
                              {"effects", static_cast<std::uint64_t>(e.complete ? e.effects.size() : e.retained_effects)},
                              {"dependencies", strings(e.dependencies)},
                              {"request_digest", e.request_digest},
                              {"effect_digest", e.effect_digest}};
        }
        auto event_records(std::size_t index) const -> std::vector<value> {
            load_event(index);
            auto& e = event_at(index);
            std::vector<value> out;
            for (auto& x : e.effects) {
                value::map input{{"type", x.type}};
                if (x.type == "activate" || x.type == "resolve") {
                    input["selections"] = strings(x.selections);
                    input["value"] = x.property;
                } else {
                    input["file"] = x.file;
                    if (x.type == "insert") {
                        input["left"] = x.left.encoded();
                        input["right"] = x.right.encoded();
                        input["text"] = x.text;
                    } else if (x.type == "erase") {
                        value::array targets;
                        for (auto& t : x.targets)
                            targets.push_back(t.encoded());
                        input["targets"] = std::move(targets);
                    } else
                        input["value"] = x.property;
                }
                out.emplace_back(value::map{{"event", static_cast<std::uint64_t>(index)},
                                            {"ordinal", static_cast<std::uint64_t>(x.ordinal)},
                                            {"effect", std::move(input)},
                                            {"binary", bytes{x.type == "binary" ? blobs_.at(x.property.text()) : std::string{}}}});
            }
            return out;
        }
        void add_blob(const std::string& digest, std::string bytes) {
            if (digest_of(bytes) != digest)
                fail(nodes::code::invalid_record, "binary content digest mismatch");
            blobs_.try_emplace(digest, std::move(bytes));
        }
        auto has(std::string_view id) const -> bool { return locate(std::string(id)) != index_.end(); }
        auto size() const -> std::size_t { return event_count_; }
        auto clock(const std::vector<std::string>& heads) const -> std::uint64_t {
            std::uint64_t n = 0;
            for (auto& h : heads) {
                auto i = locate(h);
                if (i == index_.end())
                    fail(nodes::code::missing_objects, "missing parent event");
                n = std::max(n, event_at(i->second).clock);
            }
            if (n == std::numeric_limits<std::uint64_t>::max())
                fail(nodes::code::resource_limit, "Lamport overflow");
            return n + 1;
        }
        auto frontier(std::vector<std::string> heads) const -> std::vector<std::string> {
            std::ranges::sort(heads);
            heads.erase(std::unique(heads.begin(), heads.end()), heads.end());
            auto all = closure(heads);
            (void)all;
            std::vector<std::string> out;
            for (auto& h : heads)
                if (std::ranges::none_of(heads, [&](auto& other) { return before(index_.at(h), index_.at(other)); }))
                    out.push_back(h);
            return out;
        }
        auto reachable(const std::vector<std::string>& heads) const -> std::vector<std::string> {
            auto bits = closure(heads);
            std::vector<std::string> out;
            bits.each([&](auto i) { out.push_back(event_at(i).id); });
            return out;
        }
        auto includes(const std::vector<std::string>& heads, const std::string& event) const -> bool {
            auto found = locate(event);
            return found != index_.end() && contains(closure(heads), found->second);
        }
        auto effect_ids(std::string_view event_id) const -> std::vector<std::string> {
            auto i = locate(std::string(event_id));
            if (i == index_.end()) {
                error e{nodes::code::missing_objects, "unknown reversal batch"};
                e.missing.push_back(std::string(event_id));
                throw e;
            }
            std::vector<std::string> out;
            load_event(i->second);
            for (auto& e : event_at(i->second).effects)
                if (e.type != "activate" && e.type != "resolve")
                    out.push_back(e.id);
            return canonical_set(std::move(out));
        }
        auto dependencies(const std::vector<std::string>& heads, const value::array& changes, std::vector<std::string> explicit_reads) const
            -> std::vector<std::string> {
            auto bits = closure(heads);
            std::set<std::string> observed_files;
            for (auto& change : changes) {
                auto type = change.at("type").text();
                if (type == "activate" || type == "resolve")
                    continue;
                observed_files.insert(change.at("file").text());
                if (type == "erase")
                    for (auto& target : change.at("targets").list())
                        explicit_reads.push_back(target.at("run").text());
            }
            for (auto& file : observed_files) {
                load_presence(file);
                if (auto found = presence_.find(file); found != presence_.end())
                    for (auto* e : found->second)
                        if (e->property.boolean() && contains(bits, e->event))
                            explicit_reads.push_back(e->id);
            }
            return canonical_set(std::move(explicit_reads));
        }
        auto compensation_dependencies(const std::vector<std::string>& heads, const std::vector<std::string>& selected) const -> std::vector<std::string> {
            auto bits = closure(heads);
            std::set<std::string> reversed(selected.begin(), selected.end()), out;
            for (auto& target : selected) {
                load_effect(target);
                load_dependents(target);
                auto found = effects_.find(target);
                if (found == effects_.end() || std::ranges::none_of(found->second, [&](auto* alias) { return contains(bits, alias->event); }))
                    fail(nodes::code::invalid_record, "compensation target outside selected source");
                if (auto dependent = dependency_events_.find(target); dependent != dependency_events_.end())
                    for (auto i : dependent->second)
                        if (contains(bits, i)) {
                            load_event(i);
                            bool remains = std::ranges::any_of(event_at(i).effects, [&](auto& e) {
                                return e.type != "activate" && e.type != "resolve" && !reversed.contains(e.id) && active(heads, e.id);
                            });
                            if (remains)
                                out.insert(conflict("dependency", {target, event_at(i).author + ":" + event_at(i).operation}));
                        }
            }
            return {out.begin(), out.end()};
        }
        auto operation_effect(std::string_view author, std::string_view operation, std::size_t ordinal) const -> std::string {
            return std::string(author) + ":" + std::string(operation) + ":" + std::to_string(ordinal);
        }
        void add(const std::string& id, const value& body) {
            if (has(id))
                return;
            auto heads = string_list(body.at("parents"));
            if (frontier(heads) != heads)
                fail(nodes::code::invalid_record, "parents are not a canonical frontier");
            auto clock_value = clock(heads);
            if (body.at("lamport").number() != clock_value)
                fail(nodes::code::invalid_record, "invalid Lamport value");
            event next{
                .id = id, .author = body.at("author").text(), .operation = body.at("operation").text(), .clock = clock_value, .ancestors = closure(heads)};
            next.request_digest = body.at("request_digest").text();
            next.effect_digest = body.at("effect_digest").text();
            if (!content::is_hash(next.author) || !content::is_hash(next.operation))
                fail(nodes::code::invalid_record, "invalid operation identity");
            next.dependencies = string_list(body.at("dependencies"));
            if (next.dependencies != canonical_set(next.dependencies))
                fail(nodes::code::invalid_record, "noncanonical dependency set");
            for (auto& dependency : next.dependencies) {
                load_effect(dependency);
                auto it = effects_.find(dependency);
                if (it == effects_.end() || std::ranges::none_of(it->second, [&](auto* alias) { return contains(next.ancestors, alias->event); }))
                    fail(nodes::code::invalid_record, "dependency outside observed history");
            }
            events_.emplace(event_count_++, std::move(next));
            auto& e = events_.at(event_count_ - 1);
            try {
                std::set<std::string> observed_files;
                std::set<std::string> new_files;
                std::size_t created = 0;
                std::size_t ordinal = 0;
                for (auto& input : body.at("effects").list()) {
                    effect x{
                        .id = operation_effect(e.author, e.operation, ordinal), .type = input.at("type").text(), .event = event_count_ - 1, .ordinal = ordinal};
                    if (x.type == "activate" || x.type == "resolve") {
                        fields(input, {"selections", "type", "value"});
                        x.selections = string_list(input.at("selections"));
                        x.property = input.at("value");
                        if (x.selections != canonical_set(x.selections))
                            fail(nodes::code::invalid_record, "noncanonical effect selections");
                        if (x.selections.empty())
                            fail(nodes::code::invalid_record, "empty compensation or resolution");
                        if (x.type == "resolve") {
                            if (!x.property.boolean())
                                fail(nodes::code::invalid_record, "resolution must be affirmative");
                            auto observed = project(heads);
                            std::vector<std::string> reversed;
                            for (auto& change : body.at("effects").list())
                                if (change.at("type").text() == "activate" && !change.at("value").boolean()) {
                                    auto selected = string_list(change.at("selections"));
                                    reversed.insert(reversed.end(), selected.begin(), selected.end());
                                }
                            auto forecast = compensation_dependencies(heads, reversed);
                            observed.conflicts.insert(observed.conflicts.end(), forecast.begin(), forecast.end());
                            for (auto& selected : x.selections)
                                if (std::ranges::find(observed.conflicts, selected) == observed.conflicts.end())
                                    fail(nodes::code::source_conflict, "resolution does not name an observed conflict");
                        }
                        if (x.type == "activate") {
                            (void)x.property.boolean();
                            for (auto& target : x.selections) {
                                load_effect(target);
                                auto it = effects_.find(target);
                                if (it == effects_.end() || std::ranges::none_of(it->second, [&](auto* alias) {
                                        return contains(e.ancestors, alias->event) && alias->type != "activate" && alias->type != "resolve";
                                    }))
                                    fail(nodes::code::invalid_record, "invalid compensation target");
                            }
                        }
                    } else {
                        x.file = input.at("file").text();
                        if (x.file.empty())
                            fail(nodes::code::invalid_record, "missing file identity");
                        load_presence(x.file);
                        if (auto creator = file_creators_.find(x.file);
                            creator != file_creators_.end() && std::ranges::any_of(creator->second, [&](auto index) { return contains(e.ancestors, index); }))
                            observed_files.insert(x.file);
                        if (!observed_files.contains(x.file)) {
                            auto expected = e.author + ":" + e.operation + ":file:" + std::to_string(created);
                            if (x.type != "path" || x.file != expected)
                                fail(nodes::code::invalid_record, "file identity is outside observed history");
                            observed_files.insert(x.file);
                            new_files.insert(x.file);
                            ++created;
                        }
                        if (x.type == "insert") {
                            fields(input, {"file", "left", "right", "text", "type"});
                            x.left = element::decode(input.at("left"));
                            x.right = element::decode(input.at("right"));
                            x.text = input.at("text").text();
                            if (x.text.empty())
                                fail(nodes::code::invalid_record, "empty insertion");
                            for (unsigned char c : std::string_view{x.text})
                                if (c >= 128) {
                                    x.scalar_text = scalars(x.text);
                                    break;
                                }
                            valid_element(x.left, e, ordinal, x.file);
                            valid_element(x.right, e, ordinal, x.file);
                        } else if (x.type == "erase") {
                            fields(input, {"file", "targets", "type"});
                            x.targets.reserve(input.at("targets").list().size());
                            for (auto& selected : input.at("targets").list()) {
                                fields(selected, {"count", "first", "run"});
                                auto first = selected.at("first").number(), count = selected.at("count").number();
                                if (!count || count - 1 > std::numeric_limits<std::uint64_t>::max() - first)
                                    fail(nodes::code::invalid_record, "invalid deletion range");
                                auto* run = valid_element(selected.at("run").text(), first + count - 1, e, ordinal, x.file, false);
                                x.targets.push_back({run, first, count});
                            }
                            if (x.targets.empty())
                                fail(nodes::code::invalid_record, "empty deletion");
                            auto ordered = x.targets;
                            std::sort(ordered.begin(), ordered.end());
                            for (std::size_t i = 1; i < ordered.size(); ++i)
                                if (*ordered[i - 1].run == *ordered[i].run && ordered[i - 1].first + ordered[i - 1].count > ordered[i].first)
                                    fail(nodes::code::invalid_record, "duplicate deletion target");
                        } else {
                            fields(input, {"file", "type", "value"});
                            x.property = input.at("value");
                            if (x.type == "path")
                                checked_path(x.property.text());
                            else if (x.type == "presence" || x.type == "mode")
                                (void)x.property.boolean();
                            else if (x.type == "binary") {
                                if (!content::is_hash(x.property.text()) || !blobs_.contains(x.property.text()))
                                    fail(nodes::code::missing_objects, "binary effect requires retained immutable content");
                            } else
                                fail(nodes::code::invalid_record, "unknown source effect");
                        }
                    }
                    e.effects.push_back(std::move(x));
                    effects_[e.effects.back().id].push_back(&e.effects.back());
                    ++ordinal;
                }
                e.created = {new_files.begin(), new_files.end()};
                register_event(event_count_ - 1);
            } catch (...) {
                for (auto& x : e.effects) {
                    auto found = effects_.find(x.id);
                    if (found != effects_.end()) {
                        std::erase(found->second, &x);
                        if (found->second.empty())
                            effects_.erase(found);
                    }
                }
                events_.erase(--event_count_);
                throw;
            }
        }

    private:
        static auto changed(const causal_set& from, const causal_set& to) -> causal_set {
            auto out = from.without(to);
            out.unite(to.without(from));
            return out;
        }
        auto affected_files(const causal_set& delta) const -> std::set<std::string> {
            std::set<std::string> files;
            delta.each([&](auto index) {
                load_event(index);
                for (auto& effect : event_at(index).effects) {
                    if (!effect.file.empty())
                        files.insert(effect.file);
                    auto include = [&](const std::string& id) {
                        load_effect(id);
                        if (auto found = effects_.find(id); found != effects_.end())
                            for (auto* alias : found->second)
                                if (!alias->file.empty())
                                    files.insert(alias->file);
                    };
                    include(effect.id);
                    if (effect.type == "activate")
                        for (auto& target : effect.selections)
                            include(target);
                }
            });
            return files;
        }
        auto compute(const std::vector<std::string>& heads) const -> source_state {
            source_state out;
            if (!projected_.empty())
                out = *projected_.back().second;
            auto base = out;
            auto bits = closure(heads);
            auto delta = changed(out.causal, bits);
            out.causal = bits;
            std::set<std::string> controls, resolutions, dependencies;
            delta.each([&](auto index) {
                load_event(index);
                auto& event = event_at(index);
                dependencies.insert(event.dependencies.begin(), event.dependencies.end());
                for (auto& effect : event.effects)
                    if (effect.type == "activate") {
                        controls.insert(effect.selections.begin(), effect.selections.end());
                        dependencies.insert(effect.selections.begin(), effect.selections.end());
                    } else if (effect.type == "resolve")
                        resolutions.insert(effect.selections.begin(), effect.selections.end());
            });
            auto assign_conflicts = [](auto& index, const std::string& key, std::vector<std::string> values) {
                if (values.empty())
                    index.erase(key);
                else
                    index[key] = canonical_set(std::move(values));
            };
            for (auto& target : controls) {
                load_controls(target);
                std::vector<const effect*> writes;
                for (auto* write : activation_writes_.at(target))
                    if (contains(bits, write->event))
                        writes.push_back(write);
                if (writes.empty()) {
                    out.activation.erase(target);
                    out.control_conflicts.erase(target);
                    continue;
                }
                auto live = maximal(writes);
                bool state = live.front()->property.boolean();
                std::vector<std::string> conflicts;
                if (std::ranges::any_of(live, [&](auto* e) { return e->property.boolean() != state; })) {
                    std::vector<std::string> ids{target};
                    for (auto* e : live)
                        ids.push_back(e->id);
                    conflicts.push_back(conflict("activation", std::move(ids)));
                    state = false;
                }
                out.activation[target] = state;
                assign_conflicts(out.control_conflicts, target, std::move(conflicts));
            }
            for (auto& target : resolutions) {
                load_controls(target);
                if (std::ranges::any_of(resolution_writes_.at(target), [&](auto* write) { return contains(bits, write->event); }))
                    out.resolutions[target] = true;
                else
                    out.resolutions.erase(target);
            }
            auto active = [&](const effect& e) {
                auto found = out.activation.find(e.id);
                return found == out.activation.end() || found->second;
            };
            for (auto& target : controls)
                if (auto found = effects_.find(target); found != effects_.end())
                    for (auto* effect : found->second)
                        dependencies.insert(event_at(effect->event).dependencies.begin(), event_at(effect->event).dependencies.end());
            for (auto& target : dependencies) {
                std::vector<std::string> conflicts;
                auto enabled = out.activation.find(target);
                if (enabled != out.activation.end() && !enabled->second) {
                    load_dependents(target);
                    if (auto found = dependency_events_.find(target); found != dependency_events_.end())
                        for (auto index : found->second)
                            if (contains(bits, index)) {
                                load_event(index);
                                if (std::ranges::any_of(event_at(index).effects,
                                                        [&](auto& e) { return e.type != "activate" && e.type != "resolve" && active(e); }))
                                    conflicts.push_back(conflict("dependency", {target, event_at(index).author + ":" + event_at(index).operation}));
                            }
                }
                assign_conflicts(out.dependency_conflicts, target, std::move(conflicts));
            }
            std::set<std::string> changed_paths;
            auto path_owner = [&](const std::string& path, const std::string& id, bool add) {
                std::set<std::string> owners;
                if (auto found = out.paths.find(path); found != out.paths.end())
                    owners = *found->second;
                if (add)
                    owners.insert(id);
                else
                    owners.erase(id);
                if (owners.empty())
                    out.paths.erase(path);
                else
                    out.paths[path] = std::make_shared<const std::set<std::string>>(owners);
                changed_paths.insert(path);
            };
            for (auto& id : affected_files(delta)) {
                ++projected_files_;
                auto old = out.files.find(id);
                auto previous = old == out.files.end() ? std::shared_ptr<const source_file>{} : old->second;
                std::vector<const effect*> list;
                delta.each([&](auto index) {
                    for (auto& effect : event_at(index).effects)
                        if (effect.file == id)
                            list.push_back(&effect);
                });
                bool serial = true;
                for (std::size_t i = 1; i < list.size(); ++i)
                    if (list[i - 1]->event != list[i]->event && !event_at(list[i]->event).ancestors.contains(list[i - 1]->event))
                        serial = false;
                // A metadata-only change keeps the authenticated content of the file it names; anything that can change
                // visible characters or their activation replays just this file's effects.
                bool metadata_only = serial && previous && !previous->fragments_ready && controls.empty() && resolutions.empty() &&
                                     base.causal.without(bits).empty() && !base.file_conflicts.contains(id) &&
                                     std::ranges::all_of(list, [](auto* e) { return e->type == "path" || e->type == "presence" || e->type == "mode"; });
                std::shared_ptr<const source_file> current;
                if (metadata_only) {
                    auto copy = std::make_shared<source_file>(*previous);
                    for (auto* change : list) {
                        if (change->type == "path")
                            copy->path = change->property.text();
                        else if (change->type == "mode")
                            copy->executable = change->property.boolean();
                        else
                            copy->present = change->property.boolean();
                    }
                    if (copy->path.empty())
                        copy->present = false;
                    if (copy->present) {
                        out.hidden.erase(id);
                        current = copy;
                    } else
                        out.hidden[id] = copy;
                } else {
                    list.clear();
                    load_file(id);
                    if (auto found = file_effects_.find(id); found != file_effects_.end())
                        for (auto* effect : found->second)
                            if (contains(bits, effect->event))
                                list.push_back(effect);
                    if (!list.empty()) {
                        auto& file = render(id, list, out.activation);
                        if (file.file->present) {
                            current = file.file;
                            out.hidden.erase(id);
                        } else
                            out.hidden[id] = file.file;
                        assign_conflicts(out.file_conflicts, id, file.conflicts);
                    } else {
                        out.file_conflicts.erase(id);
                        out.hidden.erase(id);
                    }
                }
                if (previous && (!current || previous->path != current->path))
                    path_owner(previous->path, id, false);
                if (current && (!previous || previous->path != current->path))
                    path_owner(current->path, id, true);
                if (current)
                    out.files[id] = current;
                else
                    out.files.erase(id);
            }
            auto affected_paths = changed_paths;
            for (auto& path : changed_paths) {
                auto prefix = path + "/";
                for (auto* paths : {&base.paths, &out.paths})
                    for (auto it = paths->lower_bound(prefix); it != paths->end() && it->first.starts_with(prefix); ++it)
                        affected_paths.insert(it->first);
            }
            for (auto& path : affected_paths) {
                std::vector<std::string> conflicts;
                auto found = out.paths.find(path);
                if (found != out.paths.end()) {
                    std::vector<std::string> ids{found->second->begin(), found->second->end()};
                    if (ids.size() > 1)
                        conflicts.push_back(conflict("path_collision", ids));
                    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
                        if (auto parent = out.paths.find(path.substr(0, slash)); parent != out.paths.end()) {
                            auto all = ids;
                            all.insert(all.end(), parent->second->begin(), parent->second->end());
                            conflicts.push_back(conflict("directory_collision", std::move(all)));
                        }
                }
                assign_conflicts(out.path_conflicts, path, std::move(conflicts));
            }
            out.conflicts.clear();
            for (auto* index : {&out.file_conflicts, &out.path_conflicts, &out.control_conflicts, &out.dependency_conflicts})
                for (auto& [_, conflicts] : *index)
                    for (auto& id : conflicts)
                        if (!out.resolutions.contains(id))
                            out.conflicts.push_back(id);
            out.conflicts = canonical_set(std::move(out.conflicts));
            out.digest = state_digest(out);
            return out;
        }

    public:
        auto projected_files() const -> std::uint64_t { return projected_files_; }
        auto changed_files(const std::vector<std::string>& before, const std::vector<std::string>& after) const -> std::set<std::string> {
            return affected_files(changed(closure(before), closure(after)));
        }
        auto projection(const std::vector<std::string>& heads) const -> std::shared_ptr<const source_state> {
            auto ordered = heads;
            std::ranges::sort(ordered);
            ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
            for (std::size_t i = 0; i < projected_.size(); ++i)
                if (projected_[i].first == ordered) {
                    auto entry = std::move(projected_[i]);
                    auto cached = entry.second;
                    projected_.erase(projected_.begin() + static_cast<std::ptrdiff_t>(i));
                    projected_.push_back(std::move(entry));
                    return cached;
                }
            if (reader_)
                if (auto stored = reader_->projection(ordered)) {
                    auto state = std::make_shared<source_state>(*stored);
                    state->causal = closure(ordered);
                    state->digest = state_digest(*state);
                    projected_.emplace_back(ordered, state);
                    if (projected_.size() > 8)
                        projected_.pop_front();
                    return state;
                }
            auto wanted = closure(ordered);
            if (ordered.empty()) {
                auto empty = std::make_shared<source_state>();
                empty->digest = state_digest(*empty);
                projected_.emplace_back(ordered, empty);
                if (projected_.size() > 8)
                    projected_.pop_front();
                return empty;
            }
            if (projected_.empty() && reader_) {
                auto [frontier, stored] = reader_->base(ordered);
                if (stored) {
                    auto state = std::make_shared<source_state>(*stored);
                    state->causal = closure(frontier);
                    state->digest = state_digest(*state);
                    projected_.emplace_back(std::move(frontier), std::move(state));
                }
            }
            if (projected_.size() > 1) {
                std::size_t best = 0;
                std::uint64_t score = std::numeric_limits<std::uint64_t>::max();
                for (std::size_t i = 0; i < projected_.size(); ++i) {
                    auto difference = changed(projected_[i].second->causal, wanted);
                    const auto size = difference.size();
                    if (size < score) {
                        score = size;
                        best = i;
                    }
                }
                auto entry = std::move(projected_[best]);
                projected_.erase(projected_.begin() + static_cast<std::ptrdiff_t>(best));
                projected_.push_back(std::move(entry));
            }
            auto state = std::make_shared<const source_state>(compute(heads));
            projected_.emplace_back(std::move(ordered), state);
            if (projected_.size() > 8)
                projected_.pop_front();
            return state;
        }
        auto project(const std::vector<std::string>& heads) const -> source_state { return *projection(heads); }
        // Release older reconstruction states while keeping the newest one for the next publication.
        void trim_projections(std::size_t keep = 2) const {
            while (projected_.size() > keep)
                projected_.pop_front();
        }
        // The file as `heads` sees it, with its character runs. A version's manifest names content only; a write
        // that needs anchors replays just this file's effects.
        auto materialize(const std::string& file, const std::vector<std::string>& heads) const -> std::shared_ptr<const source_file> {
            auto state = projection(heads);
            std::shared_ptr<const source_file> found;
            if (auto it = state->files.find(file); it != state->files.end())
                found = it->second;
            else if (auto it = state->hidden.find(file); it != state->hidden.end())
                found = it->second;
            if (!found) {
                error e{nodes::code::invalid_record, "unknown selected file identity"};
                throw e;
            }
            if (found->fragments_ready)
                return found;
            return replay(*found, state->causal, state->activation);
        }
        // A file whose version names only content: render its character runs by replaying just this file's effects.
        auto replay(const source_file& file, const causal_set& bits, const std::map<std::string, bool>& activation) const
            -> std::shared_ptr<const source_file> {
            load_file(file.id);
            std::vector<const effect*> list;
            if (auto it = file_effects_.find(file.id); it != file_effects_.end())
                for (auto* effect : it->second)
                    if (contains(bits, effect->event))
                        list.push_back(effect);
            if (list.empty())
                return std::make_shared<const source_file>(file);
            return render(file.id, list, activation).file;
        }
        auto active(const std::vector<std::string>& heads, const std::string& target) const -> bool {
            auto bits = closure(heads);
            std::vector<const effect*> writes;
            // The effect page and its activation writes name the same `<effect>/<event>/<ordinal>` location under two
            // indexes; read the effect itself before its controls so both kinds are present.
            load_effect(target);
            load_controls(target);
            if (!effects_.contains(target))
                fail(nodes::code::invalid_record, "unknown effect");
            if (auto found = activation_writes_.find(target); found != activation_writes_.end())
                for (auto* write : found->second)
                    if (contains(bits, write->event))
                        writes.push_back(write);
            if (writes.empty())
                return true;
            auto live = maximal(writes);
            bool result = live.front()->property.boolean();
            if (std::ranges::any_of(live, [&](auto* e) { return e->property.boolean() != result; }))
                fail(nodes::code::source_conflict, "activation requires resolution");
            return result;
        }
    };
} // namespace nodes
