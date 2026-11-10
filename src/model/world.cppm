export module world;
import std;
export import content;

// Who is asking: the identity every source request names and every node write is attributed to.
export namespace worlds {
    struct actor {
        std::string issuer;
        std::string user;
        std::string caller;
        std::string role;
        std::string email;
        std::optional<std::string> client;
        // Set only after the source resolves a task execution credential.
        std::string run, task, project, worldline, execution_token;
    };

    // The caller as one string: issuer, user and caller name, each length-prefixed so no two callers share a key.
    inline auto identity_key(const actor& who) -> std::string {
        return std::to_string(who.issuer.size()) + ":" + who.issuer + std::to_string(who.user.size()) + ":" + who.user + std::to_string(who.caller.size()) +
               ":" + who.caller;
    }

    // Work queues and execution audit records distinguish concurrent runs of one caller.
    inline auto execution_key(const actor& who) -> std::string {
        auto key = identity_key(who);
        if (!who.run.empty()) key += "\nrun:" + who.run;
        return key;
    }

    // What a role may do, in increasing order: a member uses the workspace, an owner also administers it, and the host that runs this
    // core (a front on its private network) also sets the limits it grants. Zero is no role.
    inline auto rank(std::string_view role) -> int { return role == "member" ? 1 : role == "owner" ? 2 : role == "host" ? 3 : 0; }

    // A source-internal caller acting for a service of the system itself, not for a user.
    inline auto service_actor(std::string_view service) -> actor {
        return {.issuer = "source", .user = std::string{service}, .caller = std::string{service}, .role = "service"};
    }
} // namespace worlds
