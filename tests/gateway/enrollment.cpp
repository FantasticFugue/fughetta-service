import std;
import boost.ut;
import enrollment;
import durability;
import host;
import support;

namespace {
    using namespace boost::ut;
    auto status(auto action) -> int {
        try {
            action();
        } catch (const enrollment::refused& error) {
            return error.status;
        }
        return 0;
    }
    constexpr auto cases = [] {
        "registration persists hashed credentials and stable identities"_test = [] {
            test::scratch scratch{"worker-enrollment"};
            enrollment::credential first, second;
            {
                enrollment::store store{scratch.dir, "workspace-a"};
                first = store.add("same name", "owner", 101);
                second = store.add("same name", "owner", 103);
                expect(status([&] { store.add("", "owner", 104); }) == 400_i);
                expect(first.worker_id != second.worker_id);
                expect(status([&] { store.authenticate("workspace-b", first.worker_id, first.secret); }) == 401_i);
                expect(status([&] { store.authenticate("workspace-a", first.worker_id, "bad"); }) == 401_i);
                auto authenticated = store.authenticate("workspace-a", first.worker_id, first.secret, 110);
                expect(authenticated.has_value());
                store.rename(first.worker_id, "renamed");
                auto listed = store.list();
                expect(listed.size() == 2_u);
                auto disk = test::slurp(scratch.dir / "worker-registry" / "registry.json");
                expect(disk.find(first.secret) == std::string::npos);
                expect(disk.find(second.secret) == std::string::npos);
                store.revoke(first.worker_id, 120);
                expect(!store.active(first.worker_id));
                expect(store.active(second.worker_id));
            }
            auto backup = scratch.dir / "registry-backup.json";
            durability::durable_private_write(backup, test::slurp(scratch.dir / "worker-registry" / "registry.json"));
            test::scratch recovery{"worker-enrollment-recovery"};
            std::filesystem::create_directories(recovery.dir / "worker-registry");
            durability::durable_private_write(recovery.dir / "worker-registry" / "registry.json", test::slurp(backup));
            {
                enrollment::store restored{recovery.dir, "workspace-a"};
                expect(restored.list().size() == 2_u);
                expect(status([&] { restored.authenticate("workspace-a", first.worker_id, first.secret); }) == 401_i);
                expect(restored.authenticate("workspace-a", second.worker_id, second.secret).has_value());
            }
            expect(throws<std::runtime_error>([&] { enrollment::store wrong{recovery.dir, "workspace-b"}; }));
        };
        "concurrent registrations create distinct identities"_test = [] {
            test::scratch scratch{"worker-enrollment-race"};
            enrollment::store store{scratch.dir, "workspace-a"};
            std::vector<std::jthread> attempts;
            for (int i = 0; i < 8; ++i)
                attempts.emplace_back([&] { store.add("machine", "owner", 501); });
            attempts.clear();
            expect(store.list().size() == 8_u);
        };
        "credential files reject exposed permissions and symbolic links"_test = [] {
            test::scratch scratch{"worker-private-file"};
            auto path = scratch.dir / "credential.json";
            durability::durable_private_write(path, "secret");
            auto receive = [](const std::filesystem::path& file, std::string& read) {
                try {
                    for (auto bytes : durability::read_private(file, 64))
                        read.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                } catch (const durability::unreadable&) {
                    return false;
                }
                return true;
            };
            std::string read;
            expect(receive(path, read));
            expect(read == "secret");
            // Windows guards these files with ACLs, not mode bits or symlink checks.
            if (host::os == "windows")
                return;
            std::filesystem::permissions(path, std::filesystem::perms::group_read, std::filesystem::perm_options::add);
            expect(!receive(path, read));
            durability::durable_private_write(path, "new secret");
            expect(receive(path, read));
            auto link = scratch.dir / "link";
            std::filesystem::create_symlink(path, link);
            expect(!receive(link, read));
        };
    };
} // namespace
int main(int argc, const char** argv) {
    boost::ut::suite<"enrollment">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
