module indexing;
import std;
import json;
import profiles;
import content;

namespace semantic {
    namespace reanalysis {
        using input_key = std::tuple<input_kind, std::string, std::string>;
        struct read {
            dependency key;
            std::set<std::string> values;
            std::set<unit_id> producers;
        };
        struct unit {
            unit_key key;
            std::map<input_key, std::set<std::string>> inputs;
            std::map<std::string, read> reads;
            std::map<std::string, std::set<std::string>> channels;
        };
        auto changed_units(const indexed_view& before, const indexed_view& after) -> std::set<unit_id> {
            std::set<unit_id> out;
            auto append = [&](const auto& previous, const auto& current) {
                for (const auto& [id, shards] : previous) {
                    const auto next = current.find(id);
                    if (next == current.end() || next->second != shards)
                        out.insert(id);
                }
            };
            append(before.description.units, after.description.units);
            append(after.description.units, before.description.units);
            return out;
        }
        auto units(const indexed_view& view, const std::set<unit_id>& selected, const std::set<unit_id>& active) -> std::map<unit_id, unit> {
            std::map<unit_id, unit> out;
            for (const auto& id : selected) {
                const auto parts = view.unit_parts.find(id);
                if (parts == view.unit_parts.end())
                    continue;
                for (const auto part : parts->second) {
                    const auto& piece = *view.pieces.at(part);
                    auto& item = out[id];
                    item.key = piece.unit.recipe.unit;
                    for (const auto& channel : piece.unit.output_channels)
                        item.channels[channel.name].insert(channel.value);
                    if (!active.contains(id))
                        continue;
                    for (const auto& input : piece.unit.direct_inputs)
                        item.inputs[{input.kind, input.name, input.type}].insert(input.object);
                    for (std::size_t row = 0; row < piece.unit.observed_dependencies.size(); ++row) {
                        const auto& dependency = piece.unit.observed_dependencies[row];
                        // dependency_keys is the normalized lookup identity;
                        // output retains the actual observed read and provenance.
                        auto& read = item.reads[piece.index->dependency_keys.at(row)];
                        read.key = dependency;
                        if (read.key.project.empty())
                            read.key.project = item.key.project;
                        read.values.insert(dependency.value);
                        read.producers.insert(dependency.producers.begin(), dependency.producers.end());
                        if (dependency.producer)
                            read.producers.insert(*dependency.producer);
                        else
                            read.key.producers.assign(read.producers.begin(), read.producers.end());
                    }
                }
            }
            return out;
        }
    } // namespace reanalysis
    auto reanalysis_between(const indexed_view& before, const indexed_view& after) -> std::vector<reanalysis_change> {
        const auto changed = reanalysis::changed_units(before, after);
        auto active = changed;
        for (const auto* view : {&before, &after})
            for (const auto& producer : changed)
                if (const auto consumers = view->unit_consumers.find(producer); consumers != view->unit_consumers.end())
                    active.insert(consumers->second.begin(), consumers->second.end());
        auto selected = active;
        for (const auto* view : {&before, &after})
            for (const auto& id : active) {
                const auto parts = view->unit_parts.find(id);
                if (parts == view->unit_parts.end())
                    continue;
                for (const auto part : parts->second)
                    for (const auto& read : view->pieces.at(part)->unit.observed_dependencies) {
                        if (read.producer)
                            selected.insert(*read.producer);
                        selected.insert(read.producers.begin(), read.producers.end());
                    }
            }
        const auto old = reanalysis::units(before, selected, active), current = reanalysis::units(after, selected, active);
        std::vector<reanalysis_change> out;
        for (const auto& [id, previous] : old) {
            if (!active.contains(id))
                continue;
            const auto found = current.find(id);
            // A unit absent from the target view has no target computation to
            // reanalyze. Its removed declarations remain in the symbol diff.
            if (found == current.end())
                continue;
            const auto& next = found->second;
            reanalysis_change change{.id = id, .unit = next.key};
            std::set<reanalysis::input_key> inputs;
            for (const auto& [key, _] : previous.inputs)
                inputs.insert(key);
            for (const auto& [key, _] : next.inputs)
                inputs.insert(key);
            for (const auto& key : inputs) {
                const auto a = previous.inputs.find(key), b = next.inputs.find(key);
                const std::set<std::string> empty;
                const auto& old_values = a == previous.inputs.end() ? empty : a->second;
                const auto& new_values = b == next.inputs.end() ? empty : b->second;
                if (old_values != new_values)
                    change.inputs.push_back(
                        {std::get<0>(key), std::get<1>(key), std::get<2>(key), {old_values.begin(), old_values.end()}, {new_values.begin(), new_values.end()}});
            }
            std::set<std::string> reads;
            for (const auto& [key, _] : previous.reads)
                reads.insert(key);
            for (const auto& [key, _] : next.reads)
                reads.insert(key);
            for (const auto& key : reads) {
                const auto a = previous.reads.find(key), b = next.reads.find(key);
                const reanalysis::read empty;
                const auto& old_read = a == previous.reads.end() ? empty : a->second;
                const auto& new_read = b == next.reads.end() ? empty : b->second;
                const auto& read = a == previous.reads.end() ? new_read.key : old_read.key;
                auto values = new_read.values;
                bool unavailable = false;
                // Channel dependencies are checked against the target producer,
                // even when the consumer contribution was retained unchanged.
                if (read.producer && read.channel && a != previous.reads.end() && b != next.reads.end()) {
                    values.clear();
                    if (auto producer = current.find(*read.producer); producer != current.end()) {
                        if (auto channel = producer->second.channels.find(*read.channel); channel != producer->second.channels.end())
                            values = channel->second;
                        else
                            unavailable = true;
                    } else
                        unavailable = true;
                }
                if (read.kind == dependency_kind::record_set && read.channel && b != next.reads.end()) {
                    std::map<unit_id, std::string> channels;
                    for (const auto& id : new_read.producers) {
                        const auto producer = current.find(id);
                        if (producer == current.end()) {
                            unavailable = true;
                            continue;
                        }
                        const auto channel = producer->second.channels.find(*read.channel);
                        if (channel == producer->second.channels.end() || channel->second.size() != 1) {
                            unavailable = true;
                            continue;
                        }
                        channels.emplace(id, *channel->second.begin());
                    }
                    values.clear();
                    if (!unavailable)
                        values.insert(content::hash_of(json::write(channels)).str());
                }
                if (old_read.values == values && old_read.producers == new_read.producers && !unavailable)
                    continue;
                auto producers = old_read.producers;
                producers.insert(new_read.producers.begin(), new_read.producers.end());
                change.dependencies.push_back({read,
                                               {old_read.values.begin(), old_read.values.end()},
                                               {values.begin(), values.end()},
                                               {producers.begin(), producers.end()},
                                               unavailable});
            }
            if (change.inputs.empty() && change.dependencies.empty())
                continue;
            if (after.description.computation) {
                auto associate = [&](const auto& plan) {
                    if (plan.project != next.key.project)
                        return;
                    const auto stage = plan.profile.stages.find(next.key.stage);
                    if (stage == plan.profile.stages.end())
                        return;
                    std::set<std::string> configurations{stage->second.build_configuration};
                    for (const auto& [port, producers] : stage->second.inputs)
                        for (const auto& producer : producers)
                            if (const auto* build = std::get_if<language::build_input>(&producer))
                                configurations.insert(build->configuration);
                    for (const auto& name : configurations)
                        if (const auto build = plan.builds.find(name); build != plan.builds.end())
                            for (const auto& [operation, value] : build->second.operations)
                                if (operation == "build" || operation == "test")
                                    for (const auto& task : value.tasks)
                                        if (!std::ranges::any_of(change.tasks, [&](const auto& previous) {
                                                return previous.project == next.key.project && previous.configuration == name && previous.task == task &&
                                                       previous.operation == operation;
                                            }))
                                            change.tasks.push_back({next.key.project, name, task, operation});
                };
                associate(*after.description.computation);
                for (const auto& [selection, dependency] : after.description.computation->dependencies)
                    associate(dependency);
            }
            out.push_back(std::move(change));
        }
        return out;
    }
} // namespace semantic
