module hostrole;
import std;

auto run_host(int, char**) -> int {
    std::cerr << "host role requires Linux\n";
    return 1;
}
