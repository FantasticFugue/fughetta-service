import std;
import boost.ut;
import coroutine;
import native;
import http;
import protocol;
import publication;
import enrollment;
import knowledge;
import support;
import provider;
import json;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = io::scheduler_of_t<native::context>;
    using socket = native::socket;

    auto answer(socket& s, bool reply, bool fail) -> coroutine::task<> {
        transport::channel<socket> chan{s};
        for (;;) {
            auto f = co_await chan.recv();
            if (!reply)
                continue;
            auto job = transport::decode<jobs::hash_file>(f);
            if (fail) {
                co_await chan.fail(f.corr, "missing file", "not_found");
                continue;
            }
            co_await chan.reply(f.corr, jobs::digest{.hex = job.path.value()});
        }
    }

    auto call(ctx& io, bool reply, bool fail = false) -> coroutine::task<std::optional<std::string>> {
        auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        std::optional<std::string> out;
        auto s = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
        auto [peer, remote] = co_await net::async_accept(l);
        transport::channel<socket> chan{s};
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(answer(peer, reply, fail));
            children.spawn(chan.pump([](const transport::frame&) {}));
            if (reply) {
                auto d = co_await chan.call(jobs::hash_file{.path = "local/a.txt"});
                if (fail) {
                    expect(!d.has_value() >> fatal);
                    auto* error = std::get_if<jobs::failure>(&d.error());
                    expect((error != nullptr) >> fatal);
                    expect(error->code == "not_found");
                    out = error->message;
                } else {
                    expect(d.has_value() >> fatal);
                    out = d->hex;
                }
            } else {
                static constexpr auto run_child = [](transport::channel<socket>& chan) -> coroutine::task<> {
                    co_await chan.call(jobs::hash_file{.path = "local/b.txt"});
                    expect(false) << "an unanswered call returned";
                };
                children.spawn(run_child(chan));
                co_await execution::schedule_after(io, 5ms);
            }
        };
        co_await coroutine::with_scope(std::move(run_children));
        co_return out;
    }

    auto encode_frame(std::uint32_t kind, std::uint64_t corr, std::span<const std::byte> payload) -> std::vector<std::byte> {
        std::vector<std::byte> bytes(transport::header_size + payload.size());
        auto length = static_cast<std::uint32_t>(payload.size());
        std::memcpy(bytes.data(), &length, 4);
        std::memcpy(bytes.data() + 4, &kind, 4);
        std::memcpy(bytes.data() + 8, &corr, 8);
        std::ranges::copy(payload, bytes.begin() + transport::header_size);
        return bytes;
    }

    auto fragmented_frames(ctx& io, std::size_t chunk) -> coroutine::task<> {
        auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto writer = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
        auto [reader, remote] = co_await net::async_accept(listener);
        auto progress = transport::encode(jobs::progress{.done = 7, .total = 11});
        auto cancel = transport::encode(jobs::cancel{});
        auto bytes = encode_frame(jobs::code<jobs::progress>, 0xfedcba9876543210ULL, progress);
        auto next = encode_frame(jobs::code<jobs::cancel>, 42, cancel);
        bytes.insert(bytes.end(), next.begin(), next.end());
        transport::channel<socket> channel{reader};
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            static constexpr auto run_child = [](socket& writer, std::vector<std::byte>& bytes, std::size_t chunk) -> coroutine::task<> {
                for (std::size_t offset = 0; offset < bytes.size(); offset += chunk)
                    co_await io::async_write(writer, std::span{bytes}.subspan(offset, std::min(chunk, bytes.size() - offset)));
            };
            children.spawn(run_child(writer, bytes, chunk));
            auto first = co_await channel.recv();
            expect(first.kind == jobs::code<jobs::progress> && first.corr == 0xfedcba9876543210ULL);
            auto value = transport::decode<jobs::progress>(first);
            expect(value.done == 7_u && value.total == 11_u);
            auto second = co_await channel.recv();
            expect(second.kind == jobs::code<jobs::cancel> && second.corr == 42_u);
            expect(std::ranges::equal(second.payload, cancel));
        };
        co_await coroutine::with_scope(std::move(run_children));
    }

    auto frame_limit(ctx& io, bool oversized) -> coroutine::task<> {
        auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto writer = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
        auto [reader, remote] = co_await net::async_accept(listener);
        auto bytes = encode_frame(jobs::code<jobs::heartbeat>, 13, {});
        auto length = static_cast<std::uint32_t>(transport::max_payload + (oversized ? 1 : 0));
        std::memcpy(bytes.data(), &length, 4);
        if (!oversized)
            bytes.resize(transport::header_size + length, std::byte{0x5a});
        transport::channel<socket> channel{reader};
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            static constexpr auto run_child = [](socket& writer, std::vector<std::byte>& bytes) -> coroutine::task<> {
                co_await io::async_write(writer, bytes);
            };
            children.spawn(run_child(writer, bytes));
            bool rejected = false;
            try {
                auto frame = co_await channel.recv();
                expect(!oversized);
                expect(frame.payload.size() == transport::max_payload);
                expect(frame.corr == 13_u && frame.kind == jobs::code<jobs::heartbeat>);
                expect(std::ranges::all_of(frame.payload, [](std::byte b) { return b == std::byte{0x5a}; }));
            } catch (const std::length_error& error) {
                rejected = true;
                expect(std::string_view{error.what()} == "frame too large");
            }
            expect(rejected == oversized);
        };
        co_await coroutine::with_scope(std::move(run_children));
    }

    // A staged object can be complete JSON and still have no committed contribution.
    auto committed_calls(ctx& io) -> coroutine::task<> {
        auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto client = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
        auto [peer, remote] = co_await net::async_accept(listener);
        transport::channel<socket> channel{client}, server{peer};
        std::size_t delivered = 0;
        test::scratch scratch{"signed-unit"};
        enrollment::store registry{scratch.dir, "workspace"};
        const auto credential = registry.add("analysis Worker", "owner");
        const content::receipt_key key{credential.secret};
        auto send = [&]() -> coroutine::task<> {
            for (unsigned pass = 0; pass < 3; ++pass) {
                auto frame = co_await server.recv();
                if (pass == 2) {
                    co_await server.fail(frame.corr, "worker is full", "worker_overloaded");
                    continue;
                }
                const bool commit = pass == 1;
                auto request = transport::decode<jobs::analysis_request>(frame);
                language::evaluated_unit unit{
                    .session =
                        {.id = "session", .slot = request.analysis.slot_id, .project = "app", .stage = "python", .provider = "ty", .options = "options",
                         .snapshot = "source", .epoch = 1, .phase = std::string{"ready"}},
                    .request = {.execution = request.analysis.execution,
                                .slot_id = request.analysis.slot_id,
                                .unit = {"app", "python", "ty", "evaluate", semantic::unit_kind::file, "a.py"},
                                .epoch = 1},
                    .shards = {{.diagnostics = {{.id = "diagnostic", .severity = "warning", .message = "staged diagnostic"}}}}};
                semantic::recipe recipe{unit.request.unit, "options"};
                unit.shards.front().unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
                unit.shards.front().diagnostics.front().ownership = {
                    unit.shards.front().unit.id, unit.shards.front().unit.context, {.provider = "ty", .kind = std::string{"source"}}};
                auto receipt = jobs::shard_claim(unit, unit.shards.front(), credential.workspace_id, credential.worker_id);
                receipt.signature = key.sign(receipt.payload());
                unit.receipts.push_back(std::move(receipt));
                auto data = json::write(unit);
                auto bytes = std::as_bytes(std::span{data});
                co_await server.post(jobs::facts_batch{0, {bytes.begin(), bytes.end()}}, frame.corr);
                jobs::build_report report{.analysis_execution = request.analysis.execution};
                if (commit) {
                    co_await server.post(
                        jobs::unit_committed{request.analysis.execution, request.analysis.slot_id, unit.request.unit, 1, data.size(), jobs::inventory(unit)},
                        frame.corr);
                    report.committed_units = 1;
                    co_await server.post(jobs::analysis_completed{request.analysis.execution, 1}, frame.corr);
                } else {
                    report.analysis_error = "evaluation failed before commit";
                    co_await server.post(jobs::analysis_failed{request.analysis.execution, 0, "analysis_failed", *report.analysis_error}, frame.corr);
                }
                co_await server.reply(frame.corr, report);
            }
        };
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(send());
            children.spawn(channel.pump([](const transport::frame&) {}));
            jobs::analysis_request request{.project = "app",
                                           .tree = "work",
                                           .analysis =
                                               jobs::analysis_plan{.provider = "ty", .execution = {"first", "calculation", "python"}, .slot_id = "app/work"}};
            auto inputs = std::make_shared<jobs::analysis_exchange>(io, io);
            auto close = jobs::analysis_exchange::lifetime{inputs};
            auto receive = [&]() -> coroutine::task<> {
                while (auto item = co_await inputs->next()) {
                    auto unit = std::get<language::evaluated_unit>(item->data);
                    publishing::bound publication{.project = "app", .plan = request.analysis, .expected_snapshot = "source",
                                                  .signer = {&registry, credential.worker_id}};
                    publication.plan.options = "options";
                    publication.plan.registration.emplace();
                    publication.plan.registration->operations = {"evaluate"};
                    auto altered = unit;
                    altered.shards.front().diagnostics.front().message = "tampered";
                    expect(throws<tools::tool_error>([&] { publication.receive(std::move(altered)); }));
                    publication.receive(unit);
                    expect(publication.units->receipts.size() == 1_u);
                    const auto root = scratch.dir / "semantic";
                    semantic::view_id view;
                    {
                        semantic::store store{root};
                        view = store.publish_records({.snapshots = {{"app", "main", "v1", "source"}}, .configuration = "python"},
                                                     std::move(publication.units->shards), {}, {}, std::move(publication.units->receipts));
                    }
                    semantic::store restored{root};
                    const auto published = restored.describe(view);
                    expect((published.receipts.size() == 1_u) >> fatal);
                    expect(published.receipts.front().submitted.signature == unit.receipts.front().signature);
                    expect(published.receipts.front().published == published.units.begin()->second.front());
                    ++delivered;
                    expect(unit.shards.front().diagnostics.front().message == "staged diagnostic");
                    item->answer(std::monostate{});
                }
            };
            children.spawn(receive());
            auto failed = co_await channel.call(request, nullptr, inputs);
            expect(failed && failed->analysis_error && delivered == 0u) << "a failed staged contribution is never published";
            request.analysis.execution.request_id = "second";
            auto complete = co_await channel.call(request, nullptr, inputs);
            expect(complete && !complete->analysis_error && complete->committed_units == 1u && delivered == 1u);
            request.analysis.execution.request_id = "third";
            auto refused = co_await channel.call(request, nullptr, inputs);
            expect(!refused && std::get<jobs::failure>(refused.error()).code == "worker_overloaded" && delivered == 1u)
                << "admission refusal precedes any provider execution";
        };
        co_await coroutine::with_scope(std::move(run_children));
    }

    static constexpr auto edge_suite = [] {
        "jobs frames preserve signal kinds and correlation across arbitrary byte chunks"_test = [] {
            for (auto chunk : {1u, 7u, 16u, 4096u}) {
                native::context context;
                auto io = context.get_scheduler();
                expect(cores::run(context, fragmented_frames(io, chunk)));
            }
        };

        "jobs accepts exactly 64 MiB and rejects an oversized header without reading its body"_test = [] {
            for (bool oversized : {false, true}) {
                native::context context;
                auto io = context.get_scheduler();
                expect(cores::run(context, frame_limit(io, oversized)));
            }
        };

        "semantic publication waits for unit commit and consumes its terminal event"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(cores::run(context, committed_calls(io)));
        };

        "a call is answered through the pump"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            auto out = cores::run(context, call(io, true));
            expect(static_cast<bool>(out) >> fatal);
            expect(*out == std::optional<std::string>{"local/a.txt"});
        };

        "a remote failure is returned as a value"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            auto out = cores::run(context, call(io, true, true));
            expect(out.has_value() >> fatal);
            expect(*out == std::optional<std::string>{"missing file"});
        };

        "stopping the pump stops waiting calls"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            auto out = cores::run(context, call(io, false));
            expect(static_cast<bool>(out) >> fatal);
            expect(*out == std::nullopt);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"edge">{edge_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
