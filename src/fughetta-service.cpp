import std;
import gateway;
import source;
import worker;
import signals;

int application(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << "Usage: fughetta-service [--service] [--instance NAME] [--log-dir PATH] [config.json]\n"
                     "       fughetta-service token NAME owner|member [config.json]\n"
                     "       fughetta-service worker run --config PATH\n"
                     "Starts the gateway, source, and workers with the configured runtime budget.\n";
        return 0;
    }
    if (argc > 1 && std::string_view{argv[1]} == "worker")
        return worker_cli(argc - 1, argv + 1);
    if (argc > 2 && std::string_view{argv[1]} == "--role") {
        auto role = std::string_view{argv[2]};
        if (role == "source")
            return run_source(argc - 2, argv + 2);
        if (role == "worker")
            return run_worker(argc - 2, argv + 2);
        if (role == "gateway")
            return run_gateway(argc - 2, argv + 2, false);
        std::cerr << "unknown service role\n";
        return 1;
    }
    return run_gateway(argc, argv, true);
}

int main(int argc, char** argv) {
    bool service = false;
    std::string identity = "fughetta-service";
    std::ofstream output, errors;
    auto* old_output = std::cout.rdbuf();
    auto* old_errors = std::cerr.rdbuf();
    while (argc > 1) {
        std::string_view option{argv[1]};
        if (option == "--service") {
            service = true;
            --argc; ++argv;
        } else if ((option == "--instance" || option == "--log-dir") && argc > 2) {
            std::string_view value{argv[2]};
            if (option == "--instance") {
                if (value.empty() || value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != value.npos) {
                    std::cerr << "instance must contain only letters, digits, underscore or hyphen\n";
                    return 1;
                }
                identity = "fughetta-service-" + std::string{value};
            } else {
                auto directory = std::filesystem::path{value};
                output.open(directory / "service.out.log", std::ios::app);
                errors.open(directory / "service.err.log", std::ios::app);
                if (!output || !errors) {
                    std::cerr << "cannot open service log files\n";
                    return 1;
                }
            }
            argc -= 2; argv += 2;
        } else break;
    }
    if (output.is_open()) { std::cout.rdbuf(output.rdbuf()); std::cerr.rdbuf(errors.rdbuf()); }
    int result = 1;
    try {
        result = daemon::run(argc, argv, application, service, identity);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
    }
    std::cout.rdbuf(old_output);
    std::cerr.rdbuf(old_errors);
    return result;
}
