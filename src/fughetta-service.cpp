#include <argparse/argparse.hpp>
import std;
import gateway;
import source;
import worker;
import signals;
import hostrole;

int application(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << "Usage: fughetta-service [--service] [--instance NAME] [--log-dir PATH] [config.json]\n"
                     "       fughetta-service token NAME owner|member [config.json]\n"
                     "       fughetta-service worker run --config PATH\n"
                     "       fughetta-service public-cache-fill CONFIG DIRECTORY KEY [KEY ...]\n"
                     "       fughetta-service --role host [config.json]\n"
                     "Starts the gateway, source, and workers with the configured runtime budget.\n";
        return 0;
    }
    if (argc > 1 && std::string_view{argv[1]} == "public-cache-fill")
        return fill_public_cache(argc - 1, argv + 1);
    if (argc > 1 && std::string_view{argv[1]} == "worker")
        return worker_cli(argc - 1, argv + 1);
    if (argc > 2 && std::string_view{argv[1]} == "--role") {
        auto role = std::string_view{argv[2]};
        if (role == "source")
            return run_source(argc - 2, argv + 2);
        if (role == "worker")
            return run_worker(argc - 2, argv + 2);
        if (role == "host")
            return run_host(argc - 2, argv + 2);
        if (role == "gateway")
            return run_gateway(argc - 2, argv + 2, false);
        std::cerr << "unknown service role\n";
        return 1;
    }
    return run_gateway(argc, argv, true);
}

int main(int argc, char** argv) {
    argparse::ArgumentParser options{"fughetta-service", "", argparse::default_arguments::none};
    options.add_argument("--service").flag();
    options.add_argument("--instance");
    options.add_argument("--log-dir");
    std::string identity = "fughetta-service";
    std::ofstream output, errors;
    auto* old_output = std::cout.rdbuf();
    auto* old_errors = std::cerr.rdbuf();
    std::vector<std::string> arguments{argv[0]};
    try {
        auto remainder = options.parse_known_args(argc, argv);
        arguments.append_range(std::move(remainder));
        if (auto instance = options.present<std::string>("--instance")) {
            if (instance->empty() || instance->find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != instance->npos)
                throw std::invalid_argument("instance must contain only letters, digits, underscore or hyphen");
            identity += "-" + *instance;
        }
        if (auto log_dir = options.present<std::string>("--log-dir")) {
            const auto directory = std::filesystem::path{*log_dir};
            output.open(directory / "service.out.log", std::ios::app);
            errors.open(directory / "service.err.log", std::ios::app);
            if (!output || !errors)
                throw std::runtime_error("cannot open service log files");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::vector<char*> forwarded;
    for (auto& argument : arguments)
        forwarded.push_back(argument.data());
    forwarded.push_back(nullptr);
    if (output.is_open()) {
        std::cout.rdbuf(output.rdbuf());
        std::cerr.rdbuf(errors.rdbuf());
    }
    int result = 1;
    try {
        result = lifecycle::run(static_cast<int>(arguments.size()), forwarded.data(), application, options.get<bool>("--service"), identity);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
    }
    std::cout.rdbuf(old_output);
    std::cerr.rdbuf(old_errors);
    return result;
}
