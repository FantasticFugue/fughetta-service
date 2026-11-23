import std;
import boost.ut;
import configuration;
import keyring;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "trusted authentication requires the core's immutable workspace identity for every role"_test = [] {
            config::auth auth{.mode = config::auth_mode::trusted, .workspace = "original-id", .front_token = "sk_front"};
            std::map<std::string, std::string, std::less<>> fields{{"x-fughetta-user", "new-owner"}, {"x-fughetta-issuer", "cloud"},
                {"authorization", "Bearer sk_front"}};
            auto header = [&](std::string_view name) -> std::optional<std::string_view> {
                auto found = fields.find(name);
                if (found == fields.end())
                    return std::nullopt;
                return found->second;
            };
            for (auto role : {"member", "owner", "host"}) {
                fields["x-fughetta-role"] = role;
                fields.erase("x-fughetta-workspace");
                expect(throws<keyring::unauthorized>([&] { keyring::authenticate(auth, header); }));
                fields["x-fughetta-workspace"] = "replacement-id";
                expect(throws<keyring::unauthorized>([&] { keyring::authenticate(auth, header); }));
                fields["x-fughetta-workspace"] = "original-id";
                auto who = keyring::authenticate(auth, header);
                expect(who.user == "new-owner" && who.role == role && who.issuer == "cloud");
            }
            auth.workspace.clear();
            expect(throws<keyring::unauthorized>([&] { keyring::authenticate(auth, header); }));
        };
        "trusted configuration requires a workspace identity and front token"_test = [] {
            expect(throws<std::invalid_argument>([] { config::parse(R"({"server":{"auth":{"mode":"trusted"}}})"); }));
            expect(throws<std::invalid_argument>([] { config::parse(R"({"server":{"auth":{"mode":"trusted","workspace":""}}})"); }));
            expect(throws<std::invalid_argument>([] { config::parse(R"({"server":{"auth":{"mode":"trusted","workspace":"original-id"}}})"); }));
            expect(throws<std::invalid_argument>([] {
                config::parse(R"({"server":{"auth":{"mode":"trusted","workspace":"original-id","front_token":""}}})");
            }));
            auto doc = config::parse(R"({"server":{"auth":{"mode":"trusted","workspace":"original-id","front_token":"sk_front"}}})");
            expect(doc.server.auth.workspace == "original-id" && doc.server.auth.front_token == "sk_front");
        };
    };
} // namespace

int main(int argc, const char** argv) {
    boost::ut::suite<"identity">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
