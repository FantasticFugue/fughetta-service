module;
#include <sqlite3.h>
module engine;
import :state;
import std;
import constants;
import content;
import codec;
import kernel;
import durability;
import archive;
import index;
import reflection;
import nodestore;

namespace nodes {
    using namespace persistence;
    auto local_node::impl::issuer(const std::string& id) const -> std::string {
        (void)get(id);
        return digest_of(objects.at(id).at("public_key").raw());
    }

    auto local_node::impl::policy_before(const std::string& a, const std::string& b) const -> bool {
        if (a == b)
            return true;
        std::set<std::string> seen;
        std::vector<std::string> work{b};
        while (!work.empty()) {
            auto id = std::move(work.back());
            work.pop_back();
            if (!seen.insert(id).second)
                continue;
            if (id == a)
                return true;
            auto& x = get(id);
            if (x.at("kind").text() == "policy")
                for (auto& p : string_list(x.at("previous")))
                    work.push_back(p);
        }
        return false;
    }

    void local_node::impl::refresh_policies() {
        std::vector<std::string> all;
        if (has_object(project))
            all.push_back(project);
        for (auto& id : object_ids("policy"))
            if (is_validated(id))
                all.push_back(id);
        policies.clear();
        for (auto& id : all)
            if (std::ranges::none_of(all, [&](auto& other) { return id != other && policy_before(id, other); }))
                policies.push_back(id);
        std::ranges::sort(policies);
    }

    auto local_node::impl::allowed(const value& policy, std::string_view field, const std::string& who) const -> bool {
        return std::ranges::any_of(policy.at(field).list(), [&](auto& identity) { return identity.text() == who; });
    }

    auto local_node::impl::protected_target(const value& policy, std::string_view worldline) const -> bool {
        for (auto& name : string_list(policy.at("protected")))
            if (name == worldline)
                return true;
        return false;
    }

    auto local_node::impl::line_of(const std::string& event) const -> std::string {
        auto& body = get(event);
        if (body.at("kind").text() != "batch")
            fail("worldline_boundary", "selected source is not a source batch");
        return body.at("line").text();
    }

    auto local_node::impl::lines() const -> const line_index& {
        if (!line_names) {
            line_names.emplace();
            for (auto& id : object_ids("admission"))
                if (is_validated(id))
                    register_line(get(id));
        }
        return *line_names;
    }

    auto local_node::impl::registered_lines(const std::string& name) const -> const std::set<std::string>& {
        static const std::set<std::string> none;
        auto found = lines().names.find(name);
        return found == lines().names.end() ? none : found->second;
    }

    void local_node::impl::register_line(const value& admission) const {
        if (!line_names)
            return;
        auto& line = admission.at("line").text();
        line_names->names[line == project ? std::string{"main"} : get(line, "line").at("name").text()].insert(line);
        if (!admission.at("frontier").list().empty())
            line_names->active.insert(line);
    }

    auto local_node::impl::canonical(const std::string& name) const -> bool {
        auto admission = root_admissions.find(name);
        if (admission == root_admissions.end() || !is_validated(admission->second))
            return admission == root_admissions.end();
        auto& line = get(admission->second, "admission").at("line").text();
        if (name == "main")
            return line == project;
        auto& found = registered_lines(name);
        return !found.empty() && *found.begin() == line;
    }

    auto local_node::impl::worldlines() const -> std::map<std::string, std::vector<std::string>> {
        std::map<std::string, std::vector<std::string>> out;
        for (auto& [name, frontier] : roots)
            if (canonical(name))
                out.emplace(name, frontier);
        return out;
    }

    auto local_node::impl::named_line(const std::string& name) const -> std::string {
        if (name == "main")
            return project;
        auto& found = registered_lines(name);
        if (found.empty())
            fail(nodes::code::invalid_record, "worldline not found");
        return *found.begin();
    }

    auto local_node::impl::shown_operation(const value& attempt) -> std::string {
        auto& input = request_input(attempt.at("input").text());
        auto& caller = input.at("caller_operation").text();
        return caller.empty() ? attempt.at("operation").text() : caller;
    }

    auto local_node::impl::line_base(const std::string& line) const -> std::vector<std::string> {
        return line == project ? std::vector<std::string>{} : string_list(get(line, "line").at("base"));
    }

    auto local_node::impl::in_line(const std::string& line, const std::string& event) const -> bool {
        return line_of(event) == line || source.includes(line_base(line), event);
    }

