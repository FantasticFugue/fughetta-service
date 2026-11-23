module Fughetta.MSBuildModel

open Argu
open System
open System.IO
open System.Diagnostics
open System.Collections.Generic
open System.Reflection.Metadata
open System.Reflection.PortableExecutable
open System.Security.Cryptography
open System.Text.Json.Nodes
open System.Text.RegularExpressions
open System.Xml.Linq

let text (s: string) : JsonNode = JsonValue.Create(s)
let number (n: int) : JsonNode = JsonValue.Create(n)
let array (items: seq<JsonNode>) : JsonNode = JsonArray(Array.ofSeq items)
let strings items = items |> Seq.map text |> array
let object (items: (string * JsonNode) list) : JsonNode =
    JsonObject(items |> Seq.map (fun (k, v) -> KeyValuePair(k, v)))
let field (node: JsonNode) (name: string) = if isNull node[name] then "" else node[name].GetValue<string>()
let values (node: JsonNode) = node.AsArray() |> Seq.cast<JsonNode>
let full (path: string) = Path.GetFullPath(path)
let within (root: string) (path: string) = path = root || path.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar) + string Path.DirectorySeparatorChar, StringComparison.Ordinal)
let join root path = Path.Combine(root, path) |> full
let expandProjects directory projects =
    let supported (path: string) =
        match Path.GetExtension(path).ToLowerInvariant() with
        | ".csproj" | ".fsproj" -> true
        | _ -> false
    let solutionEntry = Regex("""^\s*Project\("[^"]*"\)\s*=\s*"[^"]*",\s*"([^"]+\.(?:cs|fs)proj)"(?:\s*,|\s*$)""",
                              RegexOptions.IgnoreCase ||| RegexOptions.CultureInvariant)
    let comparer = if OperatingSystem.IsWindows() then StringComparer.OrdinalIgnoreCase else StringComparer.Ordinal
    let seen = HashSet<string>(comparer)
    projects
    |> List.collect (fun path ->
        let path = join directory path
        let resolve (entry: string) =
            entry.Replace('\\', Path.DirectorySeparatorChar).Replace('/', Path.DirectorySeparatorChar)
            |> join (Path.GetDirectoryName path)
        let entries =
            match Path.GetExtension(path).ToLowerInvariant() with
            | ".slnx" ->
                XDocument.Load(path).Descendants()
                |> Seq.filter (fun node -> node.Name.LocalName = "Project")
                |> Seq.choose (fun node ->
                    match node.Attribute(XName.Get "Path") with
                    | null -> None
                    | attribute ->
                        let project = resolve attribute.Value
                        if not (File.Exists project) then failwithf "Solution %s references missing project: %s" path project
                        Some project)
                |> Seq.filter supported
                |> Seq.toList
            | ".sln" ->
                File.ReadLines(path)
                |> Seq.choose (fun line ->
                    let entry = solutionEntry.Match line
                    if entry.Success then Some (resolve entry.Groups[1].Value) else None)
                |> Seq.toList
            | _ -> [path]
        if entries.IsEmpty then failwithf "Solution %s contains no supported .csproj or .fsproj projects" path
        entries)
    |> List.filter seen.Add
let relative root path =
    let path = full path
    if not (within root path) then failwithf "Source/output lies outside the installed tree: %s" path
    Path.GetRelativePath(root, path).Replace('\\', '/')
let run (tool: string) (cwd: string) (args: seq<string>) =
    let info = ProcessStartInfo(tool, WorkingDirectory=cwd, RedirectStandardOutput=true, RedirectStandardError=true, UseShellExecute=false)
    for arg in args do info.ArgumentList.Add(arg)
    use child = Process.Start(info)
    let stdout, stderr = child.StandardOutput.ReadToEndAsync(), child.StandardError.ReadToEndAsync()
    child.WaitForExit()
    if child.ExitCode <> 0 then failwith (stdout.Result + stderr.Result)
    stdout.Result
let digest path =
    use input = File.OpenRead(path)
    SHA256.HashData(input) |> Convert.ToHexStringLower
