export module kernel;
import std;
import content;
import codec;
import index;

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
    struct sequence_state {
        virtual ~sequence_state() = default;
        virtual auto size() const -> std::uint64_t = 0;
        virtual auto at(std::uint64_t offset) const -> element = 0;
        virtual auto position(const element& selected) const -> std::optional<std::uint64_t> = 0;
        virtual auto splice(std::uint64_t offset, std::uint64_t count, std::string run, std::uint64_t inserted) const -> std::shared_ptr<const sequence_state> = 0;
        virtual auto all() const -> std::vector<fragment> = 0;
        virtual auto save() const -> value = 0;
    };
    struct source_file;
    struct source_state;
    // What a kernel reads from its node's storage, which outlives the kernel and everything it loads.
    struct causal_reader {
        virtual ~causal_reader() = default;
        virtual auto header(std::size_t index) -> value = 0;
        virtual auto locate(const std::string& id) -> std::optional<std::size_t> = 0;
        virtual auto records(const std::string& kind, const std::string& key) -> std::vector<value> = 0;
        virtual auto text(const std::string& id) -> std::string = 0;
        virtual auto file_text(const source_file& file) -> std::string = 0;
        virtual auto projection(const std::vector<std::string>& heads) -> std::shared_ptr<const source_state> = 0;
        virtual auto base(const causal_set& wanted) -> std::pair<std::vector<std::string>, std::shared_ptr<const source_state>> = 0;
        virtual auto file_changed(const std::string& file, const causal_set& missing) -> bool = 0;
    };
    struct source_file {
        std::string id, path, kind = "text", digest;
        mutable std::string text, storage_header, content_object;
        bool executable = false, present = true;
        mutable std::vector<fragment> fragments;
        mutable bool fragments_ready = true;
        std::uint64_t stored_bytes = 0, stored_scalars = 0;
        // Set while the content is still in storage, under `content_object`.
        mutable causal_reader* stored = nullptr;
        mutable std::shared_ptr<const sequence_state> sequence;
        void ensure() const {
            (void)content();
            if (!fragments_ready && sequence) { fragments = sequence->all(); fragments_ready = true; }
        }
        auto content() const -> const std::string& {
            if (stored) { text = stored->file_text(*this); stored = nullptr; }
            return text;
        }
        auto bytes() const -> std::uint64_t { return stored ? stored_bytes : text.size(); }
        auto metadata_digest() const -> std::string {
            return digest_of(codec::encode(value::map{{"content", digest}, {"executable", executable}, {"file", id}, {"kind", kind}, {"path", path}}));
        }
        auto at(std::uint64_t offset) const -> element {
            if (sequence) return sequence->at(offset);
            for (auto& f : fragments) { if (offset < f.count) return {f.run, f.first + offset}; offset -= f.count; }
            fail(nodes::code::invalid_record, "text position outside visible source");
        }
        auto size() const -> std::uint64_t {
            if (sequence) return sequence->size();
            std::uint64_t n = 0; for (auto& f : fragments) n += f.count; return n;
        }
        // Where a character is in this file, or nothing when it is not visible here.
        auto position(const element& selected) const -> std::optional<std::uint64_t> {
            if (sequence) return sequence->position(selected);
            std::uint64_t n = 0;
            for (auto& f : fragments) {
                if (f.run == selected.run && selected.offset >= f.first && selected.offset - f.first < f.count) return n + (selected.offset - f.first);
                n += f.count;
            }
            return std::nullopt;
        }
    };
    struct source_state {
        persistent_index<std::shared_ptr<const source_file>> files;
        persistent_index<std::shared_ptr<const source_file>> hidden;
        persistent_index<std::shared_ptr<const std::set<std::string>>> paths;
        persistent_index<std::vector<std::string>> file_conflicts, path_conflicts, control_conflicts, identity_conflicts, dependency_conflicts;
        persistent_index<bool> activation, resolutions;
        causal_set causal;
        std::vector<std::string> conflicts;
        std::string digest;
        auto path(std::string_view path) const -> const source_file& {
            auto found = paths.find(path);
            if (found == paths.end() || found->second->empty())
                fail(nodes::code::invalid_record, "source path not found: " + std::string(path));
            if (found->second->size() != 1)
                fail(nodes::code::source_conflict, "path has multiple file identities");
            return *files.find(*found->second->begin())->second;
        }
    };

    class kernel {
        struct target {
            const std::string* run = nullptr;
            std::uint64_t offset = 0;
            auto operator<(const target& other) const -> bool { return std::tie(*run, offset) < std::tie(*other.run, other.offset); }
            auto matches(const element& other) const -> bool { return *run == other.run && offset == other.offset; }
        };
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
            // Set while the inserted text is still in storage, under `text_id`.
            mutable causal_reader* stored = nullptr;
            std::string text_id;
            std::uint64_t retained_scalars = 0;
            void ensure_text() const {
                if (stored) {
                    auto decoded = stored->text(text_id); std::u32string unicode;
                    for (unsigned char c : std::string_view{decoded}) if (c >= 128) { unicode = scalars(decoded); break; }
                    if ((unicode.empty() ? decoded.size() : unicode.size()) != retained_scalars) fail(nodes::code::storage_failure, "insertion scalar proof differs");
                    auto& self = const_cast<effect&>(*this); self.text = std::move(decoded); self.scalar_text = std::move(unicode); self.stored = nullptr;
                }
            }
            auto scalar_size() const -> std::size_t { return stored ? retained_scalars : scalar_text.empty() ? text.size() : scalar_text.size(); }
            auto scalar_at(std::size_t offset) const -> char32_t {
                ensure_text();
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
        mutable std::set<std::string> loaded_files_, loaded_effects_, loaded_controls_, loaded_dependencies_, loaded_identities_;
        mutable std::set<std::size_t> loaded_events_;
        mutable std::map<std::string, std::size_t> index_;
        mutable std::map<std::string, std::vector<const effect*>> effects_;
        mutable std::map<std::string, std::vector<std::size_t>> file_creators_;
        mutable std::map<std::string, std::vector<const effect*>> presence_;
        mutable std::map<std::string, std::vector<const effect*>> file_effects_;
        mutable std::vector<const effect*> control_effects_;
        mutable std::map<std::string, std::vector<const effect*>> activation_writes_;
        mutable std::map<std::string, std::vector<const effect*>> resolution_writes_;
        mutable std::map<std::string, std::vector<std::size_t>> dependency_events_, identity_events_;
        mutable std::map<std::string, std::set<std::string>> path_files_;
        mutable std::map<std::pair<std::string, std::string>, std::pair<std::string, std::string>> operations_;
        mutable std::map<std::string, std::string> blobs_;
        mutable bool aliases_present_ = false;
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
            e.id = h.at("id").text(); e.author = h.at("author").text(); e.operation = h.at("operation").text();
            e.clock = h.at("clock").number(); e.request_digest = h.at("request_digest").text(); e.effect_digest = h.at("effect_digest").text();
            e.dependencies = string_list(h.at("dependencies")); e.retained_effects = h.at("effects").number(); e.complete = false;
            for (auto& r : h.at("ancestors").list()) {
                auto first = r.at("first").number(), last = r.at("last").number();
                if (first >= last || last > index || (!e.ancestors.ranges.empty() && first <= e.ancestors.ranges.back().second))
                    fail(nodes::code::invalid_record, "causal proof intervals differ");
                e.ancestors.ranges.emplace_back(first, last);
            }
            if (!content::is_hash(e.id) || !content::is_hash(e.author) || !content::is_hash(e.operation) || !e.clock)
                fail(nodes::code::invalid_record, "causal event metadata differs");
            auto [found, inserted] = events_.emplace(index, std::move(e));
            if (!index_.emplace(found->second.id, index).second)
                fail(nodes::code::invalid_record, "duplicate indexed causal identity");
            auto& retained = found->second;
            operations_.try_emplace(std::pair{retained.author, retained.operation}, std::pair{retained.request_digest, retained.effect_digest});
            identity_events_[retained.author + ":" + retained.operation].push_back(index);
            for (auto& dep : retained.dependencies) dependency_events_[dep].push_back(index);
            return retained;
        }
        auto locate(const std::string& id) const -> std::map<std::string, std::size_t>::iterator {
            if (auto found = index_.find(id); found != index_.end()) return found;
            if (reader_) if (auto index = reader_->locate(id)) {
                if (event_at(*index).id != id) fail(nodes::code::invalid_record, "causal identity index differs");
            }
            return index_.find(id);
        }
        void register_effect(const effect* e) const {
            auto& aliases = effects_[e->id];
            if (std::ranges::find(aliases, e) != aliases.end()) return;
            aliases.push_back(e);
            if (e->type == "activate" || e->type == "resolve") control_effects_.push_back(e);
            else file_effects_[e->file].push_back(e);
            if (e->type == "activate") for (auto& target : e->selections) activation_writes_[target].push_back(e);
            if (e->type == "resolve") for (auto& target : e->selections) resolution_writes_[target].push_back(e);
            if (e->type == "presence") presence_[e->file].push_back(e);
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
            if (!e.complete && ordinal >= e.retained_effects) fail(nodes::code::invalid_record, "causal effect ordinal differs");
            if (e.complete || e.loaded_ordinals.contains(ordinal)) return;
            auto& input = record.at("effect");
            effect x{.id = operation_effect(e.author, e.operation, ordinal), .type = input.at("type").text(), .event = index, .ordinal = ordinal};
            if (x.type == "activate" || x.type == "resolve") {
                x.selections = string_list(input.at("selections")); x.property = input.at("value");
            } else {
                x.file = input.at("file").text();
                if (x.type == "insert") {
                    x.left = element::decode(input.at("left")); x.right = element::decode(input.at("right"));
                    x.retained_scalars = input.at("scalars").number();
                    x.stored = reader_; x.text_id = input.at("text_id").text();
                } else if (x.type == "erase") {
                    for (auto& target : input.at("targets").list()) {
                        auto run = target.at("run").text();
                        // The map owns anchor names even when their insertion is not loaded yet.
                        auto [found, _] = effects_.try_emplace(run);
                        x.targets.push_back({&found->first, target.at("offset").number()});
                    }
                } else {
                    x.property = input.at("value");
                    if (x.type == "binary") {
                        auto blob = record.at("binary").raw();
                        if (digest_of(blob) != x.property.text()) fail(nodes::code::storage_failure, "indexed binary content differs");
                        blobs_[x.property.text()] = std::move(blob);
                    }
                }
            }
            e.effects.push_back(std::move(x)); e.loaded_ordinals.insert(ordinal); register_effect(&e.effects.back());
        }
        void load_records(const std::string& kind, const std::string& key) const {
            if (reader_) for (auto& record : reader_->records(kind, key)) load_record(record);
        }
        void load_file(const std::string& file) const {
            if (!reader_ || loaded_files_.contains(file)) return;
            bool newly_created = file_creators_.contains(file) && !file_creators_.at(file).empty() &&
                std::ranges::all_of(file_creators_.at(file), [&](auto index) { return index >= retained_count_; });
            if (!newly_created) load_records("file", file);
            if (auto found = file_effects_.find(file); found != file_effects_.end()) for (auto* e : found->second) e->ensure_text();
            if (auto found = file_effects_.find(file); found != file_effects_.end())
                std::ranges::sort(found->second, [](auto* a, auto* b) { return std::tie(a->event, a->ordinal) < std::tie(b->event, b->ordinal); });
            loaded_files_.insert(file);
        }
        void load_presence(const std::string& file) const { load_records("presence", file); }
        void load_effect(const std::string& id) const { if (reader_ && !loaded_effects_.contains(id)) { load_records("effect", id); loaded_effects_.insert(id); } }
        void load_controls(const std::string& id) const { if (reader_ && !loaded_controls_.contains(id)) { load_records("control", id); loaded_controls_.insert(id); } }
        void load_dependents(const std::string& id) const {
            if (reader_ && !loaded_dependencies_.contains(id)) {
                for (auto& v : reader_->records("dependent", id)) (void)event_at(v.at("event").number());
                loaded_dependencies_.insert(id);
            }
        }
        void load_identity(const std::string& id) const {
            if (reader_ && !loaded_identities_.contains(id)) {
                for (auto& v : reader_->records("identity", id)) (void)event_at(v.at("event").number());
                loaded_identities_.insert(id);
            }
        }
        void load_event(std::size_t index) const {
            auto& e = event_at(index);
            if (!e.complete) {
                load_records("event", std::format("{:016x}", index));
                if (e.effects.size() != e.retained_effects) fail(nodes::code::missing_objects, "required causal effects are absent");
                std::set<std::string> created;
                for (auto& x : e.effects) if (x.type == "path" && x.file.starts_with(e.author + ":" + e.operation + ":file:")) created.insert(x.file);
                e.created = {created.begin(), created.end()};
                e.complete = true;
            }
        }
        void register_event(std::size_t index) {
            auto& event = event_at(index);
            index_.emplace(event.id, index);
            auto operation = std::pair{event.author, event.operation};
            if (operations_.contains(operation))
                aliases_present_ = true;
            operations_.try_emplace(std::move(operation), std::pair{event.request_digest, event.effect_digest});
            identity_events_[event.author + ":" + event.operation].push_back(index);
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
        auto before(const effect* a, const effect* b) const -> bool {
            if (before(a->event, b->event))
                return true;
            if (!aliases_present_ || a->id == b->id)
                return false;
            auto found = effects_.find(a->id);
            return found != effects_.end() && std::ranges::any_of(found->second, [&](auto* alias) {
                       return event_at(alias->event).request_digest == event_at(a->event).request_digest &&
                              event_at(alias->event).effect_digest == event_at(a->event).effect_digest && before(alias->event, b->event);
                   });
        }
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
            if (aliases_present_ || !count || count >= next.effects.size() || !previous.conflicts.empty() || previous.file->kind != "text")
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
            previous.file->ensure();
            if (previous.file->text.size() != previous.file->size())
                return false;
            next.file = std::make_shared<source_file>(*previous.file);
            next.file->storage_header.clear(); next.file->content_object.clear();
            next.file->sequence.reset();
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
                    std::map<std::string_view, std::set<std::uint64_t>> targets;
                    for (auto& target : change.targets)
                        targets[*target.run].insert(target.offset);
                    std::uint64_t required = 0, removed = 0, offset = 0;
                    for (auto& [_, values] : targets) required += values.size();
                    std::string text;
                    text.reserve(file.text.size());
                    std::vector<fragment> fragments;
                    auto append = [&](const fragment& original, std::uint64_t first, std::uint64_t length) {
                        if (!length) return;
                        text.append(file.text, offset + first - original.first, length);
                        fragments.push_back({original.run, first, length});
                    };
                    for (auto& original : file.fragments) {
                        auto first = original.first, last = first + original.count;
                        if (auto found = targets.find(original.run); found != targets.end())
                            for (auto target = found->second.lower_bound(first); target != found->second.end() && *target < last; ++target) {
                                append(original, first, *target - first);
                                first = *target + 1;
                                ++removed;
                            }
                        append(original, first, last - first);
                        offset += original.count;
                    }
                    if (removed != required) return false;
                    file.text = std::move(text);
                    file.fragments = std::move(fragments);
                } else {
                    auto left = change.left.run.empty() ? std::optional<std::uint64_t>{0} : position(change.left);
                    if (left && !change.left.run.empty()) ++*left;
                    auto right = change.right.run.empty() ? std::optional<std::uint64_t>{file.text.size()} : position(change.right);
                    if (!left || !right || *left != *right) return false;
                    std::vector<fragment> fragments;
                    std::uint64_t offset = 0;
                    bool inserted = false;
                    for (auto& original : file.fragments) {
                        if (!inserted && *left >= offset && *left <= offset + original.count) {
                            auto prefix = *left - offset;
                            if (prefix) fragments.push_back({original.run, original.first, prefix});
                            fragments.push_back({change.id, 0, change.scalar_size()});
                            if (prefix < original.count) fragments.push_back({original.run, original.first + prefix, original.count - prefix});
                            inserted = true;
                        } else fragments.push_back(original);
                        offset += original.count;
                    }
                    if (!inserted) fragments.push_back({change.id, 0, change.scalar_size()});
                    file.text.insert(*left, change.text);
                    file.fragments = std::move(fragments);
                }
            }
            file.digest = digest_of(file.text);
            return true;
        }
        auto render(const std::string& file_id, const std::vector<const effect*>& list, const persistent_index<bool>& active) const -> const cached_file& {
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
            std::map<const std::string*, std::vector<std::uint64_t>, run_less> deleted;
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
                        for (std::size_t i = 0; i < e->targets.size();) {
                            auto j = i + 1;
                            while (j < e->targets.size() && e->targets[j].run == e->targets[i].run)
                                ++j;
                            auto& offsets = deleted[e->targets[i].run];
                            auto count = offsets.size() + j - i;
                            if (count > offsets.capacity())
                                offsets.reserve(std::max(count, offsets.capacity() * 2));
                            for (; i < j; ++i)
                                offsets.push_back(e->targets[i].offset);
                        }
                    }
                } else if (is_active(e))
                    registers[e->type].push_back(e);
            }
            for (auto& [_, offsets] : deleted) {
                std::ranges::sort(offsets);
                offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
            }
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
                    f.kind = "binary";
                    auto found = blobs_.find(selected.text());
                    if (found == blobs_.end())
                        fail(nodes::code::missing_objects, "binary content is unavailable");
                    f.text = found->second;
                }
            }
            if (f.path.empty())
                f.present = false;
            bool serial = !aliases_present_ && std::adjacent_find(text_effects.begin(), text_effects.end(), [&](auto* a, auto* b) {
                                                   return a->event != b->event && !before(a->event, b->event);
                                               }) == text_effects.end();
            if (!serial)
                for (auto* a : text_effects)
                    for (auto* b : text_effects) {
                        if (a->id >= b->id || a->event == b->event || before(a, b) || before(b, a))
                            continue;
                        bool overlap = false;
                        if (a->type == "insert" && b->type == "insert")
                            overlap = a->left == b->left && a->right == b->right;
                        if (a->type == "erase" && b->type == "erase") {
                            std::set<target> targets(a->targets.begin(), a->targets.end());
                            overlap = std::ranges::any_of(b->targets, [&](auto& x) { return targets.contains(x); });
                        }
                        auto crossing = [](const effect* ins, const effect* del) {
                            if (ins->type != "insert" || del->type != "erase")
                                return false;
                            return std::ranges::any_of(del->targets, [&](auto& target) { return target.matches(ins->left) || target.matches(ins->right); });
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
                    const std::vector<std::uint64_t>* deleted;
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
                    if (!top.children && top.deleted && top.deleted->size() == e->scalar_size()) {
                        work.pop_back();
                        continue;
                    }
                    if (!top.children && e->text.size() == e->scalar_size() &&
                        (!top.deleted || std::lower_bound(top.deleted->begin(), top.deleted->end(), offset) == top.deleted->end())) {
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
                    bool visible = top.active && (!top.deleted || !std::binary_search(top.deleted->begin(), top.deleted->end(), offset));
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
            if (events_.size() != event_count_) return false;
            for (auto& [_, event] : events_) {
                if (!event.complete) return false;
                for (auto& effect : event.effects) if (effect.stored) return false;
            }
            return true;
        }
        void attach_reader(causal_reader* reader) { reader_ = reader; retained_count_ = event_count_; }
        void detach_reader() { reader_ = nullptr; discard_projections(); }
        void demand(std::size_t count, bool aliases, causal_reader* reader) {
            if (event_count_ || !events_.empty()) fail(nodes::code::invalid_record, "demand kernel must start empty");
            event_count_ = count; retained_count_ = count; aliases_present_ = aliases; reader_ = reader;
        }
        void seed(std::vector<std::string> heads, std::shared_ptr<const source_state> state) {
            if (closure(heads).ranges != state->causal.ranges) fail(nodes::code::invalid_record, "projection causal proof differs");
            projected_.emplace_back(std::move(heads), std::move(state));
        }
        auto aliases() const -> bool { return aliases_present_; }
        auto effect_files(const std::string& id) const -> std::set<std::string> {
            load_effect(id); std::set<std::string> files;
            if (auto found = effects_.find(id); found != effects_.end()) for (auto* effect : found->second) if (!effect->file.empty()) files.insert(effect->file);
            return files;
        }
        auto identity_events(const std::string& logical) const -> std::vector<std::string> {
            load_identity(logical); std::vector<std::string> ids;
            if (auto found = identity_events_.find(logical); found != identity_events_.end())
                for (auto index : found->second) ids.push_back(event_at(index).id);
            return canonical_set(std::move(ids));
        }
        auto loaded_events() const -> std::size_t { return events_.size(); }
        auto loaded_files() const -> std::size_t { return loaded_files_.size() + cache_.size(); }
        auto event_header(std::size_t index) const -> value {
            auto& e = event_at(index); value::array ranges;
            for (auto [first, last] : e.ancestors.ranges) ranges.emplace_back(value::map{{"first", static_cast<std::uint64_t>(first)}, {"last", static_cast<std::uint64_t>(last)}});
            return value::map{{"index", static_cast<std::uint64_t>(index)}, {"id", e.id}, {"author", e.author}, {"operation", e.operation}, {"clock", e.clock},
                {"ancestors", std::move(ranges)}, {"effects", static_cast<std::uint64_t>(e.complete ? e.effects.size() : e.retained_effects)}, {"dependencies", strings(e.dependencies)}, {"request_digest", e.request_digest}, {"effect_digest", e.effect_digest}};
        }
        auto event_records(std::size_t index) const -> std::vector<value> {
            load_event(index); auto& e = event_at(index); std::vector<value> out;
            for (auto& x : e.effects) {
                x.ensure_text();
                value::map input{{"type", x.type}};
                if (x.type == "activate" || x.type == "resolve") { input["selections"] = strings(x.selections); input["value"] = x.property; }
                else {
                    input["file"] = x.file;
                    if (x.type == "insert") { input["left"] = x.left.encoded(); input["right"] = x.right.encoded(); input["text"] = x.text; }
                    else if (x.type == "erase") { value::array targets; for (auto& t : x.targets) targets.push_back(element{*t.run, t.offset}.encoded()); input["targets"] = std::move(targets); }
                    else input["value"] = x.property;
                }
                out.emplace_back(value::map{{"event", static_cast<std::uint64_t>(index)}, {"ordinal", static_cast<std::uint64_t>(x.ordinal)}, {"effect", std::move(input)},
                    {"binary", bytes{x.type == "binary" ? blobs_.at(x.property.text()) : std::string{}}}});
            }
            return out;
        }
        auto snapshot(std::size_t first = 0, std::size_t count = std::numeric_limits<std::size_t>::max()) const -> std::string {
            if (first > event_count_)
                fail(nodes::code::invalid_record, "snapshot event range");
            auto last = first + std::min(count, event_count_ - first);
            std::string out{"DCS2"};
            std::set<std::string> binary;
            auto integer = [&](std::uint64_t n) { codec::integer(out, n); };
            auto text = [&](std::string_view s) {
                integer(s.size());
                out.append(s);
            };
            auto strings = [&](const auto& values) {
                integer(values.size());
                for (auto& s : values)
                    text(s);
            };
            integer(first);
            integer(last - first);
            for (auto index = first; index < last; ++index) {
                load_event(index);
                auto& event = event_at(index);
                text(event.id);
                text(event.author);
                text(event.operation);
                integer(event.clock);
                integer(event.ancestors.ranges.size());
                for (auto [lo, hi] : event.ancestors.ranges) {
                    integer(lo);
                    integer(hi);
                }
                strings(event.dependencies);
                strings(event.created);
                text(event.request_digest);
                text(event.effect_digest);
                integer(event.effects.size());
                std::vector<const effect*> ordered;
                for (auto& e : event.effects) ordered.push_back(&e);
                std::ranges::sort(ordered, [](auto* a, auto* b) { return a->ordinal < b->ordinal; });
                for (auto* ptr : ordered) { const auto& e = *ptr;
                    e.ensure_text();
                    text(e.id);
                    text(e.type);
                    text(e.file);
                    text(e.text);
                    integer(e.ordinal);
                    text(e.left.run);
                    integer(e.left.offset);
                    text(e.right.run);
                    integer(e.right.offset);
                    integer(e.targets.size());
                    for (auto& t : e.targets) {
                        text(*t.run);
                        integer(t.offset);
                    }
                    strings(e.selections);
                    codec::encode_value(out, e.property, 0);
                    if (e.type == "binary")
                        binary.insert(e.property.text());
                }
            }
            integer(binary.size());
            for (auto& id : binary) {
                text(id);
                text(blobs_.at(id));
            }
            return out;
        }
        void append_snapshot(std::string_view input) {
            if (!input.starts_with("DCS2"))
                fail(nodes::code::invalid_record, "CRDT state snapshot framing");
            std::size_t offset = 4;
            auto take = [&](std::size_t size) {
                if (size > input.size() - offset)
                    fail(nodes::code::invalid_record, "truncated CRDT state snapshot");
                auto result = input.substr(offset, size);
                offset += size;
                return result;
            };
            auto integer = [&]() {
                auto bytes = take(8);
                std::uint64_t n = 0;
                for (int i = 7; i >= 0; --i)
                    n = (n << 8) | static_cast<unsigned char>(bytes[i]);
                return n;
            };
            auto count = [&]() {
                auto n = integer();
                if (n > codec::collection_limit || n > input.size() - offset)
                    fail(nodes::code::resource_limit, "CRDT snapshot collection limit");
                return n;
            };
            auto text = [&]() {
                auto size = integer();
                return std::string(take(size));
            };
            auto strings = [&]() {
                std::vector<std::string> values;
                auto n = count();
                values.reserve(n);
                while (n--)
                    values.push_back(text());
                return values;
            };
            if (integer() != event_count_)
                fail(nodes::code::invalid_record, "CRDT snapshot event gap");
            auto events = count();
            while (events--) {
                auto index = event_count_;
                events_.try_emplace(event_count_++);
                auto& event = events_.at(index);
                event.id = text();
                event.author = text();
                event.operation = text();
                event.clock = integer();
                auto ranges = count();
                while (ranges--) {
                    auto lo = integer(), hi = integer();
                    if (lo >= hi || hi > index || (!event.ancestors.ranges.empty() && lo <= event.ancestors.ranges.back().second))
                        fail(nodes::code::invalid_record, "snapshot causal intervals");
                    event.ancestors.ranges.emplace_back(lo, hi);
                }
                event.dependencies = strings();
                event.created = strings();
                event.request_digest = text();
                event.effect_digest = text();
                if (index_.contains(event.id))
                    fail(nodes::code::invalid_record, "duplicate snapshot event");
                auto effects = count();
                for (std::size_t ordinal = 0; ordinal < effects; ++ordinal) {
                    event.effects.emplace_back();
                    auto& e = event.effects.back();
                    e.id = text();
                    e.type = text();
                    e.file = text();
                    e.text = text();
                    e.ordinal = integer();
                    e.event = index;
                    if (e.ordinal != ordinal)
                        fail(nodes::code::invalid_record, "snapshot effect ordering");
                    e.left.run = text();
                    e.left.offset = integer();
                    e.right.run = text();
                    e.right.offset = integer();
                    auto targets = count();
                    e.targets.reserve(targets);
                    while (targets--) {
                        auto run = text();
                        auto position = integer();
                        auto found = effects_.find(run);
                        if (found == effects_.end())
                            fail(nodes::code::invalid_record, "snapshot deletion anchor is absent");
                        e.targets.push_back({&found->first, position});
                    }
                    e.selections = strings();
                    codec::decoder decoder{input};
                    decoder.offset = offset;
                    e.property = decoder.read();
                    offset = decoder.offset;
                    for (unsigned char c : std::string_view{e.text})
                        if (c >= 128) {
                            e.scalar_text = scalars(e.text);
                            break;
                        }
                    effects_[e.id].push_back(&e);
                }
                register_event(index);
            }
            auto blobs = count();
            while (blobs--) {
                auto id = text();
                auto data = text();
                if (digest_of(data) != id)
                    fail(nodes::code::invalid_record, "snapshot binary digest mismatch");
                blobs_.try_emplace(std::move(id), std::move(data));
            }
            if (offset != input.size())
                fail(nodes::code::invalid_record, "trailing CRDT state snapshot bytes");
        }
        void restore(std::string_view input) {
            kernel restored;
            restored.append_snapshot(input);
            *this = std::move(restored);
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
        auto event_author(const std::string& id) const -> const std::string& {
            auto found = locate(id);
            if (found == index_.end()) {
                error e{nodes::code::missing_objects, "unknown source batch"};
                e.missing.push_back(id);
                throw e;
            }
            return event_at(found->second).author;
        }
        // One history checked against many events: the closure of heads is built once.
        auto within(const std::vector<std::string>& heads) const {
            return [this, bits = closure(heads)](const std::string& event) {
                auto found = locate(event);
                return found != index_.end() && contains(bits, found->second);
            };
        }
        auto includes(const std::vector<std::string>& heads, const std::string& event) const -> bool {
            auto found = locate(event);
            return found != index_.end() && contains(closure(heads), found->second);
        }
        auto uncovered(const std::vector<std::string>& heads, const std::vector<std::string>& admitted) const -> std::vector<std::string> {
            auto selected = closure(heads), covered = closure(admitted);
            std::vector<std::string> out;
            selected.without(covered).each([&](auto i) { out.push_back(event_at(i).id); });
            return out;
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
                        explicit_reads.push_back(element::decode(target).run);
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
            load_identity(next.author + ":" + next.operation);
            auto operation_key = std::pair{next.author, next.operation};
            auto payload = std::pair{body.at("request_digest").text(), body.at("effect_digest").text()};
            if (operations_.contains(operation_key))
                aliases_present_ = true;
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
                    effect x{.id = operation_effect(e.author, e.operation, ordinal),
                             .type = input.at("type").text(),
                             .event = event_count_ - 1,
                             .ordinal = ordinal};
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
                            if (std::ranges::any_of(x.selections, [](auto& selected) { return selected.starts_with("identity:"); }))
                                fail(nodes::code::identity_conflict, "identity decisions select a coherent worldline frontier");
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
                            std::set<target> unique;
                            x.targets.reserve(input.at("targets").list().size());
                            for (auto& selected : input.at("targets").list()) {
                                fields(selected, {"offset", "run"});
                                auto offset = selected.at("offset").number();
                                auto* run = valid_element(selected.at("run").text(), offset, e, ordinal, x.file, false);
                                target ref{run, offset};
                                if (!unique.insert(ref).second)
                                    fail(nodes::code::invalid_record, "duplicate deletion target");
                                x.targets.push_back(ref);
                            }
                            if (x.targets.empty())
                                fail(nodes::code::invalid_record, "empty deletion");
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
        auto extend_sequence(const source_file& previous, const std::vector<const effect*>& changes, const causal_set& base) const -> std::shared_ptr<const source_file> {
            if (!previous.sequence || changes.empty()) return {};
            for (auto* change : changes) {
                auto& event = event_at(change->event);
                if ((change->type == "insert" || change->type == "erase") && previous.kind != "text") return {};
                if (change->type != "insert" && change->type != "erase" && change->type != "path" && change->type != "mode" && change->type != "presence" && change->type != "binary") return {};
                auto missing = base.without(event.ancestors);
                if (!missing.ranges.empty()) {
                    if (!reader_ || reader_->file_changed(previous.id, missing)) return {};
                    bool touched = false;
                    for (auto [first, last] : missing.ranges) for (auto index = std::max(first, retained_count_); index < last; ++index)
                        for (auto& effect : event_at(index).effects) {
                            if (effect.file == previous.id) touched = true;
                            if (effect.type == "activate") for (auto& target : effect.selections) {
                                load_effect(target); for (auto* alias : effects_.at(target)) if (alias->file == previous.id) touched = true;
                            }
                        }
                    if (touched) return {};
                }
                auto logical = event.author + ":" + event.operation; load_identity(logical);
                if (std::ranges::any_of(identity_events_.at(logical), [&](auto index) { return index != change->event && base.contains(index); })) return {};
            }
            for (std::size_t i = 1; i < changes.size(); ++i)
                if (changes[i - 1]->event != changes[i]->event && !event_at(changes[i]->event).ancestors.contains(changes[i - 1]->event)) return {};
            auto file = std::make_shared<source_file>(previous);
            std::optional<std::u32string> text;
            auto edit_text = [&]() -> std::u32string& {
                if (!text) { text = scalars(previous.content()); file->stored = nullptr; file->content_object.clear(); file->fragments.clear(); file->fragments_ready = false; }
                return *text;
            };
            file->storage_header.clear();
            for (auto* change : changes) {
                if (change->type == "erase") {
                    std::vector<std::uint64_t> offsets;
                    for (auto& selected : change->targets) {
                        auto at = file->sequence->position({*selected.run, selected.offset});
                        if (!at) return {};
                        offsets.push_back(*at);
                    }
                    std::ranges::sort(offsets);
                    if (std::adjacent_find(offsets.begin(), offsets.end()) != offsets.end()) return {};
                    for (std::size_t end = offsets.size(); end;) {
                        auto first = end - 1;
                        while (first && offsets[first - 1] + 1 == offsets[first]) --first;
                        auto count = end - first;
                        file->sequence = file->sequence->splice(offsets[first], count, {}, 0);
                        edit_text().erase(offsets[first], count); end = first;
                    }
                } else if (change->type == "insert") {
                    auto left = change->left.run.empty() ? std::optional<std::uint64_t>{0} : file->sequence->position(change->left);
                    if (left && !change->left.run.empty()) ++*left;
                    auto right = change->right.run.empty() ? std::optional<std::uint64_t>{file->sequence->size()} : file->sequence->position(change->right);
                    if (!left || !right || *left != *right) return {};
                    change->ensure_text();
                    auto inserted = scalars(change->text);
                    file->sequence = file->sequence->splice(*left, 0, change->id, inserted.size());
                    edit_text().insert(*left, inserted);
                } else if (change->type == "path") file->path = change->property.text();
                else if (change->type == "mode") file->executable = change->property.boolean();
                else if (change->type == "presence") file->present = change->property.boolean();
                else {
                    if (file->kind != "binary") return {};
                    file->text = blobs_.at(change->property.text()); file->digest = change->property.text(); file->stored = nullptr; file->content_object.clear();
                }
            }
            if (text) { file->text.clear(); for (auto c : *text) append_scalar(file->text, c); file->digest = digest_of(file->text); }
            return file;
        }
        auto compute(const std::vector<std::string>& heads) const -> source_state {
            source_state out;
            if (!projected_.empty())
                out = *projected_.back().second;
            auto base = out;
            auto bits = closure(heads);
            auto delta = changed(out.causal, bits);
            out.causal = bits;
            std::set<std::string> controls, resolutions, identities, dependencies;
            delta.each([&](auto index) {
                load_event(index);
                auto& event = event_at(index);
                identities.insert(event.author + ":" + event.operation);
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
                    { auto ids = canonical_set(std::move(values)); index.set(key, ids, digest_of(codec::encode(value::map{{"ids", strings(ids)}}))); }
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
                out.activation.set(target, state, digest_of(codec::encode(value::map{{"value", state}})));
                assign_conflicts(out.control_conflicts, target, std::move(conflicts));
            }
            for (auto& target : resolutions) {
                load_controls(target);
                if (std::ranges::any_of(resolution_writes_.at(target), [&](auto* write) { return contains(bits, write->event); }))
                    out.resolutions.set(target, true, digest_of(codec::encode(value::map{{"value", true}})));
                else
                    out.resolutions.erase(target);
            }
            for (auto& identity : identities) {
                load_identity(identity);
                std::set<std::string> claims;
                auto& group = identity_events_.at(identity);
                if (group.size() > 1)
                    for (auto index : group)
                        if (contains(bits, index))
                            claims.insert(event_at(index).request_digest + event_at(index).effect_digest);
                std::vector<std::string> conflicts;
                if (claims.size() > 1) {
                    std::set<std::string> requests;
                    for (auto& claim : claims)
                        requests.insert(claim.substr(0, 64));
                    auto ids = requests.size() > 1 ? std::vector<std::string>{requests.begin(), requests.end()}
                                                   : std::vector<std::string>{claims.begin(), claims.end()};
                    ids.push_back(identity);
                    conflicts.push_back(conflict("identity", std::move(ids)));
                }
                assign_conflicts(out.identity_conflicts, identity, std::move(conflicts));
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
            std::vector<const effect*> list;
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
                    out.paths.set(path, std::make_shared<const std::set<std::string>>(owners), digest_of(codec::encode(value::map{{"ids", strings({owners.begin(), owners.end()})}})));
                changed_paths.insert(path);
            };
            for (auto& id : affected_files(delta)) {
                ++projected_files_;
                auto old = out.files.find(id);
                auto previous = old == out.files.end() ? std::shared_ptr<const source_file>{} : old->second;
                list.clear();
                delta.each([&](auto index) { for (auto& e : event_at(index).effects) if (e.file == id) list.push_back(&e); });
                std::shared_ptr<const source_file> current, extended;
                if (previous && controls.empty() && resolutions.empty() && base.causal.without(bits).ranges.empty() && !base.file_conflicts.contains(id) && base.identity_conflicts.empty())
                    extended = extend_sequence(*previous, list, base.causal);
                if (extended) {
                    if (extended->present) { current = extended; out.hidden.erase(id); }
                    else out.hidden.set(id, extended, extended->metadata_digest());
                } else {
                    list.clear(); load_file(id);
                    if (auto found = file_effects_.find(id); found != file_effects_.end())
                        for (auto* effect : found->second) if (contains(bits, effect->event)) list.push_back(effect);
                    if (aliases_present_) std::ranges::sort(list, [&](auto* a, auto* b) { return std::tuple{a->id, event_at(a->event).id} < std::tuple{b->id, event_at(b->event).id}; });
                    if (!list.empty()) {
                        auto& file = render(id, list, out.activation);
                        if (file.file->present) { current = file.file; out.hidden.erase(id); }
                        else out.hidden.set(id, file.file, file.file->metadata_digest());
                        assign_conflicts(out.file_conflicts, id, file.conflicts);
                    } else { out.file_conflicts.erase(id); out.hidden.erase(id); }
                }
                if (previous && (!current || previous->path != current->path))
                    path_owner(previous->path, id, false);
                if (current && (!previous || previous->path != current->path))
                    path_owner(current->path, id, true);
                if (current)
                    out.files.set(id, current, current->metadata_digest());
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
            for (auto* index : {&out.file_conflicts, &out.path_conflicts, &out.control_conflicts, &out.identity_conflicts, &out.dependency_conflicts})
                for (auto& [_, conflicts] : *index)
                    for (auto& id : conflicts)
                        if (!out.resolutions.contains(id))
                            out.conflicts.push_back(id);
            out.conflicts = canonical_set(std::move(out.conflicts));
            out.digest = digest_of(codec::encode(value::map{{"conflicts", strings(out.conflicts)}, {"files", out.files.digest()}}));
            return out;
        }

    public:
        void discard_projections() const { projected_.clear(); cache_.clear(); }
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
                    auto entry = std::move(projected_[i]); auto cached = entry.second;
                    projected_.erase(projected_.begin() + static_cast<std::ptrdiff_t>(i)); projected_.push_back(std::move(entry));
                    return cached;
                }
            if (reader_) if (auto state = reader_->projection(ordered)) {
                if (closure(ordered).ranges != state->causal.ranges) fail(nodes::code::invalid_record, "projection causal proof differs");
                projected_.emplace_back(ordered, state); if (projected_.size() > 8) projected_.pop_front(); return state;
            }
            auto wanted = closure(ordered);
            if (ordered.empty()) {
                auto empty = std::make_shared<source_state>();
                empty->digest = digest_of(codec::encode(value::map{{"conflicts", strings({})}, {"files", ""}}));
                projected_.emplace_back(ordered, empty); if (projected_.size() > 8) projected_.pop_front(); return empty;
            }
            if (projected_.empty() && reader_) {
                auto [frontier, base] = reader_->base(wanted);
                if (base) {
                    if (closure(frontier).ranges != base->causal.ranges) fail(nodes::code::invalid_record, "base projection causal proof differs");
                    projected_.emplace_back(std::move(frontier), std::move(base));
                }
            }
            if (projected_.size() > 1) {
                std::size_t best = 0; std::uint64_t score = std::numeric_limits<std::uint64_t>::max();
                for (std::size_t i = 0; i < projected_.size(); ++i) {
                    auto difference = changed(projected_[i].second->causal, wanted); std::uint64_t size = 0;
                    for (auto [first, last] : difference.ranges) size += last - first;
                    if (size < score) { score = size; best = i; }
                }
                auto entry = std::move(projected_[best]); projected_.erase(projected_.begin() + static_cast<std::ptrdiff_t>(best)); projected_.push_back(std::move(entry));
            }
            auto state = std::make_shared<const source_state>(compute(heads));
            projected_.emplace_back(std::move(ordered), state);
            if (projected_.size() > 8)
                projected_.pop_front();
            return state;
        }
        auto project(const std::vector<std::string>& heads) const -> source_state { return *projection(heads); }
        auto active(const std::vector<std::string>& heads, const std::string& target) const -> bool {
            auto bits = closure(heads);
            std::vector<const effect*> writes;
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
