namespace Fughetta

open System
open System.IO
open System.Collections.Generic
open System.Text.Json.Nodes
open System.Text.RegularExpressions
open FSharp.Compiler.CodeAnalysis
open FSharp.Compiler.IO
open FSharp.Compiler.Text
open FSharp.Compiler.Symbols
open FSharp.Compiler.Diagnostics
open FSharp.Compiler.Syntax
open Json

module CompilerInputs =
    let opaque (context: FSharpProjectContext) =
        try
            context.GetReferencedAssemblies() |> Seq.exists (fun assembly ->
                assembly.IsProviderGenerated ||
                (assembly.Contents.Attributes |> Seq.exists (fun attribute ->
                    attribute.IsUnresolved || attribute.AttributeType.IsProvided ||
                    attribute.AttributeType.FullName = "Microsoft.FSharp.Core.CompilerServices.TypeProviderAssemblyAttribute")))
        with :? InvalidOperationException -> true

type CheckedSources = {
    OpaqueInputs: bool
    Sources: string array
    Uses: FSharpSymbolUse array
    Diagnostics: FSharpDiagnostic array
    ImplementationFiles: FSharpImplementationFileContents array
    ParseTrees: Map<string, ParsedInput>
}

type Project = { Id: string; Dependencies: string array; Sources: string array; Inputs: string array; Generated: Set<string>; Options: FSharpProjectOptions }

