export module report;
import std;
import reflection;
export import provider;

// The report protocols a step can name. Each is a format its writers keep stable across their versions, so nothing here
// follows the text a tool prints for people.
export namespace builds {
    // JUnit XML, as CTest (--output-junit), pytest (--junit-xml) and most test runners write it: test suites, nested or under
    // a <testsuites> root, holding test cases; a case with a <failure> or an <error> failed. A case reported twice in one suite,
    // as pytest does for a test that fails and then errors in teardown, is counted once.
    template <> struct protocol<"junit"> {
        static auto read(std::string_view text) -> reading;
    };
    static_assert(parses<protocol<"junit">>);

    // Bazel's Build Event Protocol as JSON lines (--build_event_json_file): the testSummary event of every test target.
    template <> struct protocol<"bep"> {
        static auto read(std::string_view text) -> reading;
    };
    static_assert(parses<protocol<"bep">>);

    // GitLab Code Quality JSON, as ruff and ty write it (--output-format gitlab): an array of issues, each with a description,
    // a severity and a location. info stays info, minor is a warning and the rest are errors.
    template <> struct protocol<"codequality"> {
        static auto read(std::string_view text) -> reading;
    };
    static_assert(parses<protocol<"codequality">>);
} // namespace builds

namespace {
    struct junit_case {
        std::string name;
        std::string classname;
        std::vector<std::string> failure;
        std::vector<std::string> error;
    };
    struct junit_suite {
        std::string name;
        std::vector<junit_suite> testsuite;
        std::vector<junit_case> testcase;
    };

    struct summary_id {
        std::string label;
    };
    struct event_id {
        rfl::Rename<"testSummary", std::optional<summary_id>> summary;
    };
    struct summary {
        rfl::Rename<"overallStatus", std::string> status;
    };
    struct event {
        event_id id;
        rfl::Rename<"testSummary", std::optional<summary>> summary;
    };

    struct position {
        std::uint32_t line = 0;
        std::uint32_t column = 0;
    };
    struct span {
        position begin;
    };
    struct line_span {
        std::uint32_t begin = 0;
    };
    struct place {
        std::string path;
        std::optional<span> positions;
        std::optional<line_span> lines;
    };
    struct issue {
        std::string description;
        std::string check_name;
        std::string severity;
        place location;
    };

    // Cases are keyed by the path of suites around them, so same-named cases of different suites stay apart.
    void walk(const junit_suite& suite, const std::string& path, std::set<std::string>& seen, std::map<std::string, std::string>& failed) {
        auto here = path + suite.name + '\0';
        for (auto& inner : suite.testsuite)
            walk(inner, here, seen, failed);
        for (auto& c : suite.testcase) {
            auto name = c.classname.empty() || c.classname == c.name ? c.name : c.classname + "." + c.name;
            auto key = here + name;
            seen.insert(key);
            if (!c.failure.empty() || !c.error.empty())
                failed.emplace(std::move(key), std::move(name));
        }
    }
} // namespace

namespace builds {
    auto protocol<"junit">::read(std::string_view text) -> reading {
        auto suite = rfl::xml::read<junit_suite, rfl::DefaultIfMissing>(text);
        if (!suite)
            throw std::runtime_error(suite.error().what());
        std::set<std::string> seen;
        std::map<std::string, std::string> failed;
        walk(*suite, {}, seen, failed);
        tally tests{.total = static_cast<std::uint32_t>(seen.size()), .failed = static_cast<std::uint32_t>(failed.size())};
        for (auto& [key, name] : failed)
            tests.failed_tests.push_back(name);
        return {.tests = std::move(tests)};
    }

    auto protocol<"bep">::read(std::string_view text) -> reading {
        tally tests;
        for (auto part : text | std::views::split('\n')) {
            std::string_view line{part.begin(), part.end()};
            if (line.find_first_not_of(" \t\r") == std::string_view::npos)
                continue;
            auto e = rfl::json::read<event, rfl::DefaultIfMissing>(line);
            if (!e)
                throw std::runtime_error(e.error().what());
            if (!e->summary.get() || !e->id.summary.get())
                continue;
            ++tests.total;
            // An absent status is NO_STATUS, which proto3 leaves out of JSON.
            auto& status = e->summary.get()->status.get();
            if (status != "PASSED" && status != "FLAKY") {
                ++tests.failed;
                tests.failed_tests.push_back(e->id.summary.get()->label);
            }
        }
        return {.tests = std::move(tests)};
    }

    auto protocol<"codequality">::read(std::string_view text) -> reading {
        auto issues = rfl::json::read<std::vector<issue>, rfl::DefaultIfMissing>(text);
        if (!issues)
            throw std::runtime_error(issues.error().what());
        reading out;
        for (auto& i : *issues) {
            auto& at = i.location;
            out.diagnostics.push_back({.file = at.path,
                                       .line = at.positions ? at.positions->begin.line
                                               : at.lines   ? at.lines->begin
                                                            : 0u,
                                       .column = at.positions ? at.positions->begin.column : 0u,
                                       .severity = i.severity == "info"    ? "info"
                                                   : i.severity == "minor" ? "warning"
                                                                           : "error",
                                       .message = i.description.empty() ? i.check_name : i.description});
        }
        return out;
    }
} // namespace builds
