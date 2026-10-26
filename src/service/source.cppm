module;
#include <argparse/argparse.hpp>

export module source;
import std;
import runtime;
import coroutine;
import native;
import signals;
import configuration;
import server;

namespace {
    auto parse(int argc, char** argv) -> config::source {
        argparse::ArgumentParser program("source", "0.1.0");
        program.add_description("Hosted source file service.");
        program.add_argument("--bind")
            .default_value(std::string{"127.0.0.1"})
            .metavar("ADDRESS")
            .help("address of the internal listener");
        program.add_argument("--port")
            .default_value(9091)
            .scan<'i', int>()
            .metavar("PORT")
            .help("port of the internal listener (1-65535)");
        program.add_argument("--storage")
            .default_value(std::string{"storage"})
            .metavar("PATH")
            .help("durable source state directory");
        program.add_argument("--threads").default_value(static_cast<int>(allocation::plan{}.source_disk)).scan<'i', int>().help("assigned disk thread budget");
        program.add_argument("--tool")
            .append()
            .metavar("NAME=PATH")
            .help("analysis preparation executable");
        program.parse_args(argc, argv);

        auto port = program.get<int>("--port");
        if (port < 1 || port > 65535)
            throw std::invalid_argument("--port must be between 1 and 65535");
        auto threads = program.get<int>("--threads");
        if (threads < static_cast<int>(allocation::plan{}.source_disk))
            throw std::invalid_argument("source requires at least two disk threads");
        config::source cfg{.bind = program.get<std::string>("--bind"),
                           .port = static_cast<std::uint16_t>(port),
                           .storage = program.get<std::string>("--storage"), .disk_threads = static_cast<std::uint32_t>(threads)};
        if (auto tools = program.present<std::vector<std::string>>("--tool"))
            for (const auto& tool : *tools) {
                auto equal = tool.find('=');
                if (equal == std::string::npos || equal == 0 || equal + 1 == tool.size())
                    throw std::invalid_argument("--tool requires NAME=PATH");
                cfg.tools[tool.substr(0, equal)] = tool.substr(equal + 1);
            }
        config::validate(cfg);
        return cfg;
    }
} // namespace

export int run_source(int argc, char** argv) {
    try {
        auto cfg = parse(argc, argv);
        cores::io_context<runtime::context> context{1};
        cores::signal_watch signals{context};
        execution::thread_pool disk{cfg.disk_threads};
        source_server::service files{context, cfg.storage, disk, cfg.tools};
        context.run([&](auto& io, std::size_t core) -> coroutine::task<> {
            auto listener = native::listener{io, io::parse_endpoint(cfg.bind, cfg.port)};
            if (core == 0)
                logging::info(std::format("source: internal file service on {}:{}, storage {}, {} cores", cfg.bind, cfg.port, cfg.storage, context.size()));
            std::cerr << "service-ready:source\n";
            co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                for (;;)
                    children.spawn(files.serve(io, co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); }), core));
            });
        });
    } catch (const std::exception& e) {
        logging::error(std::format("source: {}", e.what()));
        return 1;
    }
    return 0;
}
