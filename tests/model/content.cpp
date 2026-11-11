import std;
import boost.ut;
import content;
import protocol;

namespace {
    using namespace boost::ut;

    static constexpr auto content_suite = [] {
        "sha256 matches the FIPS 180-4 vectors"_test = [] {
            expect(content::hash_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
            expect(content::hash_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
            expect(content::hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
            content::sha256 h;
            std::string chunk(997, 'a');
            std::size_t left = 1'000'000;
            while (left) {
                auto n = std::min(left, chunk.size());
                h.update(std::string_view{chunk}.substr(0, n));
                left -= n;
            }
            expect(content::to_hex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        };

        "only relative, normalized paths are accepted"_test = [] {
            expect(content::checked_path("src/main.cpp") == "src/main.cpp");
            expect(content::checked_path("a b/c.txt") == "a b/c.txt");
            for (auto bad : {"", "/etc/passwd", "../x", "a/../b", "a//b", "a/./b", "a/", "a\\b", "a\nb"})
                expect(throws<std::invalid_argument>([&] { content::checked_path(bad); })) << bad;
            expect(throws<std::invalid_argument>([] { content::checked_name("a/b"); }));
        };

        "globs and ignore rules"_test = [] {
            static_assert(content::glob("src/**/*.cpp", "src/a/b.cpp") && content::glob("src/**/*.cpp", "src/c.cpp"));
            static_assert(!content::glob("src/*.cpp", "src/a/b.cpp") && content::glob("*.md", "README.md"));
            static_assert(content::glob("a?c", "abc") && !content::glob("a?c", "ac"));
            expect(content::ignored(".git", ".git/HEAD") && content::ignored(".git", "sub/.git/x"));
            expect(content::ignored("*.o", "src/x.o") && !content::ignored("*.o", "src/x.c"));
            expect(content::ignored("build/", "build/out") && !content::ignored("build/", "build")) << "directories only";
            expect(content::ignored("src/gen/**", "src/gen/a/b.h") && !content::ignored("src/gen/**", "src/x.h"));
            expect(content::ignored("/out", "out/a") && !content::ignored("/out", "src/out/a")) << "anchored";
            expect(!content::ignored("build/", "") && !content::ignored("src/gen/**/", "")) << "an empty path matches nothing";
        };

        "a manifest's id follows its content"_test = [] {
            content::manifest a{{{.path = "x", .digest = content::hash_of("1"), .size = 1}}};
            auto b = a;
            expect(a.id() == b.id());
            b.files[0].executable = true;
            expect(a.id() != b.id());
            expect(content::is_hash(a.id()));
        };

        "sync_push round-trips through its binary codec"_test = [] {
            std::string text{"binary\0bytes", 12};
            auto bytes = std::as_bytes(std::span{text});
            jobs::source_bytes push{{{.digest = content::hash_of(bytes), .offset = 3, .total = 40, .bytes = {bytes.begin(), bytes.end()}}}};
            transport::frame f{.payload = transport::encode(push)};
            auto back = transport::decode<jobs::source_bytes>(f);
            expect(back.parts.size() == 1u);
            expect(back.parts[0].digest == push.parts[0].digest);
            expect(back.parts[0].offset == 3u && back.parts[0].total == 40u);
            expect(std::ranges::equal(back.parts[0].bytes, push.parts[0].bytes));
            f.payload.pop_back();
            expect(throws<std::length_error>([&] { transport::decode<jobs::source_bytes>(f); }));
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"content">{content_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
