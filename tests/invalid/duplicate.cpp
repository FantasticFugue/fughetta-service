import std;
import coroutine;
import mcp;
import tools;

struct first : tools::tool<"same", "One."> {
    struct input {};
    struct output {};
    static auto run(tools::services&, input, mcp::notifier&) -> coroutine::task<output> { co_return {}; }
};
struct second : tools::tool<"same", "Two."> {
    using input = first::input;
    using output = first::output;
    static auto run(tools::services&, input, mcp::notifier&) -> coroutine::task<output> { co_return {}; }
};

template struct tools::toolbox<first, second>;
