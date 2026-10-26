import std;
import gateway;
import source;
import worker;

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << "Usage: fughetta-service [config.json]\n"
                     "       fughetta-service token NAME owner|member [config.json]\n"
                     "       fughetta-service worker join --gateway HTTPS_ORIGIN --workspace NAME --code CODE [--name NAME] [--config PATH] [--run]\n"
                     "       fughetta-service worker run --config PATH\n"
                     "Starts the gateway, source, and workers with the configured runtime budget.\n";
        return 0;
    }
    if (argc>1 && std::string_view{argv[1]}=="worker") return worker_cli(argc-1,argv+1);
    if (argc > 2 && std::string_view{argv[1]} == "--role") {
        auto role = std::string_view{argv[2]};
        if (role == "source") return run_source(argc - 2, argv + 2);
        if (role == "worker") return run_worker(argc - 2, argv + 2);
        if (role == "gateway") return run_gateway(argc - 2, argv + 2, false);
        std::cerr << "unknown service role\n";
        return 1;
    }
    return run_gateway(argc, argv, true);
}
