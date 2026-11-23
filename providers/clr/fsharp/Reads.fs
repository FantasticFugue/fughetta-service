namespace Fughetta

open System
open System.IO
open System.Text
open System.Text.Json
open System.Text.Json.Nodes
open System.Security.Cryptography
open System.Collections.Generic
open FSharp.Compiler.Symbols
open FSharp.Compiler.Diagnostics
open FSharp.Compiler.Text
open Json

module Reads =
    exception private OpaqueCompilerInput
    type private Observation = Semantic of Map<string, string> | SourceRequired | SnapshotRequired
    let private digest (value: JsonNode) = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(value.ToJsonString())))
    let private unsupported () = raise (InvalidOperationException("FCS declaration requires source dependency"))
    let private typeName (value: FSharpType) =
        let rec inspect (typ: FSharpType) =
            if typ.IsUnresolved then unsupported ()
            if typ.HasTypeDefinition && typ.TypeDefinition.IsProvided then raise OpaqueCompilerInput
            if typ.IsAbbreviation then inspect typ.AbbreviatedType
            for argument in typ.GenericArguments do inspect argument
        inspect value
        value.Format(FSharpDisplayContext.Empty)
    let rec private constant (value: obj) =
        match value with
        | null -> object ["kind", text "null"]
        | :? FSharpType as typ -> object ["kind", text "type"; "value", text (typeName typ)]
        | :? Array as values -> object ["kind", text (value.GetType().FullName); "value", values |> Seq.cast<obj> |> Seq.map constant |> array]
        | _ when value.GetType().IsPrimitive || (value :? string) || (value :? decimal) ->
            object ["kind", text (value.GetType().FullName); "value", JsonNode.Parse(JsonSerializer.Serialize(value, value.GetType()))]
        | _ -> unsupported ()
    let private attributes (values: seq<FSharpAttribute>) =
        values |> Seq.map (fun attr ->
            if attr.IsUnresolved then unsupported ()
            if attr.AttributeType.IsProvided then raise OpaqueCompilerInput
            object ["type", text attr.AttributeType.FullName;
                    "arguments", attr.ConstructorArguments |> Seq.map (fun (typ, value) -> object ["type", text (typeName typ); "value", constant value]) |> array;
                    "named", attr.NamedArguments |> Seq.map (fun (typ, name, isField, value) -> object ["type", text (typeName typ); "name", text name; "field", flag isField; "value", constant value]) |> array]) |> array
    let private access (value: FSharpAccessibility) =
        object ["public", flag value.IsPublic; "private", flag value.IsPrivate; "internal", flag value.IsInternal; "protected", flag value.IsProtected]
    let private parameter (value: FSharpParameter) =
        object ["name", value.Name |> Option.map text |> Option.defaultValue null; "type", text (typeName value.Type);
                "optional", flag value.IsOptionalArg; "array", flag value.IsParamArrayArg; "in", flag value.IsInArg; "out", flag value.IsOutArg;
                "attributes", attributes value.Attributes]
    let private generic (value: FSharpGenericParameter) =
        if value.Constraints.Count <> 0 || value.IsMeasure || value.IsSolveAtCompileTime then unsupported ()
        object ["name", text value.Name; "attributes", attributes value.Attributes]
    let private inlineMember (value: FSharpMemberOrFunctionOrValue) =
        value.InlineAnnotation = FSharpInlineAnnotation.AlwaysInline || value.InlineAnnotation = FSharpInlineAnnotation.AggressiveInline
    let private inspect (workspace: Workspace) (id: string) =
        try
            let result = workspace.ProjectResult(id)
            if result.HasCriticalErrors || CompilerInputs.opaque result.ProjectContext then raise OpaqueCompilerInput
            if result.Diagnostics |> Array.exists (fun item -> item.Severity = FSharpDiagnosticSeverity.Error) then unsupported ()
            let inlines = Dictionary<string, string>(StringComparer.Ordinal)
            let expressionText (range: range) =
                let lines = workspace.Text(range.FileName).Split('\n')
                [ for line in range.StartLine .. range.EndLine do
                    let value = lines[line - 1]
                    let first = if line = range.StartLine then range.StartColumn else 0
                    let last = if line = range.EndLine then range.EndColumn else value.Length
                    yield value.Substring(first, last - first) ] |> String.concat "\n"
            let rec bodies declarations =
                for declaration in declarations do
                    match declaration with
                    | FSharpImplementationFileDeclaration.Entity(_, nested) -> bodies nested
                    | FSharpImplementationFileDeclaration.MemberOrFunctionOrValue(value, _, expression) when inlineMember value ->
                        if value.XmlDocSig = "" then unsupported ()
                        inlines.Add(value.XmlDocSig, expressionText expression.Range)
                    | _ -> ()
            for file in result.AssemblyContents.ImplementationFiles do bodies file.Declarations
            let rec entity (value: FSharpEntity) =
                // Complex type representations, providers and generic constraints
                // retain exact source reads until their compiler data is modeled.
                if value.IsProvided then raise OpaqueCompilerInput
                if value.IsUnresolved || not (value.IsNamespace || value.IsFSharpModule) || value.GenericParameters.Count <> 0 then unsupported ()
                let members = value.MembersFunctionsAndValues |> Seq.map (fun memberValue ->
                    if memberValue.IsUnresolved || not memberValue.IsModuleValueOrMember || memberValue.IsMember then unsupported ()
                    if inlineMember memberValue && not (inlines.ContainsKey memberValue.XmlDocSig) then unsupported ()
                    object ["key", text memberValue.XmlDocSig; "name", text memberValue.LogicalName; "compiled", text memberValue.CompiledName;
                            "type", text (typeName memberValue.FullType); "visibility", access memberValue.Accessibility;
                            "mutable", flag memberValue.IsMutable; "inline", text (string memberValue.InlineAnnotation);
                            "literal", memberValue.LiteralValue |> Option.map constant |> Option.defaultValue null;
                            "generics", memberValue.GenericParameters |> Seq.map generic |> array;
                            "parameters", memberValue.CurriedParameterGroups |> Seq.map (Seq.map parameter >> array) |> array;
                            "return", parameter memberValue.ReturnParameter; "attributes", attributes memberValue.Attributes]) |> array
                object ["key", text value.XmlDocSig; "name", text value.FullName; "compiled", text value.CompiledName;
                        "visibility", access value.Accessibility; "attributes", attributes value.Attributes; "members", members;
                        "nested", value.NestedEntities |> Seq.map entity |> array]
            let declarations = object ["attributes", attributes result.AssemblySignature.Attributes; "entities", result.AssemblySignature.Entities |> Seq.map entity |> array]
            let implementations = inlines |> Seq.sortBy (fun item -> item.Key) |> Seq.map (fun item -> object ["key", text item.Key; "body", text item.Value]) |> array
            Semantic(Map.ofList ["declarations", digest declarations; "inline", digest implementations])
        with
        | OpaqueCompilerInput -> SnapshotRequired
        | :? InvalidOperationException
        | :? ArgumentException
        | :? NotSupportedException -> SourceRequired
    let summary workspace id = match inspect workspace id with Semantic values -> Some values | _ -> None
    let key (session: JsonNode) id channel = strings [field session "provider"; field session "stage"; id; channel] |> fun value -> value.ToJsonString()
    let dependencies (workspace: Workspace) id =
        let visited = HashSet<string>(StringComparer.Ordinal)
        let rec visit current =
            for dependency in workspace.Projects[current].Dependencies do
                if visited.Add dependency then visit dependency
        visit id
        visited |> Seq.sort |> Seq.toArray
    let producers (workspace: Workspace) (session: JsonNode) id (reads: JsonNode) =
        let known = dependencies workspace id |> Seq.collect (fun dependency ->
            [ for channel in ["declarations"; "inline"] -> key session dependency channel, (dependency, channel) ]) |> Map.ofSeq
        items reads |> Seq.choose (fun read ->
            let readKey = field read "key"
            match known.TryFind readKey with
            | Some(dependency, channel) when field read "channel" = channel && field read "project" = field session "project" && isNull read["producer"] &&
                                             (["lookup"; "declaration"; "body"] |> List.contains (field read "kind")) ->
                Some(readKey, [| dependency |])
            | _ -> None)
        |> Seq.distinctBy fst |> Map.ofSeq
    let install (workspace: Workspace) (session: JsonNode) (template: JsonNode) id opaque =
        let sources = ResizeArray<JsonNode>()
        let reads = ResizeArray<JsonNode>()
        let mutable complete = not opaque
        if opaque then template["cacheable"] <- flag false
        let modelBound = items (template["direct_inputs"]) |> Seq.exists (fun input -> field input "kind" = "artifact" && field input "name" = workspace.ModelPath)
        let source project =
            for path in Seq.append workspace.Projects[project].Sources workspace.Projects[project].Inputs do
                match workspace.SourceObject path with
                | Some value -> sources.Add(object ["kind", text "file"; "name", text (Path.GetRelativePath(workspace.Root, path).Replace('\\', '/')); "object", text value; "type", text ""])
                | None when modelBound && workspace.Projects[project].Generated.Contains path -> ()
                | None -> complete <- false
        source id
        for dependency in dependencies workspace id do
            // Tree binaries and explicit provider inputs remain exact reads even
            // when declarations can use a semantic dependency.
            for path in workspace.Projects[dependency].Inputs do
                match workspace.SourceObject path with
                | Some value -> sources.Add(object ["kind", text "file"; "name", text (Path.GetRelativePath(workspace.Root, path).Replace('\\', '/')); "object", text value; "type", text ""])
                | None -> complete <- false
            match inspect workspace dependency with
            | Semantic values ->
                for KeyValue(channel, value) in values do
                    reads.Add(object ["kind", text (if channel = "inline" then "body" else "lookup"); "project", copy session["project"];
                                      "key", text (key session dependency channel); "value", text value; "channel", text channel; "scope", text dependency;
                                      "search_path", array []; "producers", array []])
            | SourceRequired -> source dependency
            | SnapshotRequired ->
                complete <- false
                template["cacheable"] <- flag false
        if complete then
            let retained = items (template["direct_inputs"]) |> Seq.filter (fun input -> field input "kind" <> "source") |> Seq.map copy |> Seq.toArray
            template["direct_inputs"] <- Seq.append retained sources |> array
            template["observed_dependencies"] <- Seq.append (items template["observed_dependencies"] |> Seq.map copy) reads |> array
    let observe (workspace: Workspace) (session: JsonNode) (reads: JsonNode) =
        let values = Dictionary<string, Map<string, string> option>(StringComparer.Ordinal)
        items reads |> Seq.choose (fun read ->
            let parts = JsonNode.Parse(field read "key") |> items |> Seq.map (fun item -> item.GetValue<string>()) |> Seq.toArray
            if parts.Length <> 4 || parts[0] <> field session "provider" || parts[1] <> field session "stage" || not (workspace.Projects.ContainsKey parts[2]) then None
            else
                let id, channel = parts[2], parts[3]
                if not (values.ContainsKey id) then values[id] <- summary workspace id
                values[id] |> Option.bind (Map.tryFind channel) |> Option.map (fun value ->
                    object ["kind", text "channel"; "name", copy read["key"]; "object", text value; "type", text ""])) |> array
