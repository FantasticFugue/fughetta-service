module indexing;
import std;
import reflection;

namespace semantic::querying {
    void impact(execution& self) {
        if (!self.request.selector)
            throw std::invalid_argument("impact requires a symbol selector");
        for (const auto& change : self.request.changes)
            if (change != "declaration" && change != "body")
                throw std::invalid_argument("impact change must be declaration or body");
        using key = execution::symbol_key;
        const auto matches = self.traverse(true);
        std::set<std::string> relations, references;
        for (const auto& row : self.out.relations)
            relations.insert(row.id);
        for (const auto& row : self.out.references)
            references.insert(row.id);
        auto endpoint = [&](const key& value) {
            const auto [begin, end] = self.view.symbols.equal_range(value.first);
            for (auto at = begin; at != end; ++at) {
                const auto& symbol = self.view.symbol(at->second);
                if (symbol.ownership.context != value.second || !self.selected.emplace(value, &symbol).second)
                    continue;
                self.out.items.push_back({.symbol_id = symbol.id,
                                          .context = value.second,
                                          .language = symbol.language,
                                          .domain = symbol.domain,
                                          .module = symbol.module,
                                          .name = symbol.name,
                                          .kind = symbol.kind,
                                          .type_id = symbol.type});
            }
        };
        for (const auto& value : matches) {
            impact_path path{.view = self.view.description.id,
                             .category = std::string{value.semantic ? "semantic" : "behavior"},
                             .seed = {value.seed.first, value.seed.second},
                             .target = {value.target.first, value.target.second},
                             .tentative = value.tentative};
            auto previous = value.seed;
            for (const auto& step : value.edges) {
                const auto* relation = step.reference ? nullptr : &self.view.relation(step.address);
                const auto* reference = step.reference ? &self.view.reference(step.address) : nullptr;
                path.steps.push_back({{previous.first, previous.second},
                                      {step.to.first, step.to.second},
                                      std::string{relation ? "relation" : "reference"},
                                      relation ? relation->id : reference->id});
                endpoint(step.to);
                previous = step.to;
                if (relation) {
                    const auto& row = *relation;
                    if (relations.insert(row.id).second)
                        self.out.relations.push_back(row);
                    if (row.reference)
                        self.origin_references.insert(*row.reference);
                    if (row.occurrence)
                        self.origin_occurrences.insert(*row.occurrence);
                } else if (references.insert(reference->id).second)
                    self.out.references.push_back(*reference);
            }
            self.out.impact.push_back(std::move(path));
        }
    }
} // namespace semantic::querying
