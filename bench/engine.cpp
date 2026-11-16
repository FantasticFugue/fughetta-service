#include <sys/resource.h>
import std;
import reflection;
import content;
import codec;
import engine;
import archive;

namespace {
    using clock_type = std::chrono::steady_clock;
    struct measured {
        std::string mode, project, receipt, source_fingerprint, state_snapshot;
        std::vector<std::string> frontier;
        double seconds = 0, p95_ms = 0;
        std::uint64_t loaded_records = 0, replayed_events = 0, snapshot_events = 0, peak_rss_kib = 0, storage_bytes = 0, source_bytes = 0, files = 0,
                      batches = 0, transfer_bytes = 0;
        std::uint64_t projected_files = 0, manifest_rows_read = 0, blob_reads = 0, manifest_rows_written = 0, blob_writes = 0, state_bytes_written = 0;
        bool causal_loaded = false;
        std::uint64_t causal_headers_read = 0, causal_effects_read = 0, file_histories_read = 0, file_payloads_read = 0, state_bytes_read = 0;
        std::uint64_t cache_evictions = 0, state_bytes_decoded = 0;
        std::string causal_scope;
    };
    struct input_file {
        std::string path;
        std::uint64_t bytes = 0;
        std::string sha256;
    };
    struct input_manifest {
        std::uint64_t schema = 1;
        std::vector<input_file> files;
    };
    auto elapsed(clock_type::time_point start) -> double { return std::chrono::duration<double>(clock_type::now() - start).count(); }
    auto operation(std::string name) -> std::string { return content::hash_of(name).str(); }
    auto call(nodes::local_node& node, nodes::node_request in) -> nodes::node_reply {
        if (in.owner.empty())
            in.owner = node.node_id();
        auto result = node.call(in);
        if (result.code != "ok")
            throw std::runtime_error(in.action + ": " + result.code + ": " + result.reason);
        return result;
    }
    auto read(const std::filesystem::path& file) -> std::string {
        nodes::archive::files io;
        return io.read(file);
    }
    auto disk(const std::filesystem::path& root) -> std::uint64_t {
        std::uint64_t bytes = 0;
        for (auto& entry : std::filesystem::recursive_directory_iterator(root))
            if (entry.is_regular_file())
                bytes += entry.file_size();
        return bytes;
    }
    auto peak() -> std::uint64_t {
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) != 0)
            throw std::runtime_error("getrusage failed");
        return usage.ru_maxrss;
    }
    auto repeated(std::string seed, std::size_t length) -> std::string {
        std::string out;
        out.reserve(length);
        while (out.size() < length)
            out += seed;
        out.resize(length);
        return out;
    }
    auto intent(std::uint64_t i) -> std::string { return repeated(content::hash_of("intent:" + std::to_string(i)).str(), 1024); }
    auto result(std::uint64_t i) -> std::string { return repeated(content::hash_of("result:" + std::to_string(i)).str(), 4096); }
    void summarize(measured& output, nodes::local_node& node, const std::string& worldline = "main", const std::vector<std::string>& heads = {}) {
        auto state = call(node, {.action = "inspect", .worldline = worldline, .source = heads});
        output.state_snapshot = state.state_snapshot;
        output.loaded_records = state.loaded_records;
        output.replayed_events = state.replayed_events;
        output.snapshot_events = state.snapshot_events;
        output.project = state.project;
        output.frontier = state.frontier;
        output.files = state.files.size();
        std::ranges::sort(state.files, {}, &nodes::file_view::path);
        content::sha256 digest;
        for (auto& f : state.files) {
            digest.update(f.path);
            digest.update(std::string_view{"\0", 1});
            digest.update(f.digest);
            digest.update(f.executable ? "1\n" : "0\n");
            output.source_bytes += f.size;
        }
        output.source_fingerprint = content::to_hex(digest.finish());
        auto work = node.statistics();
        output.projected_files = work.projected_files;
        output.manifest_rows_read = work.manifest_rows_read;
        output.blob_reads = work.blob_reads;
        output.manifest_rows_written = work.manifest_rows_written;
        output.blob_writes = work.blob_writes;
        output.state_bytes_written = work.state_bytes_written;
        output.causal_loaded = work.causal_loaded;
        output.causal_headers_read = work.causal_headers_read;
        output.causal_effects_read = work.causal_effects_read;
        output.file_histories_read = work.file_histories_read;
        output.file_payloads_read = work.file_payloads_read;
        output.state_bytes_read = work.state_bytes_read;
        output.cache_evictions = work.cache_evictions;
        output.state_bytes_decoded = work.state_bytes_decoded;
        output.causal_scope = work.causal_scope;
    }
    auto work(const std::string& mode, const std::filesystem::path& root, const std::filesystem::path& inputs) -> measured {
        if (std::filesystem::exists(root / "a/repository.sqlite"))
            throw std::runtime_error("workload repository already exists; refusing to overwrite it");
        auto start = clock_type::now();
        nodes::local_node a{root / "a"};
        call(a, {.action = "initialize", .genesis_nonce = read(root / "nonce.hex")});
        measured output{.mode = mode};
        std::vector<double> timings;
        std::vector<std::string> head;
        std::vector<std::string> paths;
        if (mode == "w1") {
            auto parsed = rfl::json::read<input_manifest>(read(inputs / "node-evaluation-project.json"));
            if (!parsed)
                throw std::runtime_error(parsed.error().what());
            std::vector<nodes::edit> initial;
            for (auto& file : parsed->files) {
                auto bytes = read(inputs / "node-evaluation-source" / file.path);
                if (bytes.size() != file.bytes || content::hash_of(bytes) != file.sha256)
                    throw std::runtime_error("frozen source input differs");
                paths.push_back(file.path);
                initial.push_back({.kind = "create", .path = file.path, .text = std::move(bytes)});
            }
            auto first = call(a, {.action = "apply", .operation = operation("w1-import"), .edits = std::move(initial), .tool = "evaluation.edit/1"});
            head = first.frontier;
            for (std::uint64_t i = 0; i < 1000; ++i) {
                auto began = clock_type::now();
                auto reply = call(a, {.action = "apply",
                                      .operation = operation("w1-batch-" + std::to_string(i)),
                                      .expected = head,
                                      .edits = {{.kind = "edit", .path = paths[i % paths.size()], .text = std::format("// dc-eval-{:06}\n", i)}},
                                      .intent = intent(i),
                                      .result = result(i),
                                      .tool = "evaluation.edit/1"});
                timings.push_back(elapsed(began) * 1000);
                head = reply.frontier;
                output.receipt = reply.receipt;
                if (i % 100 == 99)
                    std::cerr << "w1 " << i + 1 << "/1000 " << elapsed(start) << " s\n";
            }
        } else {
            auto bytes = repeated("0123456789abcdef\n", 4096);
            for (std::uint64_t batch = 0; batch < 100; ++batch) {
                std::vector<nodes::edit> initial;
                for (std::uint64_t j = 0; j < 100; ++j)
                    initial.push_back({.kind = "create", .path = std::format("scale/{:05}.txt", batch * 100 + j), .text = bytes});
                head = call(a, {.action = "apply",
                                .operation = operation("w2-import-" + std::to_string(batch)),
                                .expected = head,
                                .edits = std::move(initial),
                                .tool = "evaluation.edit/1"})
                           .frontier;
            }
            for (std::uint64_t i = 0; i < 10000; ++i) {
                auto began = clock_type::now();
                auto reply = call(a, {.action = "apply",
                                      .operation = operation("w2-batch-" + std::to_string(i)),
                                      .expected = head,
                                      .edits = {{.kind = "edit",
                                                 .path = std::format("scale/{:05}.txt", i % 100),
                                                 .text = content::hash_of(std::to_string(i)).str().substr(0, 16),
                                                 .count = 16}},
                                      .intent = intent(i),
                                      .result = result(i),
                                      .tool = "evaluation.edit/1"});
                timings.push_back(elapsed(began) * 1000);
                head = reply.frontier;
                output.receipt = reply.receipt;
                if (i % 100 == 99)
                    std::cerr << "w2 " << i + 1 << "/10000 " << elapsed(start) << " s\n";
            }
        }
        std::ofstream raw_timings(root / "latencies-ms.json");
        raw_timings << rfl::json::write(timings);
        raw_timings.close();
        summarize(output, a);
        output.seconds = elapsed(start);
        output.storage_bytes = disk(root / "a");
        output.peak_rss_kib = peak();
        output.batches = timings.size();
        std::ranges::sort(timings);
        output.p95_ms = timings[static_cast<std::size_t>(std::ceil(timings.size() * 0.95)) - 1];
        return output;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3)
            throw std::invalid_argument("node-benchmark MODE REPOSITORY [INPUTS]");
        std::string mode = argv[1];
        std::filesystem::path root = argv[2];
        measured output{.mode = mode};
        auto began = clock_type::now();
        if (mode == "w1" || mode == "w2")
            output = work(mode, root, argc > 3 ? argv[3] : "build/debug");
        else if (mode == "open" || mode == "audit") {
            nodes::local_node node{root, mode == "audit"};
            summarize(output, node);
            output.seconds = elapsed(began);
            output.storage_bytes = disk(root);
            output.peak_rss_kib = peak();
        } else
            throw std::invalid_argument("unknown benchmark mode");
        std::cout << rfl::json::write(output) << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "node-benchmark: " << e.what() << '\n';
        return 1;
    }
}
