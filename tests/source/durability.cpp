import std;
import boost.ut;
import coroutine;
import native;
import content;
import durability;
import protocol;
import world;
import configuration;
import usage;
import storage;
import support;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = io::scheduler_of_t<native::context>;

    auto entry(std::string path, std::string_view text) -> content::entry {
        return {.path = std::move(path), .digest = content::hash_of(text), .size = text.size()};
    }
    auto blob(std::string_view text) -> jobs::source_part {
        auto bytes = std::as_bytes(std::span{text});
        return {content::hash_of(text).str(), 0, text.size(), {bytes.begin(), bytes.end()}};
    }

    static constexpr auto durability_suite = [] {
        "filesystem errors survive coroutine scope unwinding"_test = [] {
            test::scratch tmp{"missing"};
            native::context context;
            auto task = [&]() -> coroutine::task<> {
                auto run_children = [&](coroutine::scope&) -> coroutine::task<> {
                    std::filesystem::recursive_directory_iterator absent{tmp.dir / "absent"};
                    co_return;
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            expect(throws<std::filesystem::filesystem_error>([&] { cores::run(context, task()); }));
        };
        "mapped file views retain bytes and support empty files"_test = [] {
            test::scratch tmp{"mapping"};
            auto path = tmp.dir / "data";
            test::spit(path, "mapped bytes");
            {
                files::mapping mapped{path};
                expect(content::hash_of(mapped.bytes()) == content::hash_of("mapped bytes"));
                durability::durable_remove(path);
                expect(content::hash_of(mapped.bytes()) == content::hash_of("mapped bytes")) << "the view owns its mapping after the file is unlinked";
            }
            test::spit(path, "");
            files::mapping empty{path};
            expect(empty.bytes().empty());
            expect(throws<std::system_error>([&] { files::mapping missing{tmp.dir / "missing"}; }));
        };

        "a durable write replaces the file whole, and leaves no temporary"_test = [] {
            test::scratch tmp{"files"};
            auto f = tmp.dir / "a" / "b.txt";
            durability::durable_directories(f.parent_path());
            durability::durable_write(f, "one");
            durability::durable_write(f, "two", true);
            expect(test::slurp(f) == "two");
            expect(!std::filesystem::exists(tmp.dir / "a" / "b.txt.write-tmp"));
            durability::durable_append(tmp.dir / "log", "x\n");
            durability::durable_append(tmp.dir / "log", "y\n");
            expect(test::slurp(tmp.dir / "log") == "x\ny\n");
            expect(throws<std::system_error>([&] { durability::durable_write(tmp.dir / "missing-dir" / "f", "x"); })) << "a failure is reported, not dropped";
        };

        "one process per directory"_test = [] {
            test::scratch tmp{"files"};
            {
                durability::dir_lock held{tmp.dir, "storage"};
                expect(throws<durability::lock_busy>([&] { durability::dir_lock again{tmp.dir, "storage"}; }));
            }
            durability::dir_lock after{tmp.dir, "storage"};
        };

        "a scan that fails keeps the index; a full scan finds what size and mtime hide"_test = [] {
            test::scratch tmp{"space"};
            auto root = tmp.dir / "project";
            test::spit(root / "src/a.txt", "aaaa");
            test::spit(root / "b.txt", "b");
            storage::project_files w{"app", root, {}};
            std::filesystem::rename(root, tmp.dir / "saved");
            test::spit(root, "not a directory");
            bool threw = false;
            try {
                w.rescan();
            } catch (const std::system_error&) {
                threw = true;
            }
            std::filesystem::remove(root);
            std::filesystem::rename(tmp.dir / "saved", root);
            expect(threw) << "a failed directory scan is an error";
            expect(w.contains("src/a.txt")) << "not a smaller tree";

            auto when = std::filesystem::last_write_time(root / "src/a.txt");
            test::spit(root / "src/a.txt", "AAAA");
            std::filesystem::last_write_time(root / "src/a.txt", when);
            w.rescan();
            expect(test::slurp(root / "src/a.txt") == "AAAA" && w.manifest().files[1].digest == content::hash_of("aaaa"));
            w.rescan(true);
            expect(w.manifest().files[1].digest == content::hash_of("AAAA"));
        };

        "a ledger drops a torn last line, but keeps a damaged log aside"_test = [] {
            test::scratch tmp{"usage"};
            auto log = tmp.dir / "events.jsonl";
            auto good = std::format(R"({{"tool":{{"at":{},"project":"app","tool":"read"}}}})", usage::now());
            test::spit(log, good + "\n" + R"({"tool":{"at":)");
            {
                usage::ledger l{tmp.dir};
                expect(l.recovery().has_value() && l.recovery()->contains("torn")) << l.recovery().value_or("none");
                expect(l.counters().tools.at("read").calls == 1u);
            }
            test::spit(log, good + "\n" + "garbage in the middle\n" + good + "\n");
            {
                usage::ledger l{tmp.dir};
                expect(l.recovery().has_value() && l.recovery()->contains("1 unreadable line")) << l.recovery().value_or("none");
                expect(l.counters().tools.at("read").calls == 2u);
            }
            bool kept = false;
            for (auto& f : std::filesystem::directory_iterator(tmp.dir))
                if (f.path().filename().string().starts_with("events.jsonl.corrupt-"))
                    kept = test::slurp(f.path()).contains("garbage in the middle");
            expect(kept) << "the damaged log as it was found";
            {
                usage::ledger l{tmp.dir};
                expect(!l.recovery().has_value()) << "the rewritten log is clean";
            }
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"durability">{durability_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
