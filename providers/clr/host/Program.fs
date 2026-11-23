namespace Fughetta

open System
open System.IO
open System.Reflection
open System.Runtime.Loader
open System.Text
open System.Text.Json.Nodes
open Json

/// Loads a plugin's own dependencies next to it while the host assembly and FSharp.Core stay
/// shared, so both sides agree on ILanguageAdapter and F# core types.
type private PluginContext(path: string) =
    inherit AssemblyLoadContext(Path.GetFileNameWithoutExtension path)
    let resolver = AssemblyDependencyResolver(path)
    let shared = set [ typeof<ILanguageAdapter>.Assembly.GetName().Name; typeof<Set<string>>.Assembly.GetName().Name ]
    override this.Load(name) =
        if shared.Contains name.Name then null
        else
            let local = Path.Combine(Path.GetDirectoryName path, name.Name + ".dll")
            match resolver.ResolveAssemblyToPath name with
            | null when File.Exists local -> this.LoadFromAssemblyPath local
            | null -> null
            | resolved -> this.LoadFromAssemblyPath resolved

/// Operations match language::helper; the worker serializes requests and owns cancellation.
type private Session(root: string, adapter: ILanguageAdapter) =
    let mutable state: JsonNode = null
    let mutable installed = false
    let mutable closed = false
    let opened () = if isNull state then failwith "helper session is not open" else state
    let ready () = let session = opened () in (if not installed then failwith "helper model is not installed"); session
    let epoch (value: JsonNode) = if not (JsonNode.DeepEquals(value["epoch"], opened()["epoch"])) then failwith "stale helper epoch"
    let same (session: JsonNode) =
        if field session "id" <> field (opened ()) "id" || field session "slot" <> field (opened ()) "slot" then
            failwith "request belongs to another helper session"
    let unitKey name =
        let session = opened ()
        object ["project", copy session["project"]; "stage", copy session["stage"]; "provider", copy session["provider"]
                "operation", text "evaluate"; "kind", text "module"; "name", text name]
    member this.Dispatch(operation, parameters: JsonNode) =
        match operation with
        | "open" ->
            if not (isNull state) then failwith "helper session is already open"
            adapter.Open(root, parameters["session"], parameters["host"], parameters["initialization"])
            state <- copy parameters["session"]
            object []
        | "update" ->
            same parameters["session"]
            adapter.Install parameters
            state <- copy parameters["session"]
            installed <- true
            object []
        | "units" ->
            epoch parameters
            ready () |> ignore
            let files = items (parameters["demand"]["files"]) |> Seq.map (fun value -> Path.GetFullPath(Path.Combine(root, value.GetValue<string>()))) |> Set.ofSeq
            adapter.Units files |> Seq.map unitKey |> array
        | "evaluate" ->
            let request = parameters["request"]
            epoch request
            let session = ready ()
            if field request "slot_id" <> field session "slot" then failwith "evaluation belongs to another slot"
            let shard, producers = adapter.Evaluate(session, parameters["unit"], field request["unit"] "name")
            let producers = producers |> Map.toList |> List.map (fun (read, modules) -> read, modules |> Seq.map unitKey |> array)
            object ["session", copy session; "request", copy request; "shards", array [shard]
                    "receipts", array []; "producer_keys", object producers; "deferred", flag false]
        | "observe" ->
            epoch parameters["session"]
            same parameters["session"]
            adapter.Observe(ready (), parameters["reads"])
        | "close" | "cancel" -> this.Close(); object []
        | _ -> failwithf "unknown helper operation: %s" operation
    member _.Close() =
        if not closed then
            closed <- true
            adapter.Dispose()

module Program =
    let private limit = 64 * 1024 * 1024

    let private serve (session: Session) (output: TextWriter) =
        let mutable running = true
        while running do
            let line = Console.ReadLine()
            if isNull line then running <- false
            else
                let message = JsonNode.Parse(line)
                let operation = field message "operation"
                let response =
                    try object ["request_id", copy message["request_id"]; "result", session.Dispatch(operation, message["params"])]
                    with error -> object ["request_id", copy message["request_id"]; "error", text error.Message]
                let response = response.ToJsonString()
                if Encoding.UTF8.GetByteCount(response) > limit then
                    output.WriteLine((object ["request_id", copy message["request_id"]; "error", text "helper result exceeds semantic object limit"]).ToJsonString())
                else output.WriteLine(response)
                if operation = "close" || operation = "cancel" then running <- false

    let private adapter (path: string) =
        let path = Path.GetFullPath path
        if not (File.Exists path) then failwithf "language adapter does not exist: %s" path
        let assembly = PluginContext(path).LoadFromAssemblyPath path
        match assembly.GetExportedTypes() |> Array.filter (fun kind -> kind.IsClass && not kind.IsAbstract && typeof<ILanguageAdapter>.IsAssignableFrom kind) with
        | [| kind |] -> Activator.CreateInstance kind :?> ILanguageAdapter
        | _ -> failwithf "%s must provide exactly one %s" path typeof<ILanguageAdapter>.FullName

    [<EntryPoint>]
    let main args =
        try
            match args with
            | [| "--clr"; assembly |] -> Console.WriteLine((Clr.read assembly None).ToJsonString())
            | [| "--clr"; assembly; pdb |] -> Console.WriteLine((Clr.read assembly (Some pdb)).ToJsonString())
            | [| "--adapter"; path |] ->
                // Compiler and platform output must never share the protocol stream.
                let output = new StreamWriter(Console.OpenStandardOutput(), UTF8Encoding(false), AutoFlush = true)
                Console.SetOut(Console.Error)
                let session = Session(Directory.GetCurrentDirectory(), adapter path)
                try serve session output finally session.Close()
            | _ -> failwith "expected --adapter <language-adapter.dll> or --clr <assembly> [portable-pdb]"
            0
        with error -> eprintfn "%s" error.Message; 1
