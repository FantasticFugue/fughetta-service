export module semantic;
import std;
import json;
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
        json::literal<"pending", "published", "expired"> state;
        std::optional<language::computation> computation;
        std::map<unit_id, std::vector<shard_id>> checkpoint;
        std::vector<shard_receipt> receipts;
        std::vector<artifact_receipt> artifact_receipts;
    };
    inline auto schemas() -> json::value {
        json::object out;
        out["request"] = json::schema<request>();
        out["result"] = json::schema<result>();
        out["shard"] = json::schema<shard>();
        out["view"] = json::schema<view>();
        out["helper"] = language::helper::schemas();
        return out;
    }
} // namespace semantic
