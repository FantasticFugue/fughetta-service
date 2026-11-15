module;
#include <sqlite3.h>
export module engine:state;
import :api;
import std;
import constants;
import content;
import codec;
import kernel;
import durability;
import archive;
import index;
import reflection;
import nodestore;

export namespace nodes {
    using namespace persistence;
    struct local_node::impl {
        struct body_view {
            const value* body;
            operator const value&() const { return *body; }
            auto at(std::string_view name) const -> const value& { return body->at(name); }
            auto object() const -> const value::map& { return body->object(); }
        };
        std::filesystem::path root;
        archive::files io_files;
        key author, node;
        std::unique_ptr<database> db;
        std::string project;
        bool read_only = false, archive_audited = false;
        std::string observed_snapshot;
        mutable std::map<std::string, value> objects;
        mutable std::map<std::string, body_view> bodies;
        std::map<std::string, value> decoded_inputs;
        mutable std::deque<std::string> object_order;
        mutable std::unordered_map<std::string, std::size_t> object_sizes;
        mutable std::size_t cached_object_bytes = 0;
        std::map<std::string, std::vector<std::string>> receipt_index;
        mutable std::set<std::string> validated;
        std::map<std::string, std::pair<std::string, std::string>> refused;
        std::map<std::string, std::set<std::string>> operation_claims;
        std::map<std::string, std::string> identity_conflicts;
        std::vector<std::string> policies;
        kernel source;
        std::map<std::string, std::vector<std::string>> roots;
        std::map<std::string, std::string> root_admissions;
        // Worldline names bound by validated acceptances, and the lines with accepted source, built on first use after each reload.
        struct line_index {
            std::map<std::string, std::set<std::string>> names;
            std::set<std::string> active;
        };
        mutable std::optional<line_index> line_names;
        bool dirty = false;
        std::uint64_t data_version = 0;
        std::uint64_t snapshot_watermark = 0, snapshot_events = 0, replayed_events = 0;
        mutable std::uint64_t loaded_records = 0;
        std::string state_snapshot;
        std::unique_ptr<state_tree> pages;
        bool snapshot_ready = false, kernel_loaded = true, status_loaded = true, pending_loaded = true, kernel_aliases = false;
        std::uint64_t causal_headers_read = 0, causal_effects_read = 0, file_histories_read = 0, file_payloads_read = 0, projected_before_eviction = 0,
                      sequence_pages_read = 0, sequence_pages_written = 0, insertion_payloads_read = 0;
        std::set<std::string> cached_payload_files;
        std::uint64_t cached_payload_bytes = 0, cache_evictions = 0;
        std::deque<std::pair<std::string, std::string>> projection_garbage;
        // A projection store; `Codec` names its pages and gives `item`, `encode`, `decode` and `hash` for what it holds.
        template <class Codec> struct projection_index : persistent_index<typename Codec::item>::backend {
            using T = typename Codec::item;
            static constexpr std::string_view name = Codec::name;
            impl& self;
            state_tree& pages;
            std::uint64_t* generated = nullptr;
            projection_index(impl& self, state_tree& pages) : self(self), pages(pages) {}
            using data = persistent_index<T>::node_data;
            auto digest(const T& item) const -> std::string override { return Codec::hash(item); }
            auto load(std::string_view id) -> data override {
                auto bytes = pages.get("j/" + std::string(name) + "/" + std::string(id));
                if (!bytes)
                    fail(nodes::code::missing_objects, "required projection branch is absent");
                if (digest_of(*bytes) != id)
                    fail(nodes::code::storage_failure, "projection object identity differs");
                auto v = codec::decode(*bytes);
                fields(v, {"count", "item", "key", "left", "left_count", "left_mass", "left_object", "mass", "right", "right_count", "right_mass",
                           "right_object", "value"});
                return {.key = v.at("key").text(),
                        .item = Codec::decode(self, v.at("item")),
                        .value_digest = v.at("value").text(),
                        .left = v.at("left").text(),
                        .right = v.at("right").text(),
                        .left_object = v.at("left_object").text(),
                        .right_object = v.at("right_object").text(),
                        .count = v.at("count").number(),
                        .left_count = v.at("left_count").number(),
                        .right_count = v.at("right_count").number(),
                        .mass = v.at("mass").number(),
                        .left_mass = v.at("left_mass").number(),
                        .right_mass = v.at("right_mass").number(),
                        .bound = true};
            }
            auto store(const data& node) -> std::string override {
                if (!node.bound || node.value_digest != Codec::hash(node.item))
                    fail(nodes::code::invalid_record, "projection value digest differs");
                auto item = Codec::encode(self, node.item);
                auto bytes = codec::encode(value::map{{"key", node.key},
                                                      {"item", item},
                                                      {"value", node.value_digest},
                                                      {"left", node.left},
                                                      {"right", node.right},
                                                      {"left_object", node.left_object},
                                                      {"right_object", node.right_object},
                                                      {"count", static_cast<std::uint64_t>(node.count)},
                                                      {"left_count", static_cast<std::uint64_t>(node.left_count)},
                                                      {"right_count", static_cast<std::uint64_t>(node.right_count)},
                                                      {"mass", node.mass},
                                                      {"left_mass", node.left_mass},
                                                      {"right_mass", node.right_mass}});
                auto id = digest_of(bytes), key = "j/" + std::string(name) + "/" + id;
                if (auto previous = pages.get(key)) {
                    if (*previous != bytes)
                        fail(nodes::code::identity_conflict, "projection object has differing content");
                    return id;
                }
                pages.set(key, bytes);
                if (generated)
                    ++*generated;
                pages.set("jrefs/" + std::string(name) + "/" + id, codec::encode(value::map{{"refs", std::uint64_t{0}}}));
                self.projection_reference(std::string(name), node.left_object, 1);
                self.projection_reference(std::string(name), node.right_object, 1);
                if constexpr (name == "files" || name == "hidden") {
                    self.payload_reference(item.at("data").text(), 1);
                    auto& sequence = item.at("sequence");
                    self.projection_reference("sequence", sequence.at("pieces").at("object").text(), 1);
                    self.projection_reference("anchors", sequence.at("anchors").at("object").text(), 1);
                }
                return id;
            }
        };
        struct anchor_span {
            std::string key;
            std::uint64_t first = 0, count = 0;
            auto operator==(const anchor_span&) const -> bool = default;
        };
        static auto fragment_value(const fragment& f) -> value;
        static auto span_value(const anchor_span& f) -> value;
        struct sequence_pages {
            using item = fragment;
            static constexpr std::string_view name = "sequence";
            static auto encode(impl&, const fragment& f) -> value { return fragment_value(f); }
            static auto decode(impl& self, const value& v) -> fragment {
                ++self.sequence_pages_read;
                fields(v, {"count", "first", "run"});
                auto count = v.at("count").number();
                if (!count || count > 1024)
                    fail(nodes::code::invalid_record, "sequence scalar page bound differs");
                return fragment{v.at("run").text(), v.at("first").number(), count};
            }
            static auto hash(const fragment& f) -> std::string { return digest_of(codec::encode(fragment_value(f))); }
        };
        struct anchor_pages {
            using item = anchor_span;
            static constexpr std::string_view name = "anchors";
            static auto encode(impl&, const anchor_span& f) -> value { return span_value(f); }
            static auto decode(impl& self, const value& v) -> anchor_span {
                ++self.sequence_pages_read;
                fields(v, {"count", "first", "key"});
                return anchor_span{v.at("key").text(), v.at("first").number(), v.at("count").number()};
            }
            static auto hash(const anchor_span& f) -> std::string { return digest_of(codec::encode(span_value(f))); }
        };
        std::shared_ptr<projection_index<sequence_pages>> sequence_index;
        std::shared_ptr<projection_index<anchor_pages>> anchor_index;
        struct sequence_view : sequence_state {
            persistent_index<fragment> pieces;
            persistent_index<anchor_span> anchors;
            std::shared_ptr<projection_index<sequence_pages>> piece_storage;
            std::shared_ptr<projection_index<anchor_pages>> anchor_storage;
            static auto anchor_key(const std::string& run, std::uint64_t first) -> std::string { return run + "/" + std::format("{:016x}", first); }
            static auto between(const std::string& left, const std::string& right) -> std::string {
                if (!right.empty() && left >= right)
                    fail(nodes::code::invalid_record, "sequence ordering interval differs");
                std::string out;
                bool bounded = !right.empty();
                for (std::size_t i = 0; i < 1024; ++i) {
                    unsigned lo = i < left.size() ? static_cast<unsigned char>(left[i]) : 32;
                    unsigned hi = bounded && i < right.size() ? static_cast<unsigned char>(right[i]) : 127;
                    if (hi > lo + 1) {
                        out.push_back(static_cast<char>((hi + lo) / 2));
                        return out;
                    }
                    out.push_back(static_cast<char>(lo));
                    if (lo < hi)
                        bounded = false;
                }
                fail(nodes::code::resource_limit, "sequence ordering precision exhausted");
            }
            auto size() const -> std::uint64_t override { return pieces.mass(); }
            auto at(std::uint64_t offset) const -> element override {
                auto [it, relative] = pieces.at_mass(offset);
                return {it->second.run, it->second.first + relative};
            }
            auto position(const element& selected) const -> std::optional<std::uint64_t> override {
                auto found = extent(selected);
                if (!found)
                    return {};
                return found->first;
            }
            auto extent(const element& selected) const -> std::optional<std::pair<std::uint64_t, std::uint64_t>> override {
                auto it = anchors.floor(anchor_key(selected.run, selected.offset));
                auto prefix = selected.run + "/";
                if (it == anchors.end() || !it->first.starts_with(prefix) || selected.offset < it->second.first ||
                    selected.offset - it->second.first >= it->second.count)
                    return {};
                auto piece = pieces.find(it->second.key);
                if (piece == pieces.end() || piece->second.run != selected.run || piece->second.first != it->second.first ||
                    piece->second.count != it->second.count)
                    fail(nodes::code::storage_failure, "sequence anchor and counted piece differ");
                auto inside = selected.offset - piece->second.first;
                return std::pair{pieces.mass_before(piece->first) + inside, piece->second.count - inside};
            }
            void insert(const std::string& key, fragment f) {
                if (!f.count || f.count > 1024)
                    fail(nodes::code::invalid_record, "sequence piece scalar bound differs");
                pieces.set(key, f, digest_of(codec::encode(fragment_value(f))));
                anchor_span span{key, f.first, f.count};
                anchors.set(anchor_key(f.run, f.first), span, digest_of(codec::encode(span_value(span))));
            }
            auto splice(std::uint64_t offset, std::uint64_t count, std::string run, std::uint64_t inserted) const
                -> std::shared_ptr<const sequence_state> override {
                if (offset > size() || count > size() - offset || (inserted && run.empty()))
                    fail(nodes::code::invalid_record, "sequence splice bounds differ");
                auto next = std::make_shared<sequence_view>(*this);
                std::vector<std::string> removed;
                std::vector<fragment> replacement;
                std::optional<fragment> suffix;
                std::string left, right;
                if (offset < size()) {
                    auto [it, first] = pieces.at_mass(offset);
                    auto before = pieces.floor(it->first, true);
                    if (before != pieces.end())
                        left = before->first;
                    std::uint64_t remaining = count;
                    if (first)
                        replacement.push_back({it->second.run, it->second.first, first});
                    for (;;) {
                        auto f = it->second;
                        removed.push_back(it->first);
                        auto taken = std::min(remaining, f.count - first);
                        if (first + taken < f.count)
                            suffix = fragment{f.run, f.first + first + taken, f.count - first - taken};
                        remaining -= taken;
                        ++it;
                        if (!remaining) {
                            if (it != pieces.end())
                                right = it->first;
                            break;
                        }
                        if (it == pieces.end())
                            fail(nodes::code::storage_failure, "sequence region is incomplete");
                        first = 0;
                    }
                } else {
                    auto before = pieces.floor(std::string(1, static_cast<char>(127)));
                    if (before != pieces.end())
                        left = before->first;
                }
                for (auto& key : removed) {
                    auto f = next->pieces.find(key)->second;
                    next->anchors.erase(anchor_key(f.run, f.first));
                    next->pieces.erase(key);
                }
                for (std::uint64_t first = 0; first < inserted;) {
                    auto length = std::min<std::uint64_t>(1024, inserted - first);
                    replacement.push_back({run, first, length});
                    first += length;
                }
                if (suffix)
                    replacement.push_back(*suffix);
                for (auto& f : replacement) {
                    auto key = between(left, right);
                    next->insert(key, f);
                    left = std::move(key);
                }
                if (next->size() != size() - count + inserted)
                    fail(nodes::code::storage_failure, "sequence splice scalar count differs");
                return next;
            }
            auto all() const -> std::vector<fragment> override {
                std::vector<fragment> out;
                for (auto& [_, f] : pieces)
                    out.push_back(f);
                return out;
            }
            auto span(std::uint64_t offset, std::uint64_t count) const -> std::vector<fragment> override {
                std::vector<fragment> out;
                if (!count)
                    return out;
                auto [it, first] = pieces.at_mass(offset);
                for (; count; ++it, first = 0) {
                    if (it == pieces.end())
                        fail(nodes::code::storage_failure, "sequence region is incomplete");
                    auto taken = std::min(count, it->second.count - first);
                    out.push_back({it->second.run, it->second.first + first, taken});
                    count -= taken;
                }
                return out;
            }
            auto save() const -> value override {
                pieces.persist(piece_storage);
                anchors.persist(anchor_storage);
                auto root = [](const auto& index) -> value {
                    return value::map{
                        {"root", index.digest()}, {"object", index.object()}, {"count", static_cast<std::uint64_t>(index.size())}, {"mass", index.mass()}};
                };
                return value::map{{"pieces", root(pieces)}, {"anchors", root(anchors)}};
            }
        };
        void projection_reference(const std::string& type, const std::string& id, int delta);
        void payload_reference(const std::string& id, int delta);
        void projection_roots(const std::string& previous, const std::string& current);
        void collect_projection_garbage();
        auto sequence_from(const value& roots) -> std::shared_ptr<sequence_view>;
        auto sequence_for(const source_file& file) -> std::shared_ptr<const sequence_state>;
        auto retain_payload(std::string data) -> std::string;
        auto read_payload(const std::string& id) -> std::string;
        using projected_file = std::shared_ptr<const source_file>;
        using path_owners = std::shared_ptr<const std::set<std::string>>;
        struct visible_files {
            using item = projected_file;
            static constexpr std::string_view name = "files";
            static auto encode(impl& self, const projected_file& f) -> value { return codec::decode(self.file_bytes(*f)); }
            static auto decode(impl& self, const value& v) -> projected_file { return self.file_header(codec::encode(v), v.at("file").text()); }
            static auto hash(const projected_file& f) -> std::string { return f->metadata_digest(); }
        };
        struct hidden_files : visible_files {
            static constexpr std::string_view name = "hidden";
            static auto decode(impl& self, const value& v) -> projected_file {
                auto hidden = std::make_shared<source_file>(*visible_files::decode(self, v));
                hidden->present = false;
                return hidden;
            }
        };
        struct path_pages {
            using item = path_owners;
            static constexpr std::string_view name = "paths";
            static auto encode(impl&, const path_owners& owners) -> value { return strings({owners->begin(), owners->end()}); }
            static auto decode(impl&, const value& v) -> path_owners {
                auto ids = string_list(v);
                return std::make_shared<const std::set<std::string>>(ids.begin(), ids.end());
            }
            static auto hash(const path_owners& owners) -> std::string {
                return digest_of(codec::encode(value::map{{"ids", strings({owners->begin(), owners->end()})}}));
            }
        };
        struct conflict_pages {
            using item = std::vector<std::string>;
            static constexpr std::string_view name = "conflicts";
            static auto encode(impl&, const item& ids) -> value { return strings(ids); }
            static auto decode(impl&, const value& v) -> item { return string_list(v); }
            static auto hash(const item& ids) -> std::string { return digest_of(codec::encode(value::map{{"ids", strings(ids)}})); }
        };
        struct boolean_pages {
            using item = bool;
            static constexpr std::string_view name = "booleans";
            static auto encode(impl&, bool b) -> value { return b; }
            static auto decode(impl&, const value& v) -> bool { return v.boolean(); }
            static auto hash(bool b) -> std::string { return digest_of(codec::encode(value::map{{"value", b}})); }
        };
        std::shared_ptr<projection_index<visible_files>> visible_index;
        std::shared_ptr<projection_index<hidden_files>> hidden_index;
        std::shared_ptr<projection_index<path_pages>> path_index;
        std::shared_ptr<projection_index<conflict_pages>> conflict_index;
        std::shared_ptr<projection_index<boolean_pages>> boolean_index;
        std::uint64_t object_count = 0, object_watermark = 0, archive_bytes = 0, kernel_block = 0;
        std::string kernel_tail;
        struct saved_view {
            std::vector<std::string> frontier, conflicts;
            std::string admission, version, digest, files, projection;
        };
        std::map<std::string, saved_view> views;
        mutable std::set<std::string> pending_objects;
        mutable std::deque<std::string> new_candidates;
        std::deque<std::string> completed_objects, refused_objects;
        std::map<std::string, std::size_t> remaining_dependencies;
        std::map<std::string, std::vector<std::string>> waiting_dependencies;
        mutable std::set<std::string> dirty_status;
        std::set<std::string> dirty_claims, dirty_receipts, dirty_conflicts, dirty_roots, dirty_cursors;
        std::map<std::string, causal_set> checkpoint_cursors;

