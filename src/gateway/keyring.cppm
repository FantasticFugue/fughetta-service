export module keyring;
import std;
import content;
import configuration;
import world;

// Who sends a request to this core: the holder of a bearer token, or the caller a front on the private network names.
export namespace keyring {
    struct unauthorized : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    inline auto hash(std::string_view token) -> std::string { return content::hash_of(token).str(); }

    // A new bearer token: 32 random bytes in hex.
    inline auto mint() -> std::string {
        std::random_device random;
        std::string out;
        for (int i = 0; i < 32; ++i)
            out += std::format("{:02x}", random() & 0xff);
        return out;
    }

    // `header` reads a request header by its lowercase name and returns an optional string_view.
    template <class Header> auto authenticate(const config::auth& auth, Header header) -> worlds::actor {
        auto text = [&](std::string_view name) { return std::string{header(name).value_or("")}; };
        if (auth.mode == "trusted") {
            if (auth.workspace.empty() || text("x-fughetta-workspace") != auth.workspace)
                throw unauthorized("the front named a different workspace (x-fughetta-workspace)");
            worlds::actor who{.issuer = text("x-fughetta-issuer"), .user = text("x-fughetta-user"), .caller = text("x-fughetta-caller"),
                              .role = text("x-fughetta-role"), .email = text("x-fughetta-email")};
            if (who.user.empty())
                throw unauthorized("the front named no caller (x-fughetta-user)");
            if (!worlds::rank(who.role))
                throw unauthorized("x-fughetta-role must be member, owner or host");
            if (who.issuer.empty())
                who.issuer = "front";
            if (who.caller.empty())
                who.caller = who.user;
            if (auto client = text("x-fughetta-client"); !client.empty())
                who.client = std::move(client);
            return who;
        }
        auto value = header("authorization").value_or("");
        auto scheme = value.substr(0, std::min<std::size_t>(value.size(), 7));
        if (!std::ranges::equal(scheme, std::string_view{"bearer "}, {}, [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }))
            throw unauthorized("send a bearer token");
        auto digest = hash(value.substr(7));
        auto found = std::ranges::find(auth.tokens, digest, &config::token::hash);
        if (found == auth.tokens.end())
            throw unauthorized("unknown bearer token");
        return {.issuer = "token", .user = found->name, .caller = found->name, .role = found->role, .client = hash("client:" + digest)};
    }
} // namespace keyring
