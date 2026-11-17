import std;
import boost.ut;
import configuration;
import backend;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "the execute container sees its root and caches writable and everything else read-only at host paths"_test = [] {
            config::worker cfg;
            cfg.backend = {.kind = "docker", .image = "image", .toolchains = {"/opt/llvm", "/opt/llvm/"}};
            cfg.dependency_cache = "/var/cache/vcpkg";
            cfg.dependency_upstream = "/srv/vcpkg";
            const auto mounts = backend::mounts(cfg, "/usr/local/bin/fughetta-service", "/work/front/execute/worker.json", "/work/front-execute");
            std::vector<std::pair<std::string, bool>> seen;
            for (const auto& m : mounts)
                seen.emplace_back(m.path.generic_string(), m.writable);
            expect(std::ranges::equal(seen, std::vector<std::pair<std::string, bool>>{{"/work/front-execute", true},
                                                                                      {"/var/cache/vcpkg", true},
                                                                                      {"/srv/vcpkg", false},
                                                                                      {"/usr/local/bin/fughetta-service", false},
                                                                                      {"/work/front/execute", false},
                                                                                      {"/opt/llvm", false}}));
        };

        "each backend runs the service as an execute Worker with the same mounts"_test = [] {
            const std::vector<backend::mount> mounts{{"/work/root", true}, {"/opt/llvm", false}};
            auto argv = [&](config::worker_backend b) {
                return backend::launch(b, "node", "/bin/fughetta-service", "/work/config/worker.json", "/work/root", mounts, "1000:1000");
            };
            const std::vector<std::string> tail{"/bin/fughetta-service", "--role", "worker", "/work/config/worker.json"};
            auto ends = [&](const std::vector<std::string>& got) {
                return got.size() > tail.size() && std::ranges::equal(got | std::views::drop(got.size() - tail.size()), tail);
            };
            auto docker = argv({.kind = "docker", .image = "image", .runtime = "runsc"});
            expect(std::ranges::equal(docker | std::views::take(8),
                                      std::vector<std::string>{"docker", "run", "--rm", "--init", "--name", "fughetta-node", "--network", "host"}));
            expect(std::ranges::contains(docker, "--runtime=runsc") && std::ranges::contains(docker, "--user=1000:1000"));
            expect(std::ranges::contains(docker, "--volume=/work/root:/work/root:rw") && std::ranges::contains(docker, "--volume=/opt/llvm:/opt/llvm:ro"));
            expect(ends(docker) && docker[docker.size() - tail.size() - 1] == "image");
            auto podman = argv({.kind = "docker", .program = "podman", .image = "image"});
            expect(podman.front() == "podman" && !std::ranges::any_of(podman, [](const auto& a) { return a.starts_with("--runtime"); }));
            auto ctr = argv({.kind = "containerd", .image = "image", .runtime = "kata"});
            expect(ctr.front() == "ctr" && std::ranges::contains(ctr, "--runtime=io.containerd.kata.v2") && std::ranges::contains(ctr, "--cwd=/work/root"));
            expect(std::ranges::contains(ctr, "--mount=type=bind,src=/opt/llvm,dst=/opt/llvm,options=rbind:ro"));
            expect(ends(ctr) && ctr[ctr.size() - tail.size() - 1] == "fughetta-node" && ctr[ctr.size() - tail.size() - 2] == "image");
            expect(std::ranges::contains(argv({.kind = "containerd", .image = "image", .runtime = "runsc"}), "--runtime=io.containerd.runsc.v1"));
            auto apple = argv({.kind = "apple", .image = "image"});
            expect(apple.front() == "container" && std::ranges::contains(apple, "--mount=type=bind,source=/opt/llvm,target=/opt/llvm,readonly"));
            expect(std::ranges::contains(apple, "--mount=type=bind,source=/work/root,target=/work/root") && ends(apple));
            expect(throws<std::invalid_argument>([&] { argv({}); }));
        };

        "a stale container of the same name is removed before a start"_test = [] {
            expect(backend::remove({}, "node").empty());
            expect(std::ranges::equal(backend::remove({.kind = "docker", .image = "image"}, "node"),
                                      std::vector<std::vector<std::string>>{{"docker", "rm", "--force", "fughetta-node"}}));
            expect(backend::remove({.kind = "containerd", .image = "image", .runtime = "runsc"}, "node").size() == 3_u);
            expect(std::ranges::equal(backend::remove({.kind = "apple", .image = "image"}, "node"),
                                      std::vector<std::vector<std::string>>{{"container", "delete", "--force", "fughetta-node"}}));
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"backend">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