type Workspace(root: string, host: JsonNode) =
    let documents = Dictionary<string, string>(StringComparer.Ordinal)
    let objects = field host "objects"
    let aliases = Dictionary<string, string>(StringComparer.Ordinal)
    let objectNames = Dictionary<string, string>(StringComparer.Ordinal)
    let previousFileSystem = FileSystem
    let aliasPath (path: string) = match aliases.TryGetValue path with true, original -> original | _ -> path
    let fileSystem =
        { new DefaultFileSystem() with
            override _.OpenFileForReadShim(path, useMemoryMappedFile, shouldShadowCopy) = previousFileSystem.OpenFileForReadShim(aliasPath path, ?useMemoryMappedFile = useMemoryMappedFile, ?shouldShadowCopy = shouldShadowCopy)
            override _.FileExistsShim(path) = previousFileSystem.FileExistsShim(aliasPath path)
            override _.GetLastWriteTimeShim(path) = previousFileSystem.GetLastWriteTimeShim(aliasPath path)
            override _.GetCreationTimeShim(path) = previousFileSystem.GetCreationTimeShim(aliasPath path) }
    do FileSystem <- fileSystem
    let mutable projects: Map<string, Project> = Map.empty
    let mutable sequence = 0L
    let mutable installation = ""
    let mutable modelPath = ""
    let versions = Dictionary<string, int>(StringComparer.Ordinal)
    let mutable sourceInputs = Map.empty<string, string>
    let mutable generated = Map.empty<string, string>
    let source path = async {
        let path = Path.GetFullPath(path)
        match documents.TryGetValue path with
        | true, value -> return Some(SourceText.ofString value)
        | _ when File.Exists path -> return Some(SourceText.ofString(File.ReadAllText path))
        | _ -> return None
    }
    let checker = FSharpChecker.Create(keepAssemblyContents = true, keepAllBackgroundResolutions = true,
                                       documentSource = DocumentSource.Custom source)
    let full (path: string) = Path.GetFullPath(Path.Combine(root, path))
    let expand (argument: string) =
        Regex.Replace(argument, @"\$\{(tree|object:[^}]+|sdk:[^}]+)\}", MatchEvaluator(fun matched ->
            let key = matched.Groups[1].Value
            if key = "tree" then root
            elif key.StartsWith("object:") then
                let original = Path.Combine(objects, key.Substring(7))
                match objectNames.TryGetValue original with true, named -> named | _ -> original
            else
                let name = key.Substring(4)
                if isNull (host["sdks"][name]) then failwithf "SDK is not installed: %s" name
                (host["sdks"][name]).GetValue<string>()))
    member _.Root = root
    member _.Projects = projects
    member _.ModelPath = modelPath
    member _.Text(path: string) =
        let path = full path
        match documents.TryGetValue path with
        | true, value -> value
        | _ -> File.ReadAllText path
    member _.Install(parameters: JsonNode) =
        let previous = Dictionary<string, string>(documents, StringComparer.Ordinal)
        for document in items parameters["documents"] do
            documents[full (field document "path")] <- field document "text"
        let models = parameters["artifacts"].AsObject() |> Seq.map (fun entry -> entry.Key) |> Seq.filter (fun path -> path.EndsWith("model.json")) |> Seq.toArray
        if models.Length <> 1 then failwith "F# helper requires one project_model"
        let model = JsonNode.Parse(File.ReadAllText(full models[0]))
        modelPath <- models[0]
        if model["schema"].GetValue<int>() <> 1 then failwith "Unknown project model schema"
        let allUnits = items model["units"] |> Seq.map (fun unit -> field unit "id", unit) |> Seq.toArray
        let fsharpUnits = allUnits |> Array.filter (fun (_, unit) -> field unit "language" = "fsharp")
        if fsharpUnits.Length = 0 then failwith "project_model has no F# compilation units"
        let units = Map.ofArray fsharpUnits
        aliases.Clear()
        objectNames.Clear()
        for _, unit in fsharpUnits do
            for reference in items unit["references"] do
                if field reference "kind" = "artifact" then
                    let artifact = reference["object"]
                    let original = Path.Combine(objects, field artifact "hash")
                    let extension = Path.GetExtension(field artifact "name")
                    if extension <> "" then
                        let named = original + extension
                        objectNames[original] <- named
                        aliases[named] <- original
        let currentSources = units |> Map.toSeq |> Seq.collect (fun (_, unit) -> items unit["sources"] |> Seq.map (fun path -> full (path.GetValue<string>()))) |> Set.ofSeq
        for path in documents.Keys |> Seq.toArray do
            if not (currentSources.Contains path) then documents.Remove path |> ignore
        let provided = items parameters["documents"] |> Seq.map (fun document -> full (field document "path")) |> Set.ofSeq
        let nextGenerated = units |> Map.toSeq |> Seq.collect (fun (_, unit) ->
            unit["generated"].AsObject() |> Seq.map (fun entry -> full entry.Key, field entry.Value "hash")) |> Map.ofSeq
        for path, hash in Map.toSeq nextGenerated do
            if not (provided.Contains path) && (generated.TryFind path <> Some hash || not (documents.ContainsKey path)) then
                documents[path] <- File.ReadAllText(Path.Combine(objects, hash))
        let nextInputs =
            parameters["sources"].AsObject() |> Seq.map (fun entry -> full entry.Key, entry.Value.ToJsonString()) |> Map.ofSeq
        let changed = HashSet<string>(StringComparer.Ordinal)
        for path in currentSources do
            let documentChanged =
                match documents.TryGetValue path, previous.TryGetValue path with
                | (true, value), (true, old) -> value <> old
                | (true, _), _ -> true
                | _ -> false
            if documentChanged || nextInputs.TryFind path <> sourceInputs.TryFind path || nextGenerated.TryFind path <> generated.TryFind path || not (versions.ContainsKey path) then
                versions[path] <- (match versions.TryGetValue path with true, version -> version + 1 | _ -> 1)
                changed.Add path |> ignore
        sourceInputs <- nextInputs
        generated <- nextGenerated
        let signature = model.ToJsonString() + parameters["artifacts"].ToJsonString() +
                        (parameters["sources"].AsObject() |> Seq.filter (fun entry -> not (currentSources.Contains(full entry.Key)))
                         |> Seq.sortBy (fun entry -> entry.Key) |> Seq.map (fun entry -> entry.Key + "=" + entry.Value.ToJsonString()) |> String.concat "\n")
        sequence <- sequence + 1L
        let built = Dictionary<string, Project>()
        let visiting = HashSet<string>()
        let rec build id =
            match built.TryGetValue id with
            | true, project -> project
            | _ ->
                if not (visiting.Add id) then failwith "Cyclic F# project references"
                let unit = units[id]
                let sources = items unit["sources"] |> Seq.map (fun item -> full (item.GetValue<string>())) |> Seq.toArray
                let references = items unit["references"] |> Seq.filter (fun r -> field r "kind" = "project") |> Seq.choose (fun reference ->
                    let matches = allUnits |> Array.filter (fun (id, unit) ->
                        field unit "project" = field reference "project" &&
                        (if field reference "unit" <> "" then id = field reference "unit"
                         else items unit["variants"] |> Seq.exists (fun attributes -> JsonNode.DeepEquals(attributes, reference["attributes"]))))
                    if matches.Length <> 1 then failwith "F# project reference requires one unit"
                    let dependency, target = matches[0]
                    if not (units.ContainsKey dependency) then None
                    else
                        let project = build dependency
                        let output =
                            if field reference "path" <> "" then full (field reference "path")
                            else items target["outputs"] |> Seq.exactlyOne |> fun value -> full (value.GetValue<string>())
                        Some (project.Id, FSharpReferencedProject.FSharpReference(output, project.Options))) |> Seq.toArray
                let arguments =
                    Seq.append
                        (items unit["compiler_arguments"] |> Seq.map (fun value -> expand (value.GetValue<string>())))
                        (references |> Seq.map (fun (_, reference) -> "-r:" + reference.OutputFile))
                    |> Seq.filter (fun argument -> not (Path.IsPathFullyQualified argument && Array.contains (Path.GetFullPath argument) sources))
                    |> Seq.distinct |> Seq.toArray
                let options = checker.GetProjectOptionsFromCommandLineArgs(full (id.Replace(':', '_') + ".fsproj"), arguments)
                let options = { options with SourceFiles = sources; ReferencedProjects = references |> Array.map snd; Stamp = Some sequence }
                let inputs =
                    Seq.append
                        (items unit["references"] |> Seq.filter (fun reference -> field reference "kind" = "tree") |> Seq.map (fun reference -> full (field reference "path")))
                        (items (unit.["extensions"].["additional_files"]) |> Seq.map (fun value -> full (value.GetValue<string>())))
                    |> Seq.distinct |> Seq.toArray
                let project = { Id = id; Dependencies = references |> Array.map fst; Sources = sources; Inputs = inputs; Generated = unit["generated"].AsObject() |> Seq.map (fun entry -> full entry.Key) |> Set.ofSeq; Options = options }
                built.Add(id, project)
                visiting.Remove id |> ignore
                project
        if signature <> installation then
            projects <- units |> Map.map (fun id _ -> build id)
            installation <- signature
        else
            for _, project in Map.toSeq projects do
                for path in project.Sources do
                    if changed.Contains path then checker.NotifyFileChanged(path, project.Options) |> Async.RunSynchronously
    member _.SourceObject(path: string) =
        let path = full path
        sourceInputs.TryFind path |> Option.map (fun value -> JsonNode.Parse(value).GetValue<string>())
    member _.ProjectResult(id: string) = checker.ParseAndCheckProject(projects[id].Options) |> Async.RunSynchronously
    member this.Check(id: string, requested: Set<string>) =
        let project = projects[id]
        let selected = project.Sources |> Array.filter (fun path -> requested.IsEmpty || requested.Contains path)
        let distinctDiagnostics values =
            values |> Array.distinctBy (fun (diagnostic: FSharpDiagnostic) -> diagnostic.ErrorNumberText, diagnostic.Range, diagnostic.Severity, diagnostic.Message)
        if requested.IsEmpty then
            let result = this.ProjectResult(id)
            // Project diagnostics omit startup/reference resolution errors that
            // FCS retains in the already-checked file result.
            let startup =
                if result.HasCriticalErrors || project.Sources.Length = 0 then [||]
                else
                    let _, background = checker.GetBackgroundCheckResultsForFileInProject(project.Sources[0], project.Options) |> Async.RunSynchronously
                    background.Diagnostics |> Array.filter (fun diagnostic ->
                        diagnostic.FileName = Range.rangeStartup.FileName || diagnostic.FileName = Range.rangeCmdArgs.FileName)
            { OpaqueInputs = result.HasCriticalErrors || CompilerInputs.opaque result.ProjectContext
              Sources = selected; Uses = result.GetAllUsesOfAllSymbols(); Diagnostics = Array.append result.Diagnostics startup |> distinctDiagnostics
              ImplementationFiles = if result.HasCriticalErrors then [||] else result.AssemblyContents.ImplementationFiles |> Seq.toArray
              ParseTrees = selected |> Array.map (fun path ->
                  path, (checker.GetBackgroundParseResultsForFileInProject(path, project.Options) |> Async.RunSynchronously).ParseTree) |> Map.ofArray }
        else
            // FCS owns the ordered prefix and signature/implementation cache.
            // Export only requested files, leaving subsequent files unchecked.
            let parsed = selected |> Array.map (fun path ->
                let parse, answer = checker.ParseAndCheckFileInProject(path, versions[path], SourceText.ofString(this.Text path), project.Options) |> Async.RunSynchronously
                match answer with
                | FSharpCheckFileAnswer.Succeeded result -> parse, result
                | FSharpCheckFileAnswer.Aborted -> failwithf "F# file check was cancelled: %s" path)
            let checkedFiles = parsed |> Array.map snd
            { OpaqueInputs = checkedFiles |> Array.exists (fun result -> not result.HasFullTypeCheckInfo || CompilerInputs.opaque result.ProjectContext)
              Sources = selected
              Uses = checkedFiles |> Array.collect (fun result -> result.GetAllUsesOfAllSymbolsInFile() |> Seq.toArray)
              Diagnostics = checkedFiles |> Array.collect (fun result -> result.Diagnostics) |> distinctDiagnostics
              ImplementationFiles = checkedFiles |> Array.choose (fun result -> result.ImplementationFile)
              ParseTrees = Array.zip selected parsed |> Array.map (fun (path, (parse, _)) -> path, parse.ParseTree) |> Map.ofArray }
    member _.Close() =
        checker.InvalidateAll()
        FileSystem <- previousFileSystem
