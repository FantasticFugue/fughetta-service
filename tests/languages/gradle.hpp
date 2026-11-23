#pragma once

import std;
import launch;

namespace managed_test {
    inline void wrap_gradle(std::vector<std::string>& argv) {
#ifdef _WIN32
        auto launcher = std::filesystem::path{argv.front()};
        auto extension = launcher.extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension == ".bat" || extension == ".cmd") {
            const auto* shell = std::getenv("ComSpec");
            test::require(shell && *shell, "ComSpec is required for the Gradle batch launcher");
            argv.front() = launcher.make_preferred().string();
            argv.insert(argv.begin(), {shell, "/d", "/c", "call"});
        }
#else
        (void)argv;
#endif
    }
}
