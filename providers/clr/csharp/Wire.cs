using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

namespace Fughetta.CSharp;

internal static class Wire
{
    internal static string String(this JsonNode node, string key) => node[key]?.GetValue<string>() ?? "";
    internal static IEnumerable<JsonNode> Items(this JsonNode? node) =>
        node is JsonArray array ? array.OfType<JsonNode>() : [];
    internal static IEnumerable<KeyValuePair<string, JsonNode?>> Properties(this JsonNode? node) =>
        node is JsonObject obj ? obj : [];
    internal static JsonArray Rows(IEnumerable<JsonNode> values) => new(values.ToArray());
    internal static JsonArray Strings(IEnumerable<string> values) =>
        new(values.Select(value => (JsonNode?)JsonValue.Create(value)).ToArray());
    internal static string Digest(string text) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
}
