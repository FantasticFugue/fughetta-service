import std;
import boost.ut;
import environment;
import support;

namespace {
    using namespace boost::ut;
    static constexpr auto environment_suite = [] {
        "dotenv values are literal data"_test = [] {
            std::istringstream in{"\xef\xbb\xbf# comment\r\nexport A = plain # comment\r\nB='a # b'\nC=\"a\\n\\\"b\" # tail\nD=$(touch nope)\nE=${A}\nF=\n"};
            auto values = environment::parse(in);
            expect(values == std::vector<std::pair<std::string, std::string>>{
                                 {"A", "plain"}, {"B", "a # b"}, {"C", "a\n\"b"}, {"D", "$(touch nope)"}, {"E", "${A}"}, {"F", ""}});
        };
        "startup loading preserves the process environment"_test = [] {
            test::scratch scratch{"environment"};
            expect(!environment::load(scratch.dir / "missing.env"));
            auto key = "DOTENV_TEST_" + std::to_string(std::random_device{}());
            auto file = scratch.dir / ".env";
            test::spit(file, key + "=first\n");
            expect(environment::load(file));
            expect(std::string{std::getenv(key.c_str())} == "first");
            test::spit(file, key + "=second\n");
            expect(environment::load(file));
            expect(std::string{std::getenv(key.c_str())} == "first");
        };
        "invalid files do not partially apply or disclose secrets"_test = [] {
            test::scratch scratch{"environment-invalid"};
            auto key = "DOTENV_INVALID_" + std::to_string(std::random_device{}());
            auto file = scratch.dir / ".env";
            test::spit(file, key + "=private-value\nBROKEN='secret-without-end\n");
            try {
                environment::load(file);
                expect(false);
            } catch (const std::invalid_argument& e) {
                expect(std::string_view{e.what()} == "invalid .env assignment at line 2");
            }
            expect(std::getenv(key.c_str()) == nullptr);
            for (auto line : {"9BAD=value", "KEY='value' junk", "KEY", "KEY=\"unfinished"}) {
                std::istringstream in{line};
                expect(throws<std::invalid_argument>([&] { environment::parse(in); }));
            }
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"environment">{environment_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
