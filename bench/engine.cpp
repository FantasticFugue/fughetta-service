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
        std::string mode, project, checkpoint, baseline_checkpoint, receipt, source_fingerprint, state_snapshot;
        std::vector<std::string> frontier;
        double seconds = 0, p95_ms = 0;
        std::uint64_t loaded_records = 0, replayed_events = 0, snapshot_events = 0, peak_rss_kib = 0, storage_bytes = 0, source_bytes = 0, files = 0,
                      batches = 0, transfer_bytes = 0;
        std::uint64_t projected_files = 0, state_nodes_read = 0, state_values_read = 0, state_nodes_written = 0, state_values_written = 0,
                      state_bytes_written = 0;
        bool causal_loaded = false;
        std::uint64_t causal_headers_read = 0, causal_effects_read = 0, file_histories_read = 0, file_payloads_read = 0, state_bytes_read = 0,
                      sequence_pages_read = 0, sequence_pages_written = 0, insertion_payloads_read = 0, cache_evictions = 0, state_bytes_decoded = 0,
                      causal_blocks_read = 0;
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
    auto operation(std::string name) -> std::string { return content::hash_of(name); }
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
    auto intent(std::uint64_t i) -> std::string { return repeated(content::hash_of("intent:" + std::to_string(i)), 1024); }
    auto result(std::uint64_t i) -> std::string { return repeated(content::hash_of("result:" + std::to_string(i)), 4096); }
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
        output.state_nodes_read = work.state_nodes_read;
        output.state_values_read = work.state_values_read;
        output.state_nodes_written = work.state_nodes_written;
        output.state_values_written = work.state_values_written;
        output.state_bytes_written = work.state_bytes_written;
        output.causal_loaded = work.causal_loaded;
        output.causal_headers_read = work.causal_headers_read;
        output.causal_effects_read = work.causal_effects_read;
        output.file_histories_read = work.file_histories_read;
        output.file_payloads_read = work.file_payloads_read;
        output.state_bytes_read = work.state_bytes_read;
        output.sequence_pages_read = work.sequence_pages_read;
        output.sequence_pages_written = work.sequence_pages_written;
        output.insertion_payloads_read = work.insertion_payloads_read;
        output.cache_evictions = work.cache_evictions;
        output.state_bytes_decoded = work.state_bytes_decoded;
        output.causal_blocks_read = work.causal_blocks_read;
        output.causal_scope = work.causal_scope;
    }
    void identities(const std::filesystem::path& root, nodes::local_node& a, nodes::local_node& b, nodes::local_node& c) {
        auto value = nodes::value::map{{"authors", nodes::strings({a.author_id(), b.author_id(), c.author_id()})},
                                       {"nodes", nodes::strings({a.node_id(), b.node_id(), c.node_id()})},
                                       {"project", a.project_id()}};
        std::ofstream out(root / "identities.dcn", std::ios::binary);
        auto encoded = nodes::codec::encode(value);
        out.write(encoded.data(), encoded.size());
        auto genesis = a.objects({a.project_id()});
        std::ofstream bytes(root / "genesis.dcn", std::ios::binary);
        auto data = nodes::unhex(genesis.front().data);
        bytes.write(data.data(), data.size());
    }
    auto work(const std::string& mode, const std::filesystem::path& root, const std::filesystem::path& inputs) -> measured {
        if (std::filesystem::exists(root / "a/repository.sqlite"))
            throw std::runtime_error("workload repository already exists; refusing to overwrite it");
        auto start = clock_type::now();
        nodes::local_node a{root / "a"};
        call(a, {.action = "initialize", .genesis_nonce = read(root / "nonce.hex")});
        nodes::local_node b{root / "b", a.project_id()}, c{root / "c", a.project_id()};
        call(a, {.action = "policy", .authors = {a.author_id(), b.author_id(), c.author_id()}, .nodes = {a.node_id(), b.node_id(), c.node_id()}});
        identities(root, a, b, c);
        measured output{.mode = mode};
        std::vector<double> timings;
        std::vector<std::string> head;
        std::vector<std::string> paths;
        std::array<nodes::key, 3> authors{nodes::key::from_secret(read(root / "a/author.key")), nodes::key::from_secret(read(root / "b/author.key")),
                                          nodes::key::from_secret(read(root / "c/author.key"))};
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
                a.use_author(authors[i % 3]);
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
                if (i == 499) {
                    a.use_author(authors[0]);
                    output.baseline_checkpoint = call(a, {.action = "checkpoint"}).checkpoint;
                    nodes::archive::output_file offline(root / "offline-base.dcn");
                    a.export_stream(offline.stream);
                    offline.finish();
                }
                if (i % 100 == 99)
                    std::cerr << "w1 " << i + 1 << "/1000 " << elapsed(start) << " s\n";
            }
            a.use_author(authors[0]);
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
                                                 .text = content::hash_of(std::to_string(i)).substr(0, 16),
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
        output.checkpoint = call(a, {.action = "checkpoint"}).checkpoint;
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
            throw std::invalid_argument("node-benchmark MODE REPOSITORY [INPUTS_OR_BUNDLE] [PROJECT] [RECEIPT]");
        std::string mode = argv[1];
        std::filesystem::path root = argv[2];
        measured output{.mode = mode};
        auto began = clock_type::now();
        if (mode == "w1" || mode == "w2")
            output = work(mode, root, argc > 3 ? argv[3] : "build/debug");
        else if (mode == "peer") {
            nodes::local_node node{root, argc > 3 ? argv[3] : ""};
            std::cout << "READY\n" << std::flush;
            std::string line;
            while (std::getline(std::cin, line)) {
                auto parsed = rfl::json::read<nodes::node_request, rfl::DefaultIfMissing, rfl::NoExtraFields>(line);
                if (!parsed)
                    throw std::runtime_error(parsed.error().what());
                auto& in = *parsed;
                if (!in.action.starts_with("benchmark_")) {
                    std::cout << rfl::json::write(call(node, std::move(in))) << '\n' << std::flush;
                    continue;
                }
                if (in.action == "benchmark_work") {
                    std::cout << rfl::json::write(node.statistics()) << '\n' << std::flush;
                    continue;
                }
                measured step{.mode = in.action};
                auto start = clock_type::now();
                if (in.action == "benchmark_export") {
                    nodes::archive::output_file stream(in.target);
                    node.export_stream(stream.stream, in.configuration);
                    stream.finish();
                    step.transfer_bytes = std::filesystem::file_size(in.target);
                } else if (in.action == "benchmark_import") {
                    nodes::archive::input_file stream(in.target);
                    auto reply = node.import_stream(stream.stream);
                    if (!reply.complete || !reply.refused.empty())
                        throw std::runtime_error("incomplete peer stream");
                } else if (in.action == "benchmark_offline") {
                    nodes::archive::input_file stream(in.target);
                    auto imported = node.import_stream(stream.stream);
                    std::vector<std::string> base;
                    for (auto& [name, heads] : imported.worldlines)
                        if (name.ends_with(":main"))
                            base = heads;
                    call(node, {.action = "fork", .operation = operation("w3-offline-fork"), .source = base});
                    auto state = call(node, {.action = "inspect"});
                    auto head = state.frontier;
                    std::ranges::sort(state.files, {}, &nodes::file_view::path);
                    std::vector<std::string> paths;
                    for (auto& f : state.files)
                        paths.push_back(f.path);
                    for (std::uint64_t i = 0; i < 100; ++i) {
                        auto source = call(node, {.action = "inspect"});
                        auto path = paths[i % paths.size()];
                        auto file = std::ranges::find(source.files, path, &nodes::file_view::path);
                        auto edited = call(node, {.action = "apply",
                                                  .operation = operation("w3-offline-" + std::to_string(i)),
                                                  .expected = head,
                                                  .edits = {{.kind = "edit",
                                                             .path = path,
                                                             .text = std::format("// offline-{:06}\n", i),
                                                             .offset = nodes::scalars(file->text).size()}},
                                                  .intent = intent(i),
                                                  .result = result(i),
                                                  .tool = "evaluation.edit/1"});
                        head = edited.frontier;
                        step.receipt = edited.receipt;
                    }
                    step.batches = 100;
                } else if (in.action != "benchmark_inspect")
                    throw std::runtime_error("unknown peer benchmark action");
                summarize(step, node, in.worldline, in.source);
                step.seconds = elapsed(start);
                step.storage_bytes = disk(root);
                step.peak_rss_kib = peak();
                std::cout << rfl::json::write(step) << '\n' << std::flush;
            }
            return 0;
        } else if (mode == "open" || mode == "audit") {
            nodes::local_node node{root, {}, mode == "audit"};
            summarize(output, node);
            output.seconds = elapsed(began);
            output.storage_bytes = disk(root);
            output.peak_rss_kib = peak();
        } else if (mode == "export") {
            nodes::local_node node{root};
            nodes::archive::output_file stream(argv[3]);
            auto reply = node.export_stream(stream.stream);
            stream.finish();
            output.project = reply.project;
            output.transfer_bytes = std::filesystem::file_size(argv[3]);
            output.seconds = elapsed(began);
            output.peak_rss_kib = peak();
        } else if (mode == "import") {
            nodes::local_node node{root, argv[4]};
            nodes::archive::input_file stream(argv[3]);
            auto reply = node.import_stream(stream.stream);
            if (!reply.complete)
                throw std::runtime_error("reconstruction is incomplete");
            std::vector<std::string> heads;
            for (auto& [name, frontier] : reply.worldlines)
                if (name.ends_with(":main"))
                    heads = frontier;
            auto fork = call(node, {.action = "fork", .worldline = "recovered", .operation = operation("reconstructed-fork"), .source = heads});
            call(node, {.action = "materialize", .worldline = "recovered"});
            auto receipt = call(node, {.action = "receipt", .target = argv[5]});
            if (receipt.issuer == node.node_id())
                throw std::runtime_error("reconstruction minted another execution receipt");
            summarize(output, node, "recovered");
            output.receipt = receipt.receipt;
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