    void local_node::impl::within_line(const std::string& line, const std::vector<std::string>& heads) const {
        for (auto& head : heads)
            if (!in_line(line, head))
                fail("worldline_boundary", "selected source belongs to another worldline");
    }

    auto local_node::impl::selection(std::string_view action) -> bool { return action == "fork" || action == "integrate" || action == "admit"; }

    auto local_node::impl::admitted_batch(const value& admission) const -> std::string {
        auto prior = string_list(admission.at("prior"));
        for (auto& event : string_list(admission.at("frontier")))
            if (get(event).at("kind").text() == "batch" && get(event).at("attempt") == admission.at("attempt") && !source.includes(prior, event))
                return event;
        fail(nodes::code::invalid_record, "acceptance does not name its executed batch");
    }

    auto local_node::impl::root_line(const std::string& name) const -> std::string {
        auto admission = root_admissions.find(name);
        if (admission == root_admissions.end())
            return named_line(name);
        auto line = get(admission->second, "admission").at("line").text();
        if (line != named_line(name))
            fail("worldline_boundary", "an earlier fork owns this worldline name; fork this state under another name");
        return line;
    }

    void local_node::impl::observes_line(const std::string& line, const std::vector<std::string>& observed) const {
        within_line(line, observed);
        for (auto& event : line_base(line))
            if (!source.includes(observed, event))
                fail("stale_frontier", "observed source does not contain the worldline base");
    }

    void local_node::impl::verify_policy(const std::string& id, const value& b) const {
        auto prev = string_list(b.at("previous"));
        if (prev.empty() || prev != vector_set(prev))
            fail(nodes::code::invalid_record, "invalid policy predecessors");
        std::uint64_t epoch = 0;
        auto signer = issuer(id);
        for (auto& p : prev) {
            auto& old = get(p);
            auto kind = old.at("kind").text();
            if (kind != "genesis" && kind != "policy")
                fail(nodes::code::invalid_record, "invalid policy reference");
            if (!allowed(old, "maintainers", signer))
                fail(nodes::code::policy_denied, "policy signer is not a predecessor maintainer");
            auto maintainers = string_list(old.at("maintainers"));
            if (maintainers.size() != 1)
                fail(nodes::code::policy_denied, "policy requires every predecessor maintainer signature");
            if (kind == "policy")
                epoch = std::max(epoch, old.at("epoch").number());
        }
        if (epoch == std::numeric_limits<std::uint64_t>::max() || b.at("epoch").number() != epoch + 1)
            fail(nodes::code::invalid_record, "invalid policy epoch");
        if (string_list(b.at("maintainers")).empty())
            fail(nodes::code::invalid_record, "policy has no maintainer");
        for (auto& field : {"authors", "nodes", "maintainers"})
            for (auto& who : string_list(b.at(field)))
                if (!content::is_hash(who))
                    fail(nodes::code::invalid_record, "invalid policy identity");
        for (auto& name : string_list(b.at("protected")))
            checked_name(name);
        for (auto& a : string_list(b.at("settled")))
            get(a, "admission");
    }

