import std;
import boost.ut;
import support;
import delivery;
import protocol;
import build;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "exported bytes survive changes to the hot tree and support bounded reads"_test = [] {
            test::scratch tmp{"delivery"};
            const auto tree = tmp.dir / "tree", storage = tmp.dir / "exports";
            test::spit(tree / "dist/app", "first artifact");
            test::spit(tree / "dist/assets/data", "data");
            test::spit(tree / "empty", "");
            const auto files = delivery::capture(tree, {{"package", "dist"}, {"empty", "empty"}}, storage);
            expect(files.size() == 3u);
            const auto found = std::ranges::find(files, "app", &builds::exported_file::path);
            if (found == files.end())
                throw std::runtime_error("exported app missing");
            expect(found->name == "package" && found->size == 14u);
            expect(found->digest == content::hash_of("first artifact"));
            test::spit(tree / "dist/app", "replacement");
            const auto part = delivery::read(storage, {.digest = found->digest, .offset = 6, .length = 8}).parts.at(0);
            expect(part.total == 14u && part.offset == 6u && part.bytes.size() == 8u);
            expect(std::string(reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size()) == "artifact");
            expect(delivery::read(storage, {.digest = content::hash_of("")}).parts.at(0).bytes.empty());
            expect(throws<std::invalid_argument>([&] { delivery::read(storage, {.digest = found->digest, .offset = 15}); }));
            expect(throws<std::invalid_argument>([&] { delivery::read(storage, {.digest = found->digest, .length = jobs::part_size + 1}); }));
            expect(throws<std::invalid_argument>([&] { delivery::read(storage, {.digest = "../escape"}); }));
            const auto next = delivery::capture(tree, {{"package", "dist"}}, storage);
            expect(std::ranges::find(next, "app", &builds::exported_file::path)->digest != found->digest);
        };
        "received export parts are published only after digest verification"_test = [] {
            test::scratch tmp{"delivery-receive"};
            const auto digest = content::hash_of("abc");
            jobs::source_part first{.digest = digest, .total = 3, .bytes = {std::byte{'a'}}};
            delivery::accept(tmp.dir, first);
            expect(!std::filesystem::exists(tmp.dir / digest.view()));
            delivery::accept(tmp.dir, {.digest = digest, .offset = 1, .total = 3, .bytes = {std::byte{'b'}, std::byte{'c'}}});
            expect(delivery::read(tmp.dir, {.digest = digest}).parts.at(0).bytes.size() == 3u);
            const auto wrong = content::hash_of("wrong");
            expect(throws<std::invalid_argument>([&] { delivery::accept(tmp.dir, {.digest = wrong, .total = 1, .bytes = {std::byte{'x'}}}); }));
            expect(!std::filesystem::exists(tmp.dir / wrong.view()));
            expect(!std::filesystem::exists(tmp.dir / "partial" / wrong.view()));
        };
        "verification is separate from publication and active receivers survive collection"_test = [] {
            test::scratch tmp{"delivery-staging"};
            const auto digest = content::hash_of("abc");
            jobs::source_part part{.digest = digest, .total = 3, .bytes = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}}};
            expect(delivery::stage(tmp.dir, part));
            const auto pending = tmp.dir / "partial" / digest.view();
            std::filesystem::last_write_time(pending, std::filesystem::file_time_type::clock::now() - std::chrono::hours{48});
            delivery::collect(tmp.dir, std::chrono::hours{1}, 0, {digest.str()});
            expect(std::filesystem::exists(pending));
            delivery::verify(tmp.dir, digest, part.total);
            expect(!std::filesystem::exists(tmp.dir / digest.view()));
            delivery::publish(tmp.dir, digest);
            expect(!std::filesystem::exists(pending));
            expect(std::ranges::equal(delivery::read(tmp.dir, {.digest = digest}).parts.at(0).bytes, part.bytes));
        };
        "delivery collection expires old objects and protects recent transfers"_test = [] {
            test::scratch tmp{"delivery-retention"};
            const auto tree = tmp.dir / "tree", storage = tmp.dir / "exports";
            test::spit(tree / "old", "old");
            test::spit(tree / "recent", "recent");
            const auto files = delivery::capture(tree, {{"old", "old"}, {"recent", "recent"}}, storage);
            const auto old = storage / content::hash_of("old").view();
            const auto recent = storage / content::hash_of("recent").view();
            const auto ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours{24 * 8};
            std::filesystem::last_write_time(old, ago);
            test::spit(storage / "partial/interrupted", "partial");
            std::filesystem::last_write_time(storage / "partial/interrupted", ago);
            test::spit(storage / "unrelated.txt", "keep");
            delivery::collect(storage, std::chrono::hours{24 * 7}, 0);
            expect(!std::filesystem::exists(old));
            expect(std::filesystem::exists(recent)) << "active transfers survive size pressure";
            expect(!std::filesystem::exists(storage / "partial/interrupted"));
            expect(std::filesystem::exists(storage / "unrelated.txt"));
            std::filesystem::last_write_time(recent, std::filesystem::file_time_type::clock::now() - std::chrono::hours{1});
            delivery::collect(storage, std::chrono::hours{24 * 7}, 0);
            expect(!std::filesystem::exists(recent)) << "oldest eligible objects reduce cache size";
            delivery::capture(tree, {{"old", "old"}}, storage);
            std::filesystem::last_write_time(old, ago);
            delivery::capture(tree, {{"old", "old"}}, storage);
            delivery::collect(storage, std::chrono::hours{24 * 7}, 0);
            expect(std::filesystem::exists(old)) << "recapturing the same bytes renews their transfer protection";
        };
        "library directory exports keep regular binaries and report skipped links"_test = [] {
            test::scratch tmp{"delivery-libraries"};
            const auto tree = tmp.dir / "tree", storage = tmp.dir / "exports";
            test::spit(tree / "lib/libfoo.so.1.2", "library");
            test::spit(tree / "other", "other artifact");
            std::error_code ec;
            std::filesystem::create_symlink("libfoo.so.1.2", tree / "lib/libfoo.so.1", ec);
            if (ec)
                return;
            std::filesystem::create_symlink("libfoo.so.1", tree / "lib/libfoo.so");
            std::vector<std::string> skipped;
            auto files = delivery::capture(tree, {{"library", "lib"}, {"other", "other"}}, storage, false, &skipped);
            expect(files.size() == 2u);
            expect(std::ranges::find(files, "libfoo.so.1.2", &builds::exported_file::path) != files.end());
            expect(skipped.size() == 2u && std::ranges::contains(skipped, "lib/libfoo.so") && std::ranges::contains(skipped, "lib/libfoo.so.1"));
            expect(throws<std::invalid_argument>([&] { delivery::capture(tree, {{"library", "lib/libfoo.so"}}, storage); }));
        };
        "exports reject links and paths outside the working tree"_test = [] {
            test::scratch tmp{"delivery-paths"};
            const auto tree = tmp.dir / "tree", storage = tmp.dir / "exports";
            test::spit(tree / "good", "data");
            test::spit(tmp.dir / "secret", "private");
            expect(throws<std::invalid_argument>([&] { delivery::capture(tree, {{"bad", "../secret"}}, storage); }));
            expect(throws<std::invalid_argument>([&] { delivery::capture(tree, {{"../bad", "good"}}, storage); }));
            std::error_code ec;
            std::filesystem::create_symlink(tmp.dir / "secret", tree / "link", ec);
            if (!ec)
                expect(throws<std::invalid_argument>([&] { delivery::capture(tree, {{"bad", "link"}}, storage); }));
            for (const auto& item : std::filesystem::directory_iterator(storage))
                expect(!item.path().filename().string().starts_with("pending-"));
        };
    };
} // namespace
int main(int argc, const char** argv) {
    boost::ut::suite<"delivery">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
