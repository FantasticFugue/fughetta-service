export module objects;
import std;
export import records;

export namespace semantic {
    // Borrows one immutable shard for a projection/evaluation. Preserve source
    // order for multiple occurrences; construction visits each record once.
    struct record_lookup {
        std::unordered_map<std::string_view, const records::type*> types;
        std::unordered_map<std::string_view, const identity_binding*> identities;
        std::unordered_map<std::string_view, std::vector<const records::occurrence*>> occurrences;

        explicit record_lookup(const shard& data) {
            types.reserve(data.types.size());
            identities.reserve(data.identities.size());
            occurrences.reserve(data.symbols.size());
            for (const auto& row : data.types)
                types.try_emplace(row.id, &row);
            for (const auto& row : data.identities)
                identities.try_emplace(row.symbol, &row);
            for (const auto& row : data.occurrences)
                occurrences[row.symbol].push_back(&row);
        }
        auto type(const type_id& id) const -> const records::type* {
            const auto found = types.find(id);
            return found == types.end() ? nullptr : found->second;
        }
        auto identity(const symbol_id& id) const -> const identity_binding* {
            const auto found = identities.find(id);
            return found == identities.end() ? nullptr : found->second;
        }
        auto positions(const symbol_id& id) const -> std::span<const records::occurrence* const> {
            const auto found = occurrences.find(id);
            return found == occurrences.end() ? std::span<const records::occurrence* const>{} : found->second;
        }
    };

    template <class T> class hot_cache {
        struct entry {
            std::shared_ptr<const T> value;
            std::uint64_t bytes;
            std::list<std::string>::iterator position;
        };

    public:
        auto find(const std::string& id) -> std::shared_ptr<const T> {
            const auto found = entries_.find(id);
            if (found == entries_.end())
                return {};
            recent_.splice(recent_.begin(), recent_, found->second.position);
            return found->second.value;
        }
        void put(const std::string& id, std::shared_ptr<const T> value, std::uint64_t bytes, std::uint64_t budget) {
            erase(id);
            if (bytes > budget)
                return;
            recent_.push_front(id);
            entries_.emplace(id, entry{std::move(value), bytes, recent_.begin()});
            bytes_ += bytes;
            while (bytes_ > budget)
                erase(recent_.back());
        }
        void erase(const std::string& id) {
            auto found = entries_.find(id);
            if (found == entries_.end())
                return;
            bytes_ -= found->second.bytes;
            recent_.erase(found->second.position);
            entries_.erase(found);
        }

    private:
        std::unordered_map<std::string, entry> entries_;
        std::list<std::string> recent_;
        std::uint64_t bytes_ = 0;
    };

    struct object_directory {
        std::filesystem::path path;
        ~object_directory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };
    struct shard_object {
        std::shared_ptr<const semantic::shard> data;
        std::shared_ptr<object_directory> directory;
        std::filesystem::path path;
        ~shard_object() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    };
} // namespace semantic