    void local_node::impl::validate_object(const std::string& id) {
        if (is_validated(id))
            return;
        auto& b = get(id);
        auto kind = b.at("kind").text();
        if (kind == "genesis") {
            if (id != project || b.at("nonce").raw().size() != 32 || b.at("founding_key").raw() != objects.at(id).at("public_key").raw() ||
                !allowed(b, "maintainers", issuer(id)))
                fail("wrong_project", "invalid trusted genesis");
            mark_validated(id);
            return;
        }
        if (b.at("project").text() != project)
            fail("wrong_project", "object is bound to another project");
        for (auto* r : ref_ids(b))
            if (!is_validated(*r))
                get(*r);
        if (kind == "policy")
            verify_policy(id, b);
        else if (kind == "batch") {
            if (issuer(id) != b.at("author").text())
                fail("invalid_signature", "batch author does not match signature");
            if (b.at("parents") != b.at("expected"))
                fail(nodes::code::invalid_record, "batch base does not match parent frontier");
            auto digest = effects_digest(b.at("effects").list());
            if (digest != b.at("effect_digest").text())
                fail(nodes::code::invalid_record, "effect digest mismatch");
            get(b.at("policy").text());
            get(b.at("intent").text(), "intent");
            auto& attempt = get(b.at("attempt").text(), "attempt");
            if (attempt.at("author") != b.at("author") || attempt.at("operation") != b.at("operation") || attempt.at("intent") != b.at("intent") ||
                digest_of(get(attempt.at("input").text(), "blob").at("data").raw()) != b.at("request_digest").text())
                fail(nodes::code::invalid_record, "batch does not bind its signed attempt");
            auto request = decode_request(request_input(attempt.at("input").text()), get(attempt.at("intent").text(), "intent").at("summary").text(),
                                          get(attempt.at("result").text(), "blob").at("data").raw());
            for (auto& effect : b.at("effects").list())
                if (effect.at("type").text() == "binary") {
                    auto digest = effect.at("value").text();
                    bool declared = false;
                    for (auto& required : string_list(b.at("required"))) {
                        auto& object = get(required);
                        if (object.at("kind").text() == "blob" && digest_of(object.at("data").raw()) == digest) {
                            source.add_blob(digest, object.at("data").raw());
                            declared = true;
                            break;
                        }
                    }
                    if (!declared)
                        fail(nodes::code::invalid_record, "binary effect omits its required content object");
                }
            auto observed_effects = effects(request, request.expected, b.at("author").text());
            if (request.expected != string_list(b.at("parents")) ||
                source.dependencies(request.expected, observed_effects, request.dependencies) != string_list(b.at("dependencies")) ||
                observed_effects != b.at("effects").list())
                fail(nodes::code::invalid_record, "source effects differ from the observed signed request");
            auto line = b.at("line").text();
            if (line != project)
                get(line, "line");
            observes_line(line, string_list(b.at("parents")));
            source.add(id, b);
        } else if (kind == "line") {
            if (issuer(id) != b.at("creator").text())
                fail("invalid_signature", "worldline creator differs from signature");
            if (b.at("name").text() == "main")
                fail("worldline_boundary", "main is the genesis worldline");
            std::set<std::string> lines;
            for (auto& event : string_list(b.at("base")))
                lines.insert(line_of(event));
            if (lines.size() > 1)
                fail("worldline_boundary", "worldline base spans several worldlines");
        } else if (kind == "admission") {
            auto& p = get(b.at("policy").text());
            bool protected_line = protected_target(p, b.at("target").at("name").text());
            if (issuer(id) != b.at("issuer").text() || (protected_line && !allowed(p, "nodes", issuer(id))))
                fail(nodes::code::policy_denied, "unauthorized accepting node");
            auto& attempt = get(b.at("attempt").text(), "attempt");
            auto& input = request_input(attempt.at("input").text());
            if (attempt.at("executor") != b.at("issuer") || attempt.at("operation") != b.at("operation") || attempt.at("result") != b.at("result") ||
                b.at("target").at("owner") != b.at("issuer") || b.at("target").at("reviewer") != attempt.at("author") || input.at("owner") != b.at("issuer") ||
                input.at("project") != b.at("project") || input.at("worldline") != b.at("target").at("name") ||
                (selection(input.at("action").text()) && input.at("expected") != b.at("prior")))
                fail(nodes::code::invalid_record, "acceptance does not bind its signed request");
            if (protected_line && !allowed(p, "authors", attempt.at("author").text()))
                fail(nodes::code::policy_denied, "accepting request author is unauthorized");
            auto same_target = [&](const value& prior) {
                return prior.at("target").at("owner") == b.at("target").at("owner") && prior.at("target").at("name") == b.at("target").at("name");
            };
            if (!std::holds_alternative<std::monostate>(b.at("previous").data)) {
                auto& prior = get(b.at("previous").text(), "admission");
                if (!same_target(prior) || prior.at("frontier") != b.at("prior"))
                    fail(nodes::code::invalid_record, "previous acceptance does not bind the prior worldline root");
            } else if (!b.at("prior").list().empty())
                fail(nodes::code::invalid_record, "nonempty prior source requires its retained acceptance");
            std::vector<std::string> covered;
            for (auto& dependency : string_list(b.at("accepted_dependencies"))) {
                auto& prior = get(dependency, "admission");
                if (prior.at("policy") != b.at("policy") || !same_target(prior))
                    fail(nodes::code::policy_denied, "admission dependency is outside this target and policy");
                auto selected = string_list(prior.at("frontier"));
                covered.insert(covered.end(), selected.begin(), selected.end());
            }
            auto action = input.at("action").text();
            auto accepted = string_list(b.at("frontier"));
            auto expected = string_list(b.at("prior"));
            auto line = b.at("line").text();
            if (line == project ? b.at("target").at("name").text() != "main" : get(line, "line").at("name") != b.at("target").at("name"))
                fail("worldline_boundary", "acceptance binds a worldline name to another worldline");
            bool first = std::holds_alternative<std::monostate>(b.at("previous").data);
            if (action == "fork") {
                auto& created = get(line, "line");
                if (!first || !expected.empty() || string_list(created.at("base")) != vector_set(accepted) || created.at("operation") != b.at("operation") ||
                    created.at("creator") != attempt.at("author"))
                    fail(nodes::code::invalid_record, "fork acceptance does not bind its created worldline");
            } else if (!first && get(b.at("previous").text(), "admission").at("line") != b.at("line"))
                fail("worldline_boundary", "acceptance changes the worldline of its root");
            if (action == "fork" || action == "integrate" || action == "admit") {
                auto selected = string_list(input.at("source"));
                if (action != "fork") {
                    selected.insert(selected.end(), expected.begin(), expected.end());
                    observes_line(line, accepted);
                }
                if (source.frontier(std::move(selected)) != accepted)
                    fail(nodes::code::invalid_record, "acceptance selects source outside the signed request");
            } else {
                auto event = admitted_batch(b);
                auto& batch = get(event, "batch");
                auto grown = expected;
                grown.push_back(event);
                if (batch.at("operation") != b.at("operation") || batch.at("line") != b.at("line") || source.includes(expected, event) ||
                    source.frontier(std::move(grown)) != accepted)
                    fail(nodes::code::invalid_record, "acceptance does not bind the executed batch");
                auto parents = string_list(batch.at("parents"));
                if (parents.empty() && !expected.empty())
                    fail(nodes::code::invalid_record, "batch observed no state of the accepted worldline");
                for (auto& parent : parents)
                    if (!source.includes(expected, parent))
                        fail(nodes::code::invalid_record, "batch observed source outside the accepted worldline");
            }
            if (b.at("author_override").boolean()) {
                auto& review = get(b.at("review").text(), "review");
                if (!allowed(p, "maintainers", review.at("author").text()) || issuer(b.at("review").text()) != review.at("author").text() ||
                    review.at("policy") != b.at("policy") || review.at("frontier") != b.at("frontier") || review.at("target") != b.at("target"))
                    fail(nodes::code::policy_denied, "override lacks a signed maintainer decision");
            } else if (!std::holds_alternative<std::monostate>(b.at("review").data))
                fail(nodes::code::invalid_record, "unexpected override decision");
            if (protected_line && !b.at("author_override").boolean())
                for (auto& event : source.uncovered(string_list(b.at("frontier")), covered)) {
                    if (!allowed(p, "authors", source.event_author(event))) {
                        bool settled = false;
                        for (auto& prior : string_list(p.at("kind").text() == "policy" ? p.at("settled") : value{value::array{}})) {
                            auto accepted = source.reachable(string_list(get(prior, "admission").at("frontier")));
                            if (std::ranges::find(accepted, event) != accepted.end())
                                settled = true;
                        }
                        if (!settled)
                            fail(nodes::code::policy_denied, "admission contains an unauthorized author");
                    }
                }
            register_line(b);
        } else if (kind == "attempt") {
            if (issuer(id) != b.at("author").text())
                fail("invalid_signature", "attempt author differs from signature");
            auto& input = request_input(b.at("input").text());
            if (input.at("operation") != b.at("operation") || input.at("owner") != b.at("executor") || input.at("project") != b.at("project"))
                fail(nodes::code::invalid_record, "attempt input identity mismatch");
            (void)decode_request(input, get(b.at("intent").text(), "intent").at("summary").text(), get(b.at("result").text(), "blob").at("data").raw());
            static const std::set<std::string> actions{"apply", "promote", "fork", "integrate", "admit", "reverse", "redo", "resolve"};
            if (!actions.contains(input.at("action").text()) || b.at("tool").text() != tool_name(input.at("tool").text(), input.at("action").text()) ||
                b.at("outcome").text() != "accepted" || !content::is_hash(b.at("operation").text()) || !content::is_hash(b.at("executor").text()) ||
                get(b.at("intent").text(), "intent").at("author") != b.at("author"))
                fail(nodes::code::invalid_record, "invalid source tool attempt definition");
            auto logical = b.at("author").text() + ":" + b.at("operation").text();
            auto& claims = this->claims(logical);
            dirty_claims.insert(logical);
            claims.insert(digest_of(get(b.at("input").text(), "blob").at("data").raw()));
            dirty_conflicts.insert(logical);
            if (claims.size() > 1) {
                auto ids = std::vector<std::string>{claims.begin(), claims.end()};
                ids.push_back(logical);
                identity_conflicts[logical] =
                    "identity:" + digest_of(codec::encode(value::map{{"effects", strings(vector_set(std::move(ids)))}, {"type", "identity"}}));
            }
        } else if (kind == "intent") {
            if (issuer(id) != b.at("author").text())
                fail("invalid_signature", "intent author differs from signature");
        } else if (kind == "review") {
            if (issuer(id) != b.at("author").text() || b.at("target").at("reviewer") != b.at("author") ||
                !allowed(get(b.at("policy").text()), "maintainers", issuer(id)))
                fail(nodes::code::policy_denied, "invalid maintainer review");
        } else if (kind == "failure") {
            if (issuer(id) != b.at("author").text() || !content::is_hash(b.at("operation").text()))
                fail(nodes::code::invalid_record, "invalid failed attempt identity");
            auto raw = get(b.at("input").text(), "blob").at("data").raw();
            if (digest_of(raw) != b.at("request_digest").text())
                fail(nodes::code::invalid_record, "failed attempt input mismatch");
            (void)decode_request(codec::decode(raw), get(b.at("intent").text(), "blob").at("data").raw(), get(b.at("result").text(), "blob").at("data").raw());
        } else if (kind == "receipt") {
            auto& a = get(b.at("admission").text(), "admission");
            if (issuer(id) != b.at("issuer").text() || b.at("issuer") != a.at("issuer") || b.at("operation") != a.at("operation") ||
                b.at("target") != a.at("target") || b.at("result") != a.at("result"))
                fail(nodes::code::invalid_record, "receipt does not bind original acceptance");
            auto& attempt = get(a.at("attempt").text(), "attempt");
            if (b.at("author") != attempt.at("author") || digest_of(get(attempt.at("input").text(), "blob").at("data").raw()) != b.at("request_digest").text())
                fail(nodes::code::invalid_record, "receipt request digest mismatch");
            auto action = request_input(attempt.at("input").text()).at("action").text();
            auto expected_digest = selection(action) ? digest_of(codec::encode(value::map{{"frontier", a.at("frontier")}, {"prior", a.at("prior")}}))
                                                     : get(admitted_batch(a), "batch").at("effect_digest").text();
            if (b.at("effect_digest").text() != expected_digest)
                fail(nodes::code::invalid_record, "receipt effect digest mismatch");
            if (b.at("version").text() != version(string_list(a.at("frontier"))))
                fail(nodes::code::invalid_record, "receipt version mismatch");
            auto logical = b.at("author").text() + ":" + b.at("operation").text();
            receipts(logical).push_back(id);
            dirty_receipts.insert(logical);
        } else if (kind == "checkpoint") {
            auto state_owner = source.projection(string_list(b.at("frontier")));
            const auto& state = *state_owner;
            if (!state.conflicts.empty())
                fail(nodes::code::source_conflict, "checkpoint has unresolved source");
            value::array expected;
            for (auto& [file, f] : state.files)
                if (f->present)
                    expected.emplace_back(value::map{{"digest", f->digest},
                                                     {"executable", f->executable},
                                                     {"file", file},
                                                     {"kind", f->kind},
                                                     {"path", f->path},
                                                     {"size", static_cast<std::uint64_t>(f->bytes())}});
            auto manifest = codec::decode(get(b.at("manifest").text(), "blob").at("data").raw());
            if (manifest.at("files").list() != expected)
                fail(nodes::code::invalid_record, "checkpoint manifest does not match fixed source");
            auto inventory = codec::decode(get(b.at("inventory").text(), "blob").at("data").raw());
            if (inventory.at("objects") != b.at("retained"))
                fail(nodes::code::invalid_record, "checkpoint inventory mismatch");
            auto roots = string_list(b.at("policies"));
            if (roots.size() != 1)
                fail("policy_conflict", "checkpoint must bind one admission policy");
            auto permissions = get(roots.front());
            if (issuer(id) != b.at("owner").text())
                fail("invalid_signature", "checkpoint owner differs from signer");
            if (protected_target(permissions, b.at("worldline").text()) && !allowed(permissions, "nodes", issuer(id)))
                fail(nodes::code::policy_denied, "checkpoint node lacks target authority");
            bool accepted = false;
            std::vector<std::string> covered;
            for (auto& a : string_list(b.at("admissions"))) {
                auto& admission = get(a, "admission");
                if (admission.at("issuer") == b.at("owner") && admission.at("target").at("name") == b.at("worldline")) {
                    if (admission.at("frontier") == b.at("frontier")) {
                        within_line(admission.at("line").text(), string_list(b.at("frontier")));
                        accepted = true;
                    }
                    if (admission.at("policy") == b.at("policies").list().front()) {
                        auto heads = string_list(admission.at("frontier"));
                        covered.insert(covered.end(), heads.begin(), heads.end());
                    }
                }
                trim_objects();
            }
            if (!accepted && !(b.at("frontier").list().empty() && allowed(permissions, "nodes", b.at("owner").text())))
                fail(nodes::code::policy_denied, "checkpoint lacks a retained owner admission");
            if (protected_target(permissions, b.at("worldline").text()))
                for (auto& event : source.uncovered(string_list(b.at("frontier")), covered)) {
                    if (!allowed(permissions, "authors", source.event_author(event))) {
                        bool settled = false;
                        if (permissions.at("kind").text() == "policy")
                            for (auto& prior : string_list(permissions.at("settled")))
                                if (source.includes(string_list(get(prior, "admission").at("frontier")), event))
                                    settled = true;
                        if (!settled)
                            fail("policy_pending", "checkpoint source requires current-policy admission");
                    }
                    trim_objects();
                }
        } else if (kind == "inventory") {
            if (issuer(id) != b.at("owner").text())
                fail("invalid_signature", "inventory owner differs from origin");
            auto objects = string_list(b.at("objects"));
            if (objects != vector_set(objects))
                fail(nodes::code::invalid_record, "noncanonical inventory objects");
            if (!std::holds_alternative<std::monostate>(b.at("base").data)) {
                auto& base = get(b.at("base").text(), "checkpoint");
                auto retained = string_list(base.at("retained"));
                objects.insert(objects.end(), retained.begin(), retained.end());
                objects = vector_set(std::move(objects));
            }
            for (auto& line : b.at("worldlines").list()) {
                fields(line, {"frontier", "name"});
                checked_name(line.at("name").text());
                for (auto& event : string_list(line.at("frontier")))
                    if (!std::ranges::binary_search(objects, event))
                        fail(nodes::code::invalid_record, "inventory omits a worldline root");
            }
        }
        mark_validated(id);
    }

