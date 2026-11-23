import std;
import boost.ut;
import support;
import delivery;
import protocol;
import build;
import projectmodel;
import json;
import durability;
import registry;
import configuration;
import coroutine;
import native;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "a blocked eviction does not consume the budget reserved for newer objects"_test = [] {
            test::scratch tmp{"locked-budget"};
            const auto tree = tmp.dir / "tree", storage = tmp.dir / "exports";
            test::spit(tree / "large", std::string(4096, 'x'));
            test::spit(tree / "newer-one", "one");
            test::spit(tree / "newer-two", "two");
            auto files = delivery::capture(tree, {{"large", "large"}, {"one", "newer-one"}, {"two", "newer-two"}}, storage);
            const auto now = std::filesystem::file_time_type::clock::now();
            for (const auto& file : files)
                std::filesystem::last_write_time(storage / file.digest.view(), now - std::chrono::hours{file.name == "large" ? 2 : 1});
            const auto& locked = *std::ranges::find(files, "large", &builds::exported_file::name);
            const auto large = storage / locked.digest.view();
            std::ifstream held{large, std::ios::binary};
            expect(held.is_open());
            auto errors = delivery::collect(storage, std::chrono::hours{24 * 7}, 6);
            expect(errors.size() == 1_u && errors.front().contains(locked.digest.str()));
            for (const auto& file : files)
                expect(std::filesystem::exists(storage / file.digest.view()));
            held.close();
            expect(delivery::collect(storage, std::chrono::hours{24 * 7}, 6).empty());
            expect(!std::filesystem::exists(large));
            for (const auto& file : files)
                if (file.name != "large")
                    expect(std::filesystem::exists(storage / file.digest.view()));
        };
        "collection skips a locked model without releasing its objects and retries after close"_test = [] {
            test::scratch tmp{"locked-model"};
            const auto tree = tmp.dir / "tree", storage = tmp.dir / "exports";
            const auto hash = content::hash_of("library");
            projectmodel::document model{
                .producer = "test",
                .producer_version = "current",
                .units = {{.id = "main",
                           .project = "app",
                           .language = "kotlin",
                           .directory = ".",
                           .references = {projectmodel::artifact_reference{.object = {hash.str(), "library.jar"}, .role = "classpath"}}}}};
            test::spit(tree / "model.json", json::write(model));
            test::spit(tree / "library.jar", "library");
            test::spit(tree / "later", "later");
            const auto file = delivery::capture(tree, {{"model", "model.json"}}, storage).at(0);
            delivery::capture(tree, {{"library", "library.jar"}, {"later", "later"}}, storage);
            delivery::retain_model(storage, file.digest);
            const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours{48};
            for (const auto& id : {file.digest, hash, content::hash_of("later")})
                std::filesystem::last_write_time(storage / id.view(), old);
            std::filesystem::last_write_time(storage / file.digest.view(), old - std::chrono::hours{1});
            std::ifstream held{storage / file.digest.view(), std::ios::binary};
            expect(held.is_open());
            auto errors = delivery::collect(storage, std::chrono::seconds{0}, 0);
            expect(errors.size() == 1_u && errors.front().contains(file.digest.str()));
            expect(std::filesystem::exists(storage / file.digest.view()) && std::filesystem::exists(storage / hash.view()));
            expect(std::filesystem::exists(storage / "references" / file.digest.view()));
            expect(!std::filesystem::exists(storage / content::hash_of("later").view()));
            held.close();
            expect(delivery::collect(storage, std::chrono::seconds{0}, 0).empty());
            expect(!std::filesystem::exists(storage / file.digest.view()) && !std::filesystem::exists(storage / hash.view()));
        };
    };
} // namespace
int main(int argc, const char** argv) {
    boost::ut::suite<"delivery-windows">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
