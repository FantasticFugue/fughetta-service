namespace Fughetta

open System
open System.Text.Json.Nodes

/// A compiler projection loaded into the CLR host. The host owns framing, session identity,
/// epochs and unit keys; an adapter answers only with compiler data for named modules.
/// One adapter instance serves one helper session and is disposed with it.
type ILanguageAdapter =
    inherit IDisposable
    /// Starts compiler state rooted at root; host names the object store and SDK roots.
    abstract Open: root: string * session: JsonNode * host: JsonNode * initialization: JsonNode -> unit
    /// Installs one update: session, documents, sources, artifacts, generated files and facts.
    abstract Install: parameters: JsonNode -> unit
    /// Modules containing any of the absolute files; every module when files is empty.
    abstract Units: files: Set<string> -> string seq
    /// Projects one module into a shard completing template, with the modules producing
    /// each observed read key.
    abstract Evaluate: session: JsonNode * template: JsonNode * name: string -> JsonNode * Map<string, string array>
    /// Current channel inputs for previously observed reads; unknown reads are omitted.
    abstract Observe: session: JsonNode * reads: JsonNode -> JsonNode