// Whether the compiler would load the assembly at `path` as a type provider. An assembly that cannot be read counts as one.
let providesTypes (path: string) =
    File.Exists path &&
    try
        use pe = new PEReader(File.OpenRead path)
        pe.HasMetadata &&
        let metadata = pe.GetMetadataReader()
        metadata.GetAssemblyDefinition().GetCustomAttributes() |> Seq.exists (fun handle ->
            let constructor = metadata.GetCustomAttribute(handle).Constructor
            constructor.Kind = HandleKind.MemberReference &&
            let parent = metadata.GetMemberReference(MemberReferenceHandle.op_Explicit constructor).Parent
            parent.Kind = HandleKind.TypeReference &&
            let attribute = metadata.GetTypeReference(TypeReferenceHandle.op_Explicit parent)
            metadata.GetString(attribute.Namespace) = "Microsoft.FSharp.Core.CompilerServices" &&
            metadata.GetString(attribute.Name) = "TypeProviderAssemblyAttribute")
    with _ -> true
let write (path: string) (value: JsonNode) =
    Directory.CreateDirectory(Path.GetDirectoryName(full path)) |> ignore
    let temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp"
    try
        File.WriteAllText(temp, value.ToJsonString() + "\n")
        File.Move(temp, path, true)
    finally
        if File.Exists(temp) then File.Delete(temp)

type Arguments =
    | [<Mandatory; Unique>] Root of string
    | [<Mandatory; Unique>] Project_Dir of string
    | [<Mandatory; Unique>] Tool of string
    | [<Mandatory; Unique>] Jobs of int
    | [<Mandatory; Unique>] Restore_Output of string
    | [<Mandatory; Unique>] Model_Output of string
    | [<Mandatory; Unique>] Assets of string
    | [<Mandatory>] Project of string
    | Generated of string
    | Argument of string
    | [<Mandatory; Unique; Last>] Operations of string list
    interface IArgParserTemplate with
        member option.Usage =
            match option with
            | Root _ -> "Workspace root."
            | Project_Dir _ -> "Working directory for relative project paths."
            | Tool _ -> "Path to dotnet."
            | Jobs _ -> "Maximum number of MSBuild processes."
            | Restore_Output _ -> "Destination for the restore receipt."
            | Model_Output _ -> "Destination for the exported project model."
            | Assets _ -> "Path to the restore receipt."
            | Project _ -> "MSBuild project or solution to evaluate."
            | Generated _ -> "Generated source input."
            | Argument _ -> "Extra MSBuild argument."
            | Operations _ -> "Operations to execute in order."

