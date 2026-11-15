export module semantic;
import std;
import reflection;
import profiles;
export import records;

export namespace semantic {
    struct view {
        view_id id;
        snapshot_vector snapshots;
        std::string configuration;
        std::vector<input_object> effective_profile;
        query_defaults defaults;
        std::map<unit_id, std::vector<shard_id>> units;
        semantic::coverage coverage;
        rfl::Literal<"pending", "published", "expired"> state;
        std::optional<language::computation> computation;
        std::map<unit_id, std::vector<shard_id>> checkpoint;
    };
    inline auto schemas() -> rfl::Generic {
        rfl::Generic::Object out;
        out["request"] = rfl::json::read<rfl::Generic>(rfl::json::to_schema<request>()).value();
        out["result"] = rfl::json::read<rfl::Generic>(rfl::json::to_schema<result>()).value();
        out["shard"] = rfl::json::read<rfl::Generic>(rfl::json::to_schema<shard>()).value();
        out["view"] = rfl::json::read<rfl::Generic>(rfl::json::to_schema<view>()).value();
        out["helper"] = language::helper::schemas();
        return out;
    }
} // namespace semantic
