import std;
import reflection;
import engine;

void binary_streams();

int main(int argc, char** argv) {
    binary_streams();
    try {
        if (argc < 2) {
            std::cerr << "Usage: node REPOSITORY [TRUSTED_PROJECT_ID] [--audit]\nSend one JSON request per line on stdin.\n";
            return 2;
        }
        std::string trust, mode, known_checkpoint;
        bool audit = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--audit")
                audit = true;
            else if (arg == "--export" || arg == "--import")
                mode = arg;
            else if (arg == "--since" && i + 1 < argc)
                known_checkpoint = argv[++i];
            else if (trust.empty())
                trust = arg;
            else
                throw std::invalid_argument("unknown node argument");
        }
        nodes::local_node repository{argv[1], trust, audit};
        if (mode == "--export") {
            repository.export_stream(std::cout, known_checkpoint);
            return 0;
        }
        if (mode == "--import") {
            std::cout << rfl::json::write(repository.import_stream(std::cin)) << '\n';
            return 0;
        }
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.size() > 128 * 1024 * 1024) {
                std::cout << rfl::json::write(nodes::node_reply{.code = "resource_limit", .reason = "request exceeds 128 MiB"}) << '\n' << std::flush;
                continue;
            }
            auto request = rfl::json::read<nodes::node_request, rfl::DefaultIfMissing, rfl::NoExtraFields>(line);
            auto reply = request ? repository.call(*request) : nodes::node_reply{.code = "invalid_record", .reason = request.error().what()};
            std::cout << rfl::json::write(reply) << '\n' << std::flush;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "node: " << e.what() << '\n';
        return 1;
    }
}
