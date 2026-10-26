import std;
import boost.ut;
import content;
import storage;
import support;

namespace {
    using namespace boost::ut;

    // The checkout reader that a capture imports from: what a project directory holds, its ignore patterns applied.
    auto files(const test::scratch& tmp, std::vector<std::string> ignore = {}) -> storage::project_files {
        return storage::project_files{"app", tmp.dir, std::move(ignore)};
    }
    auto tracks(const storage::project_files& w, std::string_view path) -> bool {
        return std::ranges::contains(w.manifest().files, path, &content::entry::path);
    }

    static constexpr auto space_suite = [] {
        "blobs are served by hash and checked"_test = [] {
            test::scratch tmp{"space"};
            test::spit(tmp.dir / "a.txt", "first");
            auto w = files(tmp);
            auto part = w.blob_part(content::hash_of("first"), 1, 3);
            expect(std::string_view{reinterpret_cast<const char*>(part.data()), part.size()} == "irs");
            test::spit(tmp.dir / "a.txt", "second");
            w.rescan();
            expect(throws<storage::stale_blob>([&] { w.blob_part(content::hash_of("first"), 0, 5); }));
        };

        "ignored paths are not tracked"_test = [] {
            test::scratch tmp{"space"};
            test::spit(tmp.dir / ".git/HEAD", "ref");
            test::spit(tmp.dir / "build/out.o", "obj");
            test::spit(tmp.dir / "src/x.o", "obj");
            test::spit(tmp.dir / "src/x.c", "c");
            auto w = files(tmp, {".git", "build/", "*.o"});
            expect(w.manifest().files.size() == 1u && tracks(w, "src/x.c"));
            w.set_ignore({".git"});
            expect(tracks(w, "build/out.o") && tracks(w, "src/x.o"));
        };

        "symlinks are never followed"_test = [] {
            test::scratch tmp{"space"};
            test::scratch outside{"outside"};
            test::spit(outside.dir / "secret", "secret");
            std::filesystem::create_directories(tmp.dir / "src");
            std::filesystem::create_symlink(outside.dir / "secret", tmp.dir / "leak");
            std::filesystem::create_directory_symlink(outside.dir, tmp.dir / "src/out");
            auto w = files(tmp);
            expect(w.manifest().files.empty()) << "symlinks are not tracked";
        };

        "a rescan rehashes only what changed"_test = [] {
            test::scratch tmp{"space"};
            test::spit(tmp.dir / "a.txt", "one");
            auto w = files(tmp);
            test::spit(tmp.dir / "b.txt", "dropped in");
            expect(!tracks(w, "b.txt"));
            w.rescan();
            expect(tracks(w, "b.txt"));
            test::spit(tmp.dir / "a.txt", "two!");
            std::filesystem::remove(tmp.dir / "b.txt");
            w.rescan();
            expect(!tracks(w, "b.txt"));
            expect(w.manifest().files[0].digest == content::hash_of("two!"));
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"space">{space_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
