namespace Fughetta

open System.Collections.Generic
open System.Text.Json.Nodes

module Json =
    let text (value: string) : JsonNode = JsonValue.Create(value)
    let number (value: int) : JsonNode = JsonValue.Create(value)
    let flag (value: bool) : JsonNode = JsonValue.Create(value)
    let array (values: seq<JsonNode>) : JsonNode = JsonArray(Array.ofSeq values)
    let strings values = values |> Seq.map text |> array
    let object (values: (string * JsonNode) list) : JsonNode =
        JsonObject(values |> Seq.map (fun (key, value) -> KeyValuePair(key, value)))
    let field (value: JsonNode) (name: string) = if isNull value[name] then "" else value[name].GetValue<string>()
    let items (value: JsonNode) = if isNull value then Seq.empty else value.AsArray() :> seq<JsonNode>
    let copy (value: JsonNode) = if isNull value then null else value.DeepClone()