        static auto checked_root(std::filesystem::path path) -> std::filesystem::path;
        static auto load_key(const std::filesystem::path& file, archive::files& files) -> key;
        impl(std::filesystem::path path, std::string trust, bool audit, bool reading);
        void metadata(std::string_view name, std::string_view data);
        void trim_kernel();
        auto is_validated(const std::string& id) const -> bool;
        void queue_object(const std::string& id) const;
        void mark_validated(const std::string& id);
        void ensure_status();
        auto claims(const std::string& logical) -> std::set<std::string>&;
        auto receipts(const std::string& logical) -> std::vector<std::string>&;
        void trim_objects();
        auto put(value body, const key& signer) -> std::string;
        auto retain(value object) -> std::string;
        void persist(value object, const std::string& id, std::string_view data);
        auto blob(std::string bytes_value) -> std::string;
        auto get(const std::string& id, std::string_view kind = {}) const -> const value&;
        auto has_object(const std::string& id) const -> bool;
        auto object_bytes(const std::string& id) const -> std::string;
        auto object_ids(std::string_view kind = {}) const -> std::vector<std::string>;
        auto request_input(const std::string& id) -> const value&;
        struct id_less {
            auto operator()(const std::string* a, const std::string* b) const -> bool { return *a < *b; }
        };
        auto ref_ids(const value& b) const -> std::vector<const std::string*>;
        auto refs(const value& b) const -> std::vector<std::string>;
        void check_schema(const value& b) const;
        auto issuer(const std::string& id) const -> std::string;
        auto policy_before(const std::string& a, const std::string& b) const -> bool;
        void refresh_policies();
        auto allowed(const value& policy, std::string_view field, const std::string& who) const -> bool;
        auto protected_target(const value& policy, std::string_view worldline) const -> bool;
        auto line_of(const std::string& event) const -> std::string;
        // The line a root name stands for: main is the genesis line, every other name is bound by its creating line object.
        // A line takes its name only once an accepted fork created a root for it.
        auto lines() const -> const line_index&;
        auto registered_lines(const std::string& name) const -> const std::set<std::string>&;
        void register_line(const value& admission) const;
        // A local root keeps its name only while its line is the one every node resolves the name to.
        auto canonical(const std::string& name) const -> bool;
        auto worldlines() const -> std::map<std::string, std::vector<std::string>>;
        // Forks made apart under one name resolve to the smallest line identity on every node.
        auto named_line(const std::string& name) const -> std::string;
        auto shown_operation(const value& attempt) -> std::string;
        auto line_base(const std::string& line) const -> std::vector<std::string>;
        // An event belongs to a worldline when the line produced it or the line was forked after it.
        auto in_line(const std::string& line, const std::string& event) const -> bool;
        void within_line(const std::string& line, const std::vector<std::string>& heads) const;
        static auto selection(std::string_view action) -> bool;
        // A source acceptance adds one batch to the root: the frontier event outside the prior root that binds the accepted attempt.
        auto admitted_batch(const value& admission) const -> std::string;
        auto root_line(const std::string& name) const -> std::string;
        // An observation is a state of the line only when it contains the state the line was forked from.
        void observes_line(const std::string& line, const std::vector<std::string>& observed) const;
        void verify_policy(const std::string& id, const value& b) const;
        void validate_object(const std::string& id);
        auto missing(const std::vector<std::string>& initial) const -> std::vector<std::string>;
        void refuse(const std::string& id, const error& reason);
        auto validate_available() -> std::vector<std::string>;
        void load_objects(std::uint64_t after = 0);
        auto block_key(std::uint64_t index) const -> std::string;
        auto ordinal_key(std::uint64_t index) const -> std::string;
        // The kernel's view of this node's causal storage.
        struct causal_storage final : causal_reader {
            impl& self;
            explicit causal_storage(impl& self) : self(self) {}
            auto header(std::size_t index) -> value override {
                auto bytes = self.pages->get("q/header/" + self.ordinal_key(index));
                if (!bytes)
                    fail(nodes::code::missing_objects, "required causal proof is absent");
                ++self.causal_headers_read;
                return codec::decode(*bytes);
            }
            auto text(const std::string& id) -> std::string override {
                ++self.insertion_payloads_read;
                auto v = codec::decode(self.read_payload(id));
                fields(v, {"text"});
                return v.at("text").raw();
            }
            auto file_text(const source_file& file) -> std::string override {
                ++self.file_payloads_read;
                self.cached_payload_files.insert(file.id);
                self.cached_payload_bytes += file.stored_bytes;
                auto v = codec::decode(self.read_payload(file.content_object));
                fields(v, {"text"});
                auto text = v.at("text").raw();
                if (text.size() != file.stored_bytes || digest_of(text) != file.digest)
                    fail(nodes::code::storage_failure, "state file content differs");
                return text;
            }
            auto locate(const std::string& id) -> std::optional<std::size_t> override {
                auto bytes = self.pages->get("q/id/" + id);
                if (!bytes)
                    return {};
                return codec::decode(*bytes).at("index").number();
            }
            auto records(const std::string& kind, const std::string& key) -> std::vector<value> override {
                if (kind == "file") {
                    if (!self.pages->get("q/file_state/" + key))
                        fail(nodes::code::missing_objects, "required file causal index is absent: " + key);
                    ++self.file_histories_read;
                }
                std::vector<value> out;
                self.pages->each("q/" + kind + "/" + key + "/", [&](const std::string&, std::string bytes) {
                    out.push_back(codec::decode(bytes));
                    if (kind != "dependent" && kind != "identity")
                        ++self.causal_effects_read;
                });
                if (kind == "file") {
                    auto count = codec::decode(*self.pages->get("q/file_state/" + key)).at("count").number();
                    if (out.empty() || out.size() != count)
                        fail(nodes::code::missing_objects, "file causal membership is incomplete");
                }
                return out;
            }
            auto projection(const std::vector<std::string>& heads) -> std::shared_ptr<const source_state> override {
                if (auto name = self.matching_view(heads)) {
                    auto& saved = self.views.at(*name);
                    return self.restore_projection(saved.projection, saved.digest);
                }
                return {};
            }
            auto file_changed(const std::string& file, const causal_set& missing) -> bool override {
                for (auto [first, last] : missing.ranges)
                    for (auto kind : {"file", "file_control"}) {
                        auto prefix = "q/" + std::string(kind) + "/" + file + "/";
                        if (self.pages->contains_range(prefix + self.ordinal_key(first) + "/", prefix + self.ordinal_key(last) + "/"))
                            return true;
                    }
                return false;
            }
            auto base(const causal_set& wanted) -> std::pair<std::vector<std::string>, std::shared_ptr<const source_state>> override {
                const saved_view* closest = nullptr;
                std::uint64_t score = std::numeric_limits<std::uint64_t>::max();
                for (auto& [_, saved] : self.views) {
                    causal_set known;
                    auto v = codec::decode(saved.projection);
                    for (auto& range : v.at("causal").list())
                        known.ranges.emplace_back(range.at("first").number(), range.at("last").number());
                    auto delta = known.without(wanted);
                    delta.unite(wanted.without(known));
                    std::uint64_t size = 0;
                    for (auto [first, last] : delta.ranges)
                        size += last - first;
                    if (size < score) {
                        score = size;
                        closest = &saved;
                    }
                }
                if (!closest)
                    return {};
                return {closest->frontier, self.restore_projection(closest->projection, closest->digest)};
            }
        } causal{*this};
        void ensure_kernel();
        void persist_event(std::size_t index);
        void projection_stores();
        auto persist_projection(const source_state& state) -> std::string;
        auto restore_projection(const std::string& bytes, const std::string& expected_digest) -> std::shared_ptr<const source_state>;
        auto file_header(std::string_view encoded, const std::string& id) -> std::shared_ptr<const source_file>;
        auto file_bytes(const source_file& file) -> std::string;
        auto matching_view(const std::vector<std::string>& heads) -> std::optional<std::string>;
        auto ready_reply(const std::string& name, bool include_text, std::string_view path) -> node_reply;
        void persist_view(const std::string& name);
        void reset_state_storage();
        void save_snapshot();
        auto restore_snapshot() -> bool;
        void reload(bool audit = false);
        void refresh_external();
        auto version(const std::vector<std::string>& frontier) -> std::string;
        void root_at(const std::string& name, const std::vector<std::string>& frontier, std::string admission = {});
        auto identity() -> node_reply;
        auto view(const std::vector<std::string>& frontier, bool include_text = true, std::string_view path = {}) -> node_reply;
        auto compare(const node_request& in, const std::vector<std::string>& heads) -> node_reply;
        auto manifest_differences(const std::vector<std::string>& before_heads, const std::vector<std::string>& after_heads) -> std::vector<source_difference>;
        // The files that differ between two states, with their full contents on both sides.
        // A budget for shown contents: text up to `per_side` bytes a side within `total`, cut between characters; binary content not at all.
        struct shown {
            std::uint64_t per_side, total;
        };
        auto differences(const std::vector<std::string>& before_heads, const std::vector<std::string>& after_heads, std::optional<shown> budget = std::nullopt)
            -> std::vector<source_difference>;
        auto history(const node_request& in, const std::vector<std::string>& heads) -> node_reply;
        // The history entry of batch `id`, given the receipts that accepted its attempt.
        auto entry_of(const std::string& id, std::vector<std::string> proof) -> history_entry;
        // One batch as a contribution: its history entry, what it changed against its parents, and the worldlines that contain it.
        auto contribution(const node_request& in) -> node_reply;
        auto policy(std::string_view worldline = {}) -> std::string;
        auto target(const std::string& name) -> value;
        auto unresolved_identities(const std::vector<std::string>& heads, const std::string& p, const std::string& worldline) -> std::vector<std::string>;
        auto receipt(const std::string& id, bool replayed) -> node_reply;
        void binding(const std::string& operation, const std::string& digest, const std::string& result);
        // The receipt of `operation` as this node executed it for its author, if it ran.
        auto executed(const std::string& operation) -> std::optional<std::string>;
        auto retry(const node_request& in, const std::string& digest) -> std::optional<node_reply>;
        auto accept(const node_request& in, const std::string& line, const std::string& request_digest, const std::string& effect_digest,
                    const std::vector<std::string>& prior, const std::vector<std::string>& frontier, const std::string& result_id,
                    const std::string& attempt_id, bool override_author = false) -> node_reply;
        static void interrupt_at(std::string_view boundary);
        auto coordination(std::string_view name) -> std::optional<value>;
        auto execution_record(const std::string& token) -> value;
        void check_execution(const node_request& in);
        auto execution_reply(const value& record, bool secret) -> node_reply;
        void bind_task(const node_request& in);
        auto task_call(const node_request& in) -> node_reply;
        // Leases are local coordination metadata, never source history or cache identity.
        static auto lease_now() -> std::uint64_t;
        auto scope_leases() -> value::array;
        auto lease_range(const value& lease, const source_state& state) -> std::optional<semantic::place>;
        auto lease_reply(const value& lease, const source_state& state, bool reveal) -> scope_lease;
        auto scope_references() -> value::array;
        auto register_scopes(const node_request& in) -> node_reply;
        auto scope_call(const node_request& in) -> node_reply;
        void check_scope_leases(const node_request& in, const std::vector<std::string>& prior);

