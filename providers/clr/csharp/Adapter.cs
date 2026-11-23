using System.Text.Json.Nodes;
using Microsoft.FSharp.Collections;

namespace Fughetta.CSharp;

public sealed class CSharpAdapter : ILanguageAdapter
{
    private Workspace? workspace;
    private Workspace Active => workspace ?? throw new InvalidOperationException("C# workspace is not open");

    public void Open(string root, JsonNode session, JsonNode host, JsonNode initialization) => workspace = new(root, host);
    public void Install(JsonNode parameters) => Active.Install(parameters);

    public IEnumerable<string> Units(FSharpSet<string> files)
    {
        var requested = files.ToHashSet(Workspace.PathComparer);
        return Active.Units.Values.Where(unit => requested.Count == 0 || unit.Trees.Keys.Any(requested.Contains))
            .Select(unit => unit.Id).Order(StringComparer.Ordinal);
    }

    public Tuple<JsonNode, FSharpMap<string, string[]>> Evaluate(JsonNode session, JsonNode template, string name)
    {
        var unit = Active.Units[name];
        var output = template.DeepClone();
        var producers = Reads.Install(Active, session, output, unit);
        var shard = new Projection(Active, unit, session, output).Project();
        return Tuple.Create<JsonNode, FSharpMap<string, string[]>>(
            shard, MapModule.OfSeq(producers.Select(entry => Tuple.Create(entry.Key, entry.Value))));
    }

    public JsonNode Observe(JsonNode session, JsonNode reads) => Reads.Observe(Active, session, reads);

    public void Dispose()
    {
        workspace?.Dispose();
        workspace = null;
    }
}
