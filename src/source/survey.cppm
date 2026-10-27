export module survey;
import std;
import semantic;
import engine;

// Turns what a language server reported about a prepared tree into one semantic generation of a node version: places become the
// identities of source characters, so scopes, symbols and diagnostics follow later edits.
export namespace survey {
    // The generation of `version` on `worldline`: every place the outline names is anchored in that version.
    inline auto generation(nodes::local_node& node, const std::string& worldline, const std::string& version, std::string environment, std::string service,
                           semantic::outline found, const std::vector<std::string>& surveyed) -> semantic::generation {
        std::vector<semantic::span*> spans;
        for (auto& s : found.scopes)
            spans.push_back(&s.range);
        for (auto& s : found.symbols) {
            for (auto& d : s.declarations)
                spans.push_back(&d);
            for (auto& d : s.definitions)
                spans.push_back(&d);
        }
        for (auto& d : found.diagnostics)
            spans.push_back(&d.range);
        constexpr std::size_t batch = 10000;
        for (std::size_t first = 0; first < spans.size(); first += batch) {
            nodes::node_request ask{.action = "anchor", .worldline = worldline, .revision = version};
            for (auto i = first; i < std::min(spans.size(), first + batch); ++i)
                ask.spans.push_back(*spans[i]);
            auto anchored = node.call(ask);
            if (anchored.code != "ok")
                throw std::runtime_error("anchoring the survey failed: " + anchored.reason);
            if (anchored.version != version || anchored.spans.size() != ask.spans.size())
                throw std::runtime_error("the surveyed version moved while it was anchored");
            for (std::size_t i = 0; i < anchored.spans.size(); ++i)
                *spans[first + i] = std::move(anchored.spans[i]);
        }
        semantic::generation out{.version = version,
                                 .environment = std::move(environment),
                                 .service = std::move(service),
                                 .service_version = "1",
                                 .scopes = std::move(found.scopes),
                                 .symbols = std::move(found.symbols),
                                 .diagnostics = std::move(found.diagnostics)};
        // Every surveyed document is covered, with the digest of the text the server read.
        auto tree = node.call({.action = "tree", .worldline = worldline, .revision = version});
        if (tree.code != "ok" || tree.version != version)
            throw std::runtime_error("the surveyed version is no longer readable: " + tree.reason);
        for (auto& path : surveyed) {
            auto file = std::ranges::find(tree.files, path, &nodes::file_view::path);
            if (file == tree.files.end())
                throw std::runtime_error(path + " is not in the surveyed version");
            out.covered.push_back({.document = file->id, .digest = file->digest});
        }
        return out;
    }
} // namespace survey