        auto promotion(const node_request& in, bool include_text = true) -> std::vector<source_difference>;

        auto effects(const node_request& in, const std::vector<std::string>& heads, const std::string& creating_author = {}) -> value::array;
        auto publish(const node_request& in) -> node_reply;
        auto resolve_revision(const std::string& line, const std::string& selection) -> std::string;
        auto reference_call(const node_request& in) -> node_reply;
        // Reads select a stored revision within the requested worldline.
        auto selected_heads(const node_request& in) -> std::vector<std::string>;
        static constexpr std::size_t content_parts = 10000;
        // Byte ranges of files of one version, named by the SHA-256 of their bytes; builds sync workers from these.
        auto content(const node_request& in) -> node_reply;
        auto mail(const node_request& in) -> node_reply;
        auto worldline_list() -> node_reply;
        // Limits hold for the state a batch publishes, concurrent changes joined: no file it grows or brings back exceeds the file limit,
        // and it may not grow a project beyond the project limit. Zero is no limit.
        template <class State> void check_limits(const node_request& in, const State& before, const State& after) {
            auto total = [](const State& state) {
                std::uint64_t n = 0;
                for (auto& [_, file] : state.files)
                    n += file->bytes();
                return n;
            };
            if (in.file_limit)
                for (auto& [id, file] : after.files)
                    if (auto size = file->bytes(); size > in.file_limit) {
                        auto was = before.files.find(id);
                        if (was == before.files.end() || was->second->bytes() < size)
                            fail(nodes::code::resource_limit, std::format("{} would exceed the {} byte file limit", file->path, in.file_limit));
                    }
            if (in.project_limit)
                if (auto bytes = total(after); bytes > in.project_limit && bytes > total(before))
                    fail(nodes::code::resource_limit, std::format("the project would exceed its {} byte limit", in.project_limit));
        }
        auto create_policy(const node_request& in) -> node_reply;
        auto inventory(const std::string& known_checkpoint = {}) -> node_reply;
        auto receive(const node_request& in, bool detailed = true, bool raw_parts = false, bool validate = true, bool snapshot = true) -> node_reply;
        auto checkpoint(const node_request& in) -> node_reply;
        auto recover() -> node_reply;
        void materialize(const std::string& name, const std::vector<std::string>& heads);
        void record_failure(const node_request& in, const error& failure);
    };
} // namespace nodes