    auto local_node::impl::missing(const std::vector<std::string>& initial) const -> std::vector<std::string> {
        std::set<std::string> seen, absent;
        std::vector<std::string> work = initial;
        while (!work.empty()) {
            auto id = std::move(work.back());
            work.pop_back();
            if (!seen.insert(id).second)
                continue;
            if (is_validated(id))
                continue;
            if (!has_object(id)) {
                absent.insert(id);
                continue;
            }
            for (auto* r : ref_ids(get(id)))
                work.push_back(*r);
        }
        return {absent.begin(), absent.end()};
    }

    void local_node::impl::refuse(const std::string& id, const error& reason) {
        auto encoded = codec::encode(objects.at(id));
        auto status = codec::encode(value::map{{"code", reason.code}, {"reason", std::string(reason.what())}});
        statement q{*db, "INSERT OR REPLACE INTO rejected(id,reason,data) VALUES(?1,?2,?3)"};
        q.text(1, id);
        q.bind(2, status);
        q.bind(3, encoded);
        q.next();
        refused[id] = {reason.code, reason.what()};
        pending_objects.erase(id);
        remaining_dependencies.erase(id);
        dirty_status.insert(id);
        refused_objects.push_back(id);
    }

    auto local_node::impl::validate_available() -> std::vector<std::string> {
        ensure_status();
        if (!pending_loaded) {
            pending_loaded = true;
            pages->each("m/p/", [&](const std::string& key, std::string) { queue_object(key.substr(4)); });
        }
        std::deque<std::string> ready;
        std::vector<std::string> rejected;
        std::size_t processed = 0;
        auto release = [&](const std::string& id, bool rejected_dependency) {
            auto found = waiting_dependencies.find(id);
            if (found == waiting_dependencies.end())
                return;
            auto waiting = std::move(found->second);
            waiting_dependencies.erase(found);
            for (auto& dependent : waiting)
                if (pending_objects.contains(dependent)) {
                    if (rejected_dependency) {
                        refuse(dependent, error{nodes::code::policy_denied, "a required object was refused"});
                        rejected.push_back(dependent);
                    } else if (auto count = remaining_dependencies.find(dependent);
                               count != remaining_dependencies.end() && count->second && --count->second == 0)
                        ready.push_back(dependent);
                }
        };
        while (!new_candidates.empty() || !completed_objects.empty() || !refused_objects.empty() || !ready.empty()) {
            if (!completed_objects.empty()) {
                auto id = std::move(completed_objects.front());
                completed_objects.pop_front();
                release(id, false);
                continue;
            }
            if (!refused_objects.empty()) {
                auto id = std::move(refused_objects.front());
                refused_objects.pop_front();
                release(id, true);
                continue;
            }
            if (!ready.empty()) {
                auto id = std::move(ready.front());
                ready.pop_front();
                if (!pending_objects.contains(id))
                    continue;
                try {
                    validate_object(id);
                } catch (const error& reason) {
                    if (reason.code == nodes::code::storage_failure || reason.code == nodes::code::resource_limit)
                        throw;
                    refuse(id, reason);
                    rejected.push_back(id);
                }
                if (++processed % 128 == 0)
                    trim_objects();
                continue;
            }
            auto id = std::move(new_candidates.front());
            new_candidates.pop_front();
            if (!pending_objects.contains(id) || remaining_dependencies.contains(id) || refused.contains(id))
                continue;
            std::size_t count = 0;
            bool blocked = false;
            for (auto* reference : ref_ids(get(id))) {
                auto dependency = *reference;
                if (is_validated(dependency))
                    continue;
                if (refused.contains(dependency)) {
                    refuse(id, error{nodes::code::policy_denied, "a required object was refused"});
                    rejected.push_back(id);
                    blocked = true;
                    break;
                }
                waiting_dependencies[dependency].push_back(id);
                ++count;
                if (has_object(dependency))
                    (void)get(dependency);
            }
            if (!blocked) {
                remaining_dependencies[id] = count;
                if (!count)
                    ready.push_back(id);
            }
        }
        return rejected;
    }