let runModel (argv: string array) =
    let parser = ArgumentParser.Create<Arguments>(programName = "fughetta-msbuild-model")
    let options = parser.ParseCommandLine argv
    let modes = options.GetResult Operations
    if modes.IsEmpty || not (modes |> List.forall (fun mode -> mode = "restore" || mode = "model")) then
        failwith "Expected one or more restore or model operations"
    let jobs = options.GetResult Jobs
    if jobs < 1 then failwith "--jobs must be positive"
    let root, directory, tool = full (options.GetResult Root), full (options.GetResult Project_Dir), options.GetResult Tool
    let extra = options.GetResults Argument
    let projects = options.GetResults Project |> expandProjects directory
    let objects = Environment.GetEnvironmentVariable("FUGHETTA_STAGING")
    if String.IsNullOrEmpty objects then failwith "FUGHETTA_STAGING must name this step staging directory"
    let bindings =
        let json = Environment.GetEnvironmentVariable("FUGHETTA_SDKS")
        if String.IsNullOrEmpty json then Seq.empty else JsonNode.Parse(json) |> values
    let replacements = Dictionary<string, string>()
    let sdk path role =
        bindings |> Seq.collect (fun binding ->
            values binding["roots"] |> Seq.map (fun item -> binding, full (item.GetValue<string>())))
        |> Seq.filter (fun (_, prefix) -> within prefix path)
        |> Seq.sortByDescending (fun (_, prefix) -> prefix.Length)
        |> Seq.tryHead
        |> Option.map (fun (binding, prefix) ->
            let name = field binding "name"
            replacements[prefix] <- "${sdk:" + name + "}"
            object ["kind", text "sdk"; "name", text name; "path", text (relative prefix path); "role", text role])
    let intern path =
        use input = File.OpenRead(path)
        let hash = SHA256.HashData(input) |> Convert.ToHexStringLower
        Directory.CreateDirectory(objects) |> ignore
        let destination = Path.Combine(objects, hash)
        if not (File.Exists destination) then
            let temporary = Path.Combine(objects, ".incoming-" + Guid.NewGuid().ToString("N"))
            try
                input.Position <- 0L
                use copied = File.Create(temporary)
                input.CopyTo(copied)
                copied.Close()
                try File.Move(temporary, destination) with :? IOException when File.Exists(destination) -> ()
            finally
                if File.Exists temporary then File.Delete temporary
        replacements[path] <- "${object:" + hash + "}"
        object ["hash", text hash; "name", text (Path.GetFileName path)]
    let projectReference project unit path role =
        object ["kind", text "project"; "project", text project; "unit", text unit; "path", text path;
                "role", text role; "attributes", object []]
    let reference path role isFramework =
        match sdk path role with
        | Some value -> value
        | None when isFramework -> failwithf "Reference pack requires a worker SDK mapping: %s" path
        | None when within root path -> object ["kind", text "tree"; "path", text (relative root path); "role", text role]
        | None ->
            object ["kind", text "artifact"; "role", text role; "object", intern path]
    let arguments (args: seq<string>) =
        replacements[root] <- "${tree}"
        args |> Seq.map (fun argument ->
            replacements |> Seq.sortByDescending (fun entry -> entry.Key.Length)
            |> Seq.fold (fun value entry ->
                Regex.Replace(value, Regex.Escape(entry.Key) + "(?=$|[/\\\\,;:\\s\"'])", MatchEvaluator(fun _ -> entry.Value))) argument)
        |> strings
    let query (project: string) (flags: string list) (properties: string list) (items: string list) =
        let args = ["msbuild"; project; "-nologo"; "-verbosity:quiet"; "-maxCpuCount:" + string jobs] @ extra @ flags @
                   ["-getProperty:" + String.concat "," properties] @
                   (if List.isEmpty items then [] else ["-getItem:" + String.concat "," items])
        run tool directory args |> JsonNode.Parse
    let execute mode =
        let output =
            match mode with
            | "restore" -> full (options.GetResult Restore_Output)
            | "model" -> full (options.GetResult Model_Output)
            | _ -> failwith "Expected restore or model"
        if mode = "restore" then
            // Query the evaluated root's assets path after Restore in the same
            // MSBuild invocation. Transitive projects retain their own evaluation.
            let restoredRoots = projects |> List.map (fun project ->
                project, query project ["-target:Restore"] ["ProjectAssetsFile"; "MSBuildProjectFullPath"] [])
            let assets = SortedDictionary<string, JsonNode>()
            let visited = HashSet<string>()
            let rec record (project: string) (known: JsonNode option) =
                if visited.Add(full project) then
                    let result = match known with
                                 | Some result -> result
                                 | None -> query project [] ["ProjectAssetsFile"; "MSBuildProjectFullPath"] []
                    let path = field result["Properties"] "ProjectAssetsFile" |> full
                    if not (File.Exists path) then failwithf "Restore did not produce assets: %s" path
                    let logical = relative root path
                    assets[logical] <- object ["path", text logical; "hash", text (digest path)]
                    let restored = File.ReadAllText(path) |> JsonNode.Parse
                    let frameworks = restored["project"]["restore"]["frameworks"]
                    for framework in frameworks.AsObject() do
                        let references = framework.Value["projectReferences"]
                        if not (isNull references) then
                            for dependency in references.AsObject() do record (field dependency.Value "projectPath") None
            for project, result in restoredRoots do record project (Some result)
            write output (object ["kind", text "nuget_assets"; "files", array assets.Values])
        elif mode = "model" then
            let receipt = File.ReadAllText(options.GetResult Assets) |> JsonNode.Parse
            if field receipt "kind" <> "nuget_assets" then failwith "Expected nuget_assets input"
            let restoredAssets = HashSet<string>()
            for file in values receipt["files"] do
                let path = join root (field file "path")
                if not (File.Exists path) then failwithf "Missing restored assets: %s" path
                restoredAssets.Add(path) |> ignore
                if digest path <> field file "hash" then failwithf "Restored assets differ from nuget_assets input: %s" path
            let generatedInputs = options.GetResults Generated |> Seq.map full |> Seq.toArray
            for path in generatedInputs do
                if not (File.Exists path || Directory.Exists path) then failwithf "Missing generated_sources input: %s" path
            let units = Dictionary<string, JsonNode>()
            let mutable producerVersion = ""
            let rec visit project framework single =
                let project = full project
                match framework with
                | None ->
                    let result = query project [] ["TargetFramework"; "TargetFrameworks"] []
                    let props = result["Properties"]
                    let target = field props "TargetFramework"
                    let targets = if target <> "" then [target] else (field props "TargetFrameworks").Split(';', StringSplitOptions.RemoveEmptyEntries) |> Array.toList
                    if single && targets.Length <> 1 then failwithf "Project reference requires one target framework: %s" project
                    targets |> List.collect (fun target -> visit project (Some target) false)
                | Some target ->
                    let projectId = relative root project
                    let id = projectId + ":" + target
                    if not (units.ContainsKey id) then
                        let flags = ["-target:Compile"; "-property:TargetFramework=" + target; "-property:DesignTimeBuild=true";
                                     "-property:BuildingInsideVisualStudio=true"; "-property:BuildProjectReferences=false";
                                     "-property:SkipCompilerExecution=true"; "-property:ProvideCommandLineArgs=true"]
                        let result =
                            query project flags
                                ["TargetPath"; "AssemblyName"; "LangVersion"; "DefineConstants"; "IntermediateOutputPath"; "NETCoreSdkVersion"; "ProjectAssetsFile"; "CompilerGeneratedFilesOutputPath"]
                                ["Compile"; "ReferencePath"; "_MSBuildProjectReferenceExistent"; "FscCommandLineArgs"; "CscCommandLineArgs"; "Analyzer"; "AdditionalFiles"]
                        let props, items = result["Properties"], result["Items"]
                        if not (restoredAssets.Contains(full (field props "ProjectAssetsFile"))) then failwithf "Project assets are absent from nuget_assets input: %s" project
                        let version = field props "NETCoreSdkVersion"
                        if producerVersion <> "" && producerVersion <> version then failwith "Project model contains different .NET SDK versions"
                        producerVersion <- version
                        let projectDirectory = Path.GetDirectoryName(project)
                        let intermediate = join projectDirectory ((field props "IntermediateOutputPath").Replace('\\', '/'))
                        let isCSharp =
                            String.Equals(Path.GetExtension(project), ".csproj", StringComparison.OrdinalIgnoreCase) ||
                            (not (isNull items["CscCommandLineArgs"]) && not (Seq.isEmpty (values items["CscCommandLineArgs"])))
                        let generated = JsonObject()
                        let sources = ResizeArray<string>()
                        let sourceArguments = Dictionary<string, string>()
                        for item in values items["Compile"] do
                            let path = full (field item "FullPath")
                            let logical = relative root path
                            sources.Add(logical)
                            sourceArguments[field item "Identity"] <- "${tree}/" + logical
                            let automatic = within intermediate path || field item "AutoGen" = "true" || field item "Generated" = "true"
                            if automatic || generatedInputs |> Array.exists (fun input -> within input path) then
                                generated[logical] <- intern path
                        let generatorOutput = field props "CompilerGeneratedFilesOutputPath"
                        if isCSharp && generatorOutput <> "" then
                            let directory = join projectDirectory generatorOutput
                            let enumerator = EnumerationOptions(RecurseSubdirectories=true, AttributesToSkip=FileAttributes.ReparsePoint)
                            for input in generatedInputs do
                                let files =
                                    if File.Exists input && within directory input then Seq.singleton input
                                    elif Directory.Exists input then
                                        let candidate = if within input directory then directory else input
                                        if within directory candidate && Directory.Exists candidate then
                                            Directory.EnumerateFiles(candidate, "*.cs", enumerator)
                                        else Seq.empty
                                    else Seq.empty
                                for path in files |> Seq.sort do
                                    if String.Equals(Path.GetExtension(path), ".cs", StringComparison.OrdinalIgnoreCase) then
                                        let logical = relative root path
                                        if not (generated.ContainsKey logical) then
                                            sources.Add(logical)
                                            generated[logical] <- intern path
                        let refs = ResizeArray<JsonNode>()
                        let mutable typeProviders = false
                        for item in values items["ReferencePath"] do
                            let framework = (field item "NuGetPackageId").EndsWith(".Ref", StringComparison.Ordinal) || field item "FrameworkReferenceName" <> ""
                            if not framework && providesTypes (full (field item "FullPath")) then typeProviders <- true
                        for item in values items["ReferencePath"] |> Seq.filter (fun item ->
                            field item "ReferenceSourceTarget" <> "ProjectReference" && field item "MSBuildSourceProjectFile" = "") do
                            let pack = field item "NuGetPackageId"
                            let framework = pack.EndsWith(".Ref", StringComparison.Ordinal) || field item "FrameworkReferenceName" <> ""
                            refs.Add(reference (full (field item "FullPath")) "classpath" framework)
                        let analyzers = values items["Analyzer"] |> Seq.map (fun item -> full (field item "FullPath")) |> Seq.toArray
                        let analyzerFiles = HashSet<string>(analyzers)
                        for path in analyzers do refs.Add(reference path "plugin" false)
                        if isCSharp then
                            for directory in analyzers |> Seq.map Path.GetDirectoryName |> Seq.distinct |> Seq.filter Directory.Exists do
                                for path in Directory.EnumerateFiles(directory, "*.dll") |> Seq.sort do
                                    if analyzerFiles.Add(path) then refs.Add(reference path "plugin_dependency" false)
                        let language, compiler = if isCSharp then "csharp", "CscCommandLineArgs" else "fsharp", "FscCommandLineArgs"
                        let compilerArguments = values items[compiler] |> Seq.map (fun item ->
                            let arg = field item "Identity"
                            if sourceArguments.ContainsKey arg then sourceArguments[arg] else arg) |> arguments
                        let generatorPath = if generatorOutput = "" then "" else join projectDirectory generatorOutput
                        let generatorModelPath = if generatorPath <> "" && within root generatorPath then relative root generatorPath else ""
                        let unit = object ["id", text id; "project", text projectId; "language", text language;
                            "directory", text (relative root projectDirectory); "sources", strings sources; "generated", generated;
                            "references", JsonArray(); "variants", JsonArray(); "outputs", strings [relative root (field props "TargetPath")];
                            "compiler_arguments", compilerArguments; "extensions", object ["framework", text target;
                                "assembly", props["AssemblyName"].DeepClone(); "language_version", props["LangVersion"].DeepClone();
                                "compiler_generated_files", text generatorModelPath;
                                "defines", strings ((field props "DefineConstants").Split(';', StringSplitOptions.RemoveEmptyEntries));
                                "additional_files", strings (values items["AdditionalFiles"] |> Seq.map (fun item -> relative root (field item "FullPath")));
                                "type_providers", (JsonValue.Create(typeProviders) :> JsonNode)]]
                        units[id] <- unit
                        for item in values items["_MSBuildProjectReferenceExistent"] do
                            if field item "ReferenceOutputAssembly" <> "false" then
                                let frameworks = (field item "SetTargetFramework").Split(';', StringSplitOptions.RemoveEmptyEntries)
                                                 |> Array.choose (fun property ->
                                                     let pair = property.Split('=', 2)
                                                     if pair.Length = 2 && pair[0].Trim() = "TargetFramework" then Some (pair[1].Trim()) else None)
                                let selected = match frameworks with
                                               | [||] -> ""
                                               | [|value|] -> value
                                               | _ -> failwith "Repeated TargetFramework in project reference metadata"
                                let path = full (field item "FullPath")
                                let dependencies = visit path (if selected = "" then None else Some selected) true
                                match dependencies with
                                | [dependency] ->
                                    let referencePaths =
                                        values items["ReferencePath"]
                                        |> Seq.filter (fun reference ->
                                            let owner = field reference "MSBuildSourceProjectFile"
                                            owner <> "" && full owner = path)
                                        |> Seq.map (fun reference ->
                                            let assembly = field reference "ReferenceAssembly"
                                            relative root (if assembly = "" then field reference "FullPath" else assembly))
                                        |> Seq.distinct |> Seq.toArray
                                    if referencePaths.Length = 0 then refs.Add(projectReference (relative root path) dependency "" "classpath")
                                    else for referencePath in referencePaths do refs.Add(projectReference (relative root path) dependency referencePath "classpath")
                                | _ -> failwithf "Project reference requires one target framework: %s" path
                        unit["references"] <- array refs
                    [id]
            for project in projects do visit project None false |> ignore
            write output (object ["schema", number 1; "producer", text "msbuild"; "producer_version", text producerVersion;
                                  "units", array (units |> Seq.sortBy (fun entry -> entry.Key) |> Seq.map (fun entry -> entry.Value))])
        else failwith "Expected restore or model"
    for mode in modes do execute mode

[<EntryPoint>]
let main argv =
    try
        runModel argv
        0
    with error ->
        eprintfn "%s" error.Message
        1