    void local_node::impl::load_objects(std::uint64_t after) {
        struct record {
            std::string id, data;
            value object;
            std::exception_ptr failure;
            std::size_t bytes = 0;
        };
        std::vector<record> batch;
        statement query{*db, "SELECT id,data FROM objects WHERE rowid>?1 ORDER BY rowid"};
        query.number(1, after);
        bool exhausted = false;
        auto fill = [&] {
            batch.clear();
            std::size_t buffered = 0;
            while (!exhausted && batch.size() < 256 && buffered < 8 * 1024 * 1024) {
                if (!query.next()) {
                    exhausted = true;
                    break;
                }
                auto data = archive::unpack(query.data(1));
                buffered += data.size();
                batch.push_back({query.data(0), std::move(data), {}, {}});
                batch.back().bytes = batch.back().data.size();
            }
        };
        auto decode = [&](record& row) {
            if (digest_of(row.data) != row.id)
                fail(nodes::code::storage_failure, "stored object hash mismatch");
            auto object = codec::decode(row.data);
            if (!validated.contains(row.id))
                check_schema(open(object));
            row.object = std::move(object);
            std::string{}.swap(row.data);
        };
        auto merge = [&] {
            for (auto& row : batch) {
                if (row.failure)
                    std::rethrow_exception(row.failure);
                auto [stored, inserted] = objects.emplace(std::move(row.id), std::move(row.object));
                if (inserted) {
                    object_order.push_back(stored->first);
                    object_sizes.emplace(stored->first, row.bytes);
                    cached_object_bytes += row.bytes;
                }
                bodies.emplace(stored->first, body_view{&stored->second.at("body")});
                ++loaded_records;
                queue_object(stored->first);
            }
        };
        fill();
        if (batch.empty())
            return;
        // Decoding runs on the source's assigned disk worker, without an extra pool.
        do {
            for (auto& row : batch)
                decode(row);
            merge();
            validate_available();
            trim_objects();
            fill();
        } while (!batch.empty());
    }
} // namespace nodes
