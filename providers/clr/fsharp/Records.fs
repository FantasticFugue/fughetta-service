namespace Fughetta

open System
open System.IO
open System.Text
open System.Security.Cryptography
open System.Collections.Generic
open System.Text.Json.Nodes
open FSharp.Compiler.Symbols
open FSharp.Compiler.Text
open FSharp.Compiler.Diagnostics
open FSharp.Compiler.Syntax
open Json

module Records =
    let private digest (value: string) = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(value)))
    let project (workspace: Workspace) (session: JsonNode) (template: JsonNode) (name: string) =
        let unit = workspace.Projects[name]
        let requestedFiles = template.["coverage"].["requested"].["files"]
        let requested = items requestedFiles |> Seq.map (fun value -> Path.GetFullPath(Path.Combine(workspace.Root, value.GetValue<string>()))) |> Set.ofSeq
        let result = workspace.Check(name, requested)
        let selected = Set.ofArray result.Sources
        let projectId, provider, unitId, context = field session "project", field session "provider", field template "id", field template "context"
        let identities, symbols = Dictionary<string, JsonNode>(), Dictionary<string, JsonNode>()
        let occurrences, references, diagnostics, scopes = ResizeArray<JsonNode>(), ResizeArray<JsonNode>(), ResizeArray<JsonNode>(), ResizeArray<JsonNode>()
        let typedRanges = ResizeArray<JsonNode>()
        let files = Dictionary<string, int array * int array * string>()
        let relative path = Path.GetRelativePath(workspace.Root, path).Replace('\\', '/')
        let coordinates (range: range) =
            let path = Path.GetFullPath range.FileName
            if not (files.ContainsKey path) then
                let content = workspace.Text(path)
                let lines = ResizeArray<int>()
                lines.Add(0)
                let offsets = Array.zeroCreate (content.Length + 1)
                let mutable i, scalar = 0, 0
                while i < content.Length do
                    offsets[i] <- scalar
                    if content[i] = '\n' then lines.Add(i + 1)
                    if Char.IsHighSurrogate content[i] && i + 1 < content.Length && Char.IsLowSurrogate content[i + 1] then
                        offsets[i + 1] <- scalar
                        i <- i + 1
                    i <- i + 1
                    scalar <- scalar + 1
                offsets[content.Length] <- scalar
                files[path] <- (lines.ToArray(), offsets, content)
            let lines, offsets, _ = files[path]
            let position line column = offsets[min (offsets.Length - 1) (lines[max 0 (min (lines.Length - 1) (line - 1))] + column)]
            object ["path", text (relative path); "begin", number (position range.StartLine range.StartColumn); "end", number (position range.EndLine range.EndColumn)]
        let location range = object ["source", coordinates range; "project", text projectId]
        let ownership (range: range) = object ["owner_unit", text unitId; "context", text context;
                                       "origin", object ["provider", text provider; "kind", text (if unit.Generated.Contains(Path.GetFullPath range.FileName) then "generated" else "source"); "location", location range]]
        let xmlKey (symbol: FSharpSymbol) =
            match symbol with
            | :? FSharpEntity as value -> value.XmlDocSig
            | :? FSharpMemberOrFunctionOrValue as value when value.IsModuleValueOrMember -> value.XmlDocSig
            | :? FSharpField as value -> if value.IsUnionCaseField then "" else value.XmlDocSig
            | :? FSharpUnionCase as value -> value.XmlDocSig
            | _ -> ""
        let sourceOwners = Dictionary<string, HashSet<string>>(StringComparer.Ordinal)
        for projectId in Seq.append [name] (Reads.dependencies workspace name) do
            let project = workspace.Projects[projectId]
            for dependency, reference in Array.zip project.Dependencies project.Options.ReferencedProjects do
                let output = Path.GetFullPath reference.OutputFile
                if not (sourceOwners.ContainsKey output) then sourceOwners[output] <- HashSet<string>()
                sourceOwners[output].Add dependency |> ignore
        let sourceModule (symbol: FSharpSymbol) =
            let declaration = symbol.DeclarationLocation |> Option.map (fun range -> Path.GetFullPath range.FileName)
            match symbol.Assembly.FileName with
            | None when declaration |> Option.exists (fun path -> Array.contains path unit.Sources) -> Some name
            | Some file ->
                match sourceOwners.TryGetValue(Path.GetFullPath file) with
                | true, owners ->
                    let matching = owners |> Seq.filter (fun owner -> declaration |> Option.forall (fun path -> Array.contains path workspace.Projects[owner].Sources)) |> Seq.toArray
                    match matching with
                    | [|owner|] -> Some owner
                    | [||] -> None
                    | _ -> failwith "FCS assembly reference has several source module owners"
                | _ -> None
            | _ -> None
        let activeGroups = ResizeArray<FSharpMemberOrFunctionOrValue * range>()
        let activeDeclarations = Dictionary<string * int, range>()
        let activeGroup (value: FSharpActivePatternCase) =
            let declaration = value.DeclarationLocation
            activeGroups
            |> Seq.filter (fun (_, range) ->
                range.FileName = declaration.FileName &&
                (range.StartLine, range.StartColumn) <= (declaration.StartLine, declaration.StartColumn) &&
                (range.EndLine, range.EndColumn) >= (declaration.EndLine, declaration.EndColumn))
            |> Seq.sortBy (fun (_, range) -> range.EndLine - range.StartLine, range.EndColumn - range.StartColumn)
            |> Seq.tryHead |> Option.map fst
        let rec declarationBinding (symbol: FSharpSymbol) =
            let key = xmlKey symbol
            let sourceOwner = sourceModule symbol
            let moduleId = sourceOwner |> Option.defaultValue symbol.Assembly.QualifiedName
            let identity =
                if key <> "" then
                    object ["project", text projectId; "module", text moduleId;
                            "provider", text provider; "domain", text "fsharp"; "native_key", text key]
                else
                    let declaration =
                        match symbol with
                        | :? FSharpActivePatternCase as value ->
                            activeGroup value |> Option.bind (fun owner ->
                                let ownerId, _ = declarationBinding owner
                                match activeDeclarations.TryGetValue((ownerId, value.Index)) with true, range -> Some range | _ -> None)
                            |> Option.orElse symbol.DeclarationLocation
                        | _ -> symbol.DeclarationLocation
                    match declaration with
                    | Some range -> object ["project", text projectId; "module", text (sourceOwner |> Option.defaultValue name); "provider", text provider;
                                            "anchor", object ["file", text (relative range.FileName); "declaration", text (sprintf "pending-%d-%d-%s" range.StartLine range.StartColumn symbol.DisplayName)]]
                    | None -> failwithf "F# declaration has no identity: %s" symbol.DisplayName
            "symbol-" + digest (identity.ToJsonString()), identity
        let identify (symbol: FSharpSymbol) =
            let id, identity = declarationBinding symbol
            identities.TryAdd(id, object ["symbol", text id; "declaration", identity]) |> ignore
            id
        let typeRecords = TypeRecords(identify)
        let types = typeRecords.Rows
        let typeRecord owner value = typeRecords.Project(owner, value)
        let bodies = Dictionary<string, range>()
        let bodyKinds = Dictionary<string, string>()
        let parameters = HashSet<string>()
        let implementations = ResizeArray<FSharpExpr>()
        let fileScope (range: range) = unitId + ":file:" + relative range.FileName
        let bodyScope owner = unitId + ":body:" + owner
        // A `let` sequence's typed range covers only its first binding, so the
        // body ends where its furthest subexpression does.
        let rec furthest (expression: FSharpExpr) =
            expression.ImmediateSubExpressions
            |> List.filter (fun child -> child.Range.FileName = expression.Range.FileName)
            |> List.map furthest
            |> List.fold (fun last child -> if Position.posGeq last child then last else child) expression.Range.End
        let rec declarations values =
            for declaration in values do
                match declaration with
                | FSharpImplementationFileDeclaration.Entity(_, nested) -> declarations nested
                | FSharpImplementationFileDeclaration.MemberOrFunctionOrValue(value, arguments, expression) ->
                    if not value.IsCompilerGenerated && selected.Contains(Path.GetFullPath expression.Range.FileName) then implementations.Add expression
                    if value.IsModuleValueOrMember && selected.Contains(Path.GetFullPath expression.Range.FileName) then
                        let start = (value :> FSharpSymbol).ImplementationLocation |> Option.defaultValue value.DeclarationLocation
                        let firstLine = if start.FileName = expression.Range.FileName then min start.StartLine expression.Range.StartLine else expression.Range.StartLine
                        let owner = identify value
                        let body = Range.mkRange expression.Range.FileName (Position.mkPos firstLine 0) (furthest expression)
                        bodies[owner] <- body
                        if value.IsActivePattern then activeGroups.Add(value, body)
                        bodyKinds[owner] <- if value.IsFunction || value.IsMethod || value.IsConstructor then "function" else "initializer"
                        for group in arguments do
                            for parameter in group do parameters.Add(fst (declarationBinding parameter)) |> ignore
                | _ -> ()
        for file in result.ImplementationFiles do declarations file.Declarations
        let enclosing (range: range) =
            bodies |> Seq.filter (fun pair -> pair.Value.FileName = range.FileName && (pair.Value.StartLine, pair.Value.StartColumn) <= (range.StartLine, range.StartColumn) && (pair.Value.EndLine, pair.Value.EndColumn) >= (range.EndLine, range.EndColumn))
                   |> Seq.sortBy (fun pair -> pair.Value.EndLine - pair.Value.StartLine, pair.Value.EndColumn - pair.Value.StartColumn)
                   |> Seq.tryHead |> Option.map (fun pair -> pair.Key)
        let uses = result.Uses
        for useOf in uses do
            match useOf.Symbol with
            | :? FSharpActivePatternCase as value when useOf.IsFromDefinition && selected.Contains(Path.GetFullPath useOf.Range.FileName) ->
                activeGroup value |> Option.iter (fun owner -> activeDeclarations[(identify owner, value.Index)] <- useOf.Range)
            | _ -> ()
        let isLocal (symbol: FSharpSymbol) =
            match symbol with
            | :? FSharpMemberOrFunctionOrValue as value -> not value.IsModuleValueOrMember && not value.IsMember
            | :? FSharpGenericParameter -> true
            | :? FSharpActivePatternCase as value ->
                activeGroup value |> Option.map (fun owner -> not owner.IsModuleValueOrMember) |> Option.defaultValue value.Group.DeclaringEntity.IsNone
            | _ -> false
        let access (symbol: FSharpSymbol) =
            let value = symbol.Accessibility
            if value.IsPublic then "public" elif value.IsPrivate then "private" elif value.IsInternal then "internal" elif value.IsProtected then "protected" else ""
        let indexOf (symbol: FSharpSymbol) values = values |> Seq.findIndex (fun (value: FSharpSymbol) -> value.IsEffectivelySameAs symbol)
        let entityOf (symbol: FSharpSymbol) =
            match symbol with
            | :? FSharpEntity as value -> value.DeclaringEntity
            | :? FSharpMemberOrFunctionOrValue as value -> value.DeclaringEntity
            | :? FSharpField as value -> value.DeclaringEntity
            | :? FSharpUnionCase as value -> Some value.DeclaringEntity
            | :? FSharpActivePatternCase as value ->
                value.Group.DeclaringEntity |> Option.orElseWith (fun () -> activeGroup value |> Option.bind (fun owner -> owner.DeclaringEntity))
            | _ -> None
        let declare (symbol: FSharpSymbol) range =
            let id = identify symbol
            let kind, valueType =
                match symbol with
                | :? FSharpEntity as value ->
                    (if value.IsFSharpModule then "module" elif value.IsNamespace then "namespace" else "type"),
                    (if value.IsFSharpModule || value.IsNamespace then None else Some(typeRecords.Classifier(id, value)))
                | :? FSharpMemberOrFunctionOrValue as value ->
                    (if parameters.Contains id then "parameter" elif value.IsConstructor then "constructor" elif value.IsProperty then "property" elif value.IsMember then "method" elif value.IsFunction then "function" else "variable"),
                    Some(if value.IsFunction || value.IsMethod || value.IsConstructor then typeRecords.Callable(id, value) else typeRecord id value.FullType)
                | :? FSharpField as value -> "field", Some(typeRecord id value.FieldType)
                | :? FSharpUnionCase as value -> "enum_member", Some(typeRecord id value.ReturnType)
                | :? FSharpGenericParameter as value -> "parameter", Some(typeRecords.Parameter(id, value))
                | :? FSharpActivePatternCase as value -> "enum_member", Some(typeRecord id value.Group.OverallType)
                | _ -> "other", None
            let localOwner = if isLocal symbol then enclosing range |> Option.filter (fun owner -> owner <> id) else None
            let declaringScope = localOwner |> Option.map bodyScope |> Option.defaultValue (fileScope range)
            let row = object ["id", text id; "ownership", ownership range; "language", text "fsharp"; "domain", text "fsharp"; "module", text name;
                              "name", text symbol.DisplayName; "qualified_name", text symbol.FullName; "kind", text kind;
                              "native_key", copy (identities[id]["declaration"]); "declaring_scope", text declaringScope;
                              "visibility", text (access symbol); "modifiers", strings (if isLocal symbol then ["local"] else []); "parameters", array [];
                              "extensions", object ["fsharp.assembly", text symbol.Assembly.SimpleName]]
            let extensions = row["extensions"]
            let modifiers = ResizeArray<string>(if isLocal symbol then ["local"] else [])
            valueType |> Option.iter (fun value -> row["type"] <- text value)
            localOwner |> Option.iter (fun owner -> row["owner"] <- text owner)
            entityOf symbol |> Option.iter (fun parent ->
                if localOwner.IsNone then row["owner"] <- text (identify parent)
                extensions["fsharp.declaring_entity"] <- text parent.XmlDocSig)
            match symbol with
            | :? FSharpEntity as value ->
                extensions["fsharp.compiled_name"] <- text value.CompiledName
                for enabled, label in [value.IsFSharpRecord, "record"; value.IsFSharpUnion, "union"; value.IsInterface, "interface";
                                       value.IsValueType, "struct"; value.IsAbstractClass, "abstract"; value.IsMeasure, "measure"; value.IsFSharpAbbreviation, "alias"] do
                    if enabled then modifiers.Add label
            | :? FSharpMemberOrFunctionOrValue as value ->
                extensions["fsharp.compiled_name"] <- text value.CompiledName
                for enabled, label in [value.IsMutable, "mutable"; value.IsMember && not value.IsInstanceMember, "static"; value.IsActivePattern, "active_pattern";
                                       value.IsDispatchSlot, "abstract"; value.IsOverrideOrExplicitInterfaceImplementation, "override"; value.IsCompilerGenerated, "compiler_generated";
                                       value.InlineAnnotation = FSharpInlineAnnotation.AlwaysInline || value.InlineAnnotation = FSharpInlineAnnotation.AggressiveInline, "inline"] do
                    if enabled then modifiers.Add label
                row["parameters"] <- value.CurriedParameterGroups |> Seq.mapi (fun group values ->
                    values |> Seq.mapi (fun index value ->
                        let parameter = object ["group", number group; "index", number index;
                                                "annotations", object ["fsharp.optional", flag value.IsOptionalArg; "fsharp.param_array", flag value.IsParamArrayArg;
                                                                       "fsharp.in", flag value.IsInArg; "fsharp.out", flag value.IsOutArg]]
                        value.Name |> Option.iter (fun name -> parameter["name"] <- text name)
                        parameter)) |> Seq.concat |> array
                // A DllImport extern binds by library and entry point, as the compiled P/Invoke row does.
                value.Attributes
                |> Seq.tryFind (fun attribute -> attribute.AttributeType.TryFullName = Some "System.Runtime.InteropServices.DllImportAttribute")
                |> Option.iter (fun attribute ->
                    let named key = attribute.NamedArguments |> Seq.tryPick (fun (_, name, _, argument) -> if name = key then Some argument else None)
                    modifiers.Add "pinvoke"
                    extensions["clr.pinvoke.library"] <- text (attribute.ConstructorArguments |> Seq.tryHead |> Option.map (snd >> string) |> Option.defaultValue "")
                    extensions["clr.pinvoke.entry_point"] <- text (named "EntryPoint" |> Option.map string |> Option.defaultValue value.CompiledName)
                    extensions["clr.pinvoke.convention"] <- text (
                        match named "CallingConvention" |> Option.map System.Convert.ToInt32 with
                        | Some 2 -> "cdecl"
                        | Some 3 -> "stdcall"
                        | Some 4 -> "thiscall"
                        | Some 5 -> "fastcall"
                        | _ -> "platform"))
                value.LiteralValue |> Option.iter (fun value ->
                    modifiers.Add "literal"
                    extensions["fsharp.constant_value"] <- if isNull value then null else System.Text.Json.JsonSerializer.SerializeToNode(value, value.GetType()))
            | :? FSharpUnionCase as value ->
                extensions["fsharp.compiled_name"] <- text value.CompiledName
                extensions["fsharp.sequence"] <- number (indexOf value (value.DeclaringEntity.UnionCases |> Seq.cast<FSharpSymbol>))
                extensions["fsharp.fields"] <- value.Fields |> Seq.map (fun field ->
                    object ["name", text field.Name; "type", text (typeRecord id field.FieldType)]) |> array
            | :? FSharpField as value ->
                if value.IsMutable then modifiers.Add "mutable"
                if value.IsNameGenerated then modifiers.Add "implicit"
                if value.IsUnionCaseField then
                    let caseValue = value.DeclaringUnionCase.Value
                    row["owner"] <- text (identify caseValue)
                    extensions["fsharp.declaring_entity"] <- text caseValue.DeclaringEntity.XmlDocSig
                    extensions["fsharp.variant"] <- number (indexOf caseValue (caseValue.DeclaringEntity.UnionCases |> Seq.cast<FSharpSymbol>))
                    extensions["fsharp.sequence"] <- number (indexOf value (caseValue.Fields |> Seq.cast<FSharpSymbol>))
                    extensions["fsharp.documentation_id"] <- text value.XmlDocSig
                else
                    value.DeclaringEntity |> Option.iter (fun parent -> extensions["fsharp.sequence"] <- number (indexOf value (parent.FSharpFields |> Seq.cast<FSharpSymbol>)))
            | :? FSharpActivePatternCase as value ->
                activeGroup value |> Option.iter (fun owner -> row["owner"] <- text (identify owner))
                modifiers.Add "active_pattern_case"
                extensions["fsharp.active_pattern"] <- object ["names", strings value.Group.Names; "index", number value.Index; "total", flag value.Group.IsTotal]
            | _ -> ()
            row["modifiers"] <- strings modifiers
            symbols[id] <- row
            let occurrence = object ["id", text (id + ":definition:" + string occurrences.Count); "ownership", ownership range;
                                     "symbol", text id; "location", location range; "roles", strings ["declaration"; "definition"]]
            if bodies.ContainsKey id then occurrence["body"] <- coordinates bodies[id]
            occurrences.Add(occurrence)
        for useOf in uses do
            if useOf.IsFromDefinition && selected.Contains(Path.GetFullPath useOf.Range.FileName) then
                let symbol = useOf.Symbol
                match symbol with
                | :? FSharpEntity as value when not value.IsNamespace && not value.IsFSharpModule ->
                    typeRecords.Register(identify symbol, value.AsType().FormatWithConstraints(FSharpDisplayContext.Empty), value.GenericParameters)
                | :? FSharpMemberOrFunctionOrValue as value ->
                    typeRecords.Register(identify symbol, value.FullType.FormatWithConstraints(FSharpDisplayContext.Empty), value.GenericParameters)
                | _ -> ()
        for useOf in uses do
            let range = useOf.Range
            if selected.Contains(Path.GetFullPath range.FileName) then
                let symbol = useOf.Symbol
                let id = identify symbol
                let missingDeclaration =
                    match symbol with
                    | :? FSharpGenericParameter when not (symbols.ContainsKey id) ->
                        symbol.DeclarationLocation |> Option.filter (fun value -> selected.Contains(Path.GetFullPath value.FileName))
                    | :? FSharpActivePatternCase as value when not (symbols.ContainsKey id) ->
                        activeGroup value |> Option.bind (fun owner ->
                            match activeDeclarations.TryGetValue((identify owner, value.Index)) with true, range -> Some range | _ -> None)
                        |> Option.orElse symbol.DeclarationLocation
                        |> Option.filter (fun value -> selected.Contains(Path.GetFullPath value.FileName))
                    | _ -> None
                if (useOf.IsFromDefinition || missingDeclaration.IsSome) && (not (symbol :? FSharpActivePatternCase) || not (symbols.ContainsKey id)) then
                    declare symbol (missingDeclaration |> Option.defaultValue range)
                if not useOf.IsFromDefinition then
                    let reference = object ["id", text (unitId + ":reference:" + string references.Count); "ownership", ownership range;
                                            "target", text id; "location", location range; "roles", strings ["reference"; if useOf.IsFromType then "type"];
                                            "resolution", text "resolved"; "candidates", array []]
                    enclosing range |> Option.iter (fun owner -> reference["from"] <- text owner)
                    references.Add(reference)
        // A binding states its type inside a typed pattern, `(x: T)` or `let x: T = ...`,
        // but not through a function's result annotation, `let f x: T = ...`.
        let annotated (range: range) =
            match result.ParseTrees.TryFind(Path.GetFullPath range.FileName) with
            | None -> false
            | Some tree ->
                let contains (outer: range) = Range.rangeContainsPos outer range.Start
                let visitor =
                    { new SyntaxVisitorBase<unit>() with
                        member _.VisitExpr(_, _, traverse, expression) = traverse expression
                        member _.VisitPat(_, traverse, pattern) =
                            match pattern with
                            | SynPat.Typed(_, _, whole) when contains whole -> Some ()
                            | _ -> traverse pattern
                        member _.VisitSimplePats(_, pattern) =
                            let rec typedIn pattern =
                                match pattern with
                                | SynPat.Typed(_, _, whole) when contains whole -> Some ()
                                | SynPat.Paren(inner, _) -> typedIn inner
                                | SynPat.Tuple(elementPats = elements) -> List.tryPick typedIn elements
                                | _ -> None
                            typedIn pattern
                        member _.VisitBinding(_, traverse, binding) =
                            match binding with
                            | SynBinding(headPat = SynPat.LongIdent(argPats = SynArgPats.Pats(_ :: _))) -> traverse binding
                            | SynBinding(headPat = head; returnInfo = Some _) when contains head.Range -> Some ()
                            | _ -> traverse binding }
                (SyntaxTraversal.Traverse(range.Start, tree, visitor)).IsSome
        let typed (range: range) (shown: FSharpType) overload substitutions =
            let owner = enclosing range |> Option.defaultValue unitId
            let row = object ["ownership", ownership range; "range", coordinates range; "type", text (typeRecord owner shown)
                              "substitutions", object (substitutions |> List.map (fun (parameter, argument) -> parameter, text (typeRecord owner argument)))]
            overload |> Option.iter (fun id -> row["overload"] <- text id)
            typedRanges.Add(row)
        let spelled = Dictionary<string, ResizeArray<range>>()
        for useOf in uses do
            let range = useOf.Range
            match useOf.Symbol with
            | :? FSharpMemberOrFunctionOrValue as value when selected.Contains(Path.GetFullPath range.FileName) ->
                if useOf.IsFromDefinition then
                    if isLocal (value :> FSharpSymbol) && not value.IsCompilerGenerated && not value.IsMemberThisValue && not value.IsConstructorThisValue && not (annotated range) then
                        typed range value.FullType None []
                else
                    let id = identify value
                    if not (spelled.ContainsKey id) then spelled[id] <- ResizeArray()
                    spelled[id].Add range
            | _ -> ()
        // A call whose callee the source names (not one the compiler inserts) and that
        // is generic, overloaded or virtual; operators such as `a + 1` stay out.
        let rec calls (expression: FSharpExpr) =
            let call (callee: FSharpMemberOrFunctionOrValue) (outer: FSharpType list) (own: FSharpType list) =
                let id = identify callee
                let overloaded = callee.IsMember && callee.GetOverloads(false) |> Option.exists (fun overloads -> overloads.Count > 1)
                let named = match spelled.TryGetValue id with | true, ranges -> ranges |> Seq.exists (Range.rangeContainsRange expression.Range) | _ -> false
                if named && not (callee.CompiledName.StartsWith "op_") && (not own.IsEmpty || overloaded || callee.IsDispatchSlot) then
                    let parameters = Seq.toList callee.GenericParameters
                    let arguments = outer @ own
                    let count = min parameters.Length arguments.Length
                    let substitutions =
                        List.zip (List.skip (parameters.Length - count) parameters) (List.skip (arguments.Length - count) arguments)
                        |> List.map (fun (parameter, argument) -> typeRecords.Parameter(id, parameter), argument)
                    typed expression.Range expression.Type (Some id) substitutions
            match expression with
            | FSharpExprPatterns.Call(_, callee, outer, own, _) -> call callee outer own
            | FSharpExprPatterns.NewObject(callee, outer, _) -> call callee outer []
            | _ -> ()
            for child in expression.ImmediateSubExpressions do calls child
        for expression in implementations do calls expression
        let rec ensureEntity (entity: FSharpEntity) =
            let ensure (symbol: FSharpSymbol) =
                match symbol.DeclarationLocation with
                | Some range when selected.Contains(Path.GetFullPath range.FileName) && not (symbols.ContainsKey (fst (declarationBinding symbol))) -> declare symbol range
                | _ -> ()
            for field in entity.FSharpFields do ensure field
            for caseValue in entity.UnionCases do
                ensure caseValue
                for field in caseValue.Fields do ensure field
            for nested in entity.NestedEntities do ensureEntity nested
        let sourceEntities = uses |> Seq.choose (fun useOf ->
            match useOf.Symbol with
            | :? FSharpEntity as entity when useOf.IsFromDefinition && selected.Contains(Path.GetFullPath useOf.Range.FileName) -> Some entity
            | _ -> None)
        for entity in sourceEntities do ensureEntity entity
        let knownSources = workspace.Projects |> Map.toSeq |> Seq.collect (fun (_, project) -> project.Sources) |> Set.ofSeq
        let projectFiles = workspace.Projects |> Map.toSeq |> Seq.map (fun (_, project) -> project.Options.ProjectFileName) |> Set.ofSeq
        for diagnostic in result.Diagnostics do
            let path = if String.IsNullOrWhiteSpace diagnostic.FileName then None else Some(Path.GetFullPath diagnostic.FileName)
            let outsideSelection = path |> Option.exists (fun file -> knownSources.Contains file && not (selected.Contains file))
            let sourceLocation = path |> Option.exists (fun file -> selected.Contains file && diagnostic.StartLine > 0 && not diagnostic.Range.IsSynthetic)
            let compilerLocation = diagnostic.FileName = Range.rangeStartup.FileName || diagnostic.FileName = Range.rangeCmdArgs.FileName
            let projectLocation = compilerLocation || String.IsNullOrWhiteSpace diagnostic.FileName || diagnostic.StartLine <= 0 || diagnostic.Range.IsSynthetic || (path |> Option.exists projectFiles.Contains)
            if not outsideSelection && (sourceLocation || projectLocation) then
                let place = if sourceLocation then location diagnostic.Range else object ["project", text projectId]
                let origin = if sourceLocation then ownership diagnostic.Range else
                                object ["owner_unit", text unitId; "context", text context; "origin", object ["provider", text provider; "kind", text "unknown"]]
                diagnostics.Add(object ["id", text (unitId + ":diagnostic:" + string diagnostics.Count); "producer", text provider;
                                        "code", text diagnostic.ErrorNumberText; "severity", text (diagnostic.Severity.ToString().ToLowerInvariant()); "message", text diagnostic.Message;
                                        "ownership", origin; "location", place; "related_locations", array []; "fixes", object []])
        for path in result.Sources do
            let content = workspace.Text(path)
            let range = Range.mkRange path (Position.mkPos 1 0) (Position.mkPos (content.Split('\n').Length) (content.Split('\n') |> Array.last |> String.length))
            let scopeId = unitId + ":file:" + relative path
            scopes.Add(object ["id", text scopeId; "ownership", ownership range; "location", location range; "kind", text "translation_unit"; "lookup", array [];
                               "declared_members", symbols.Values |> Seq.filter (fun row -> field row "declaring_scope" = scopeId) |> Seq.map (fun row -> copy row["id"]) |> array])
        for KeyValue(owner, range) in bodies do
            let scopeId = bodyScope owner
            scopes.Add(object ["id", text scopeId; "ownership", ownership range; "location", location range; "kind", text bodyKinds[owner];
                               "lexical_parent", text (fileScope range); "semantic_owner", text owner; "lookup", array [];
                               "declared_members", symbols.Values |> Seq.filter (fun row -> field row "declaring_scope" = scopeId) |> Seq.map (fun row -> copy row["id"]) |> array])
        for reference in references do
            if symbols.ContainsKey(field reference "target") then reference["target_context"] <- text context
        let unitValue = copy template
        Reads.install workspace session unitValue name result.OpaqueInputs
        let rows = ["symbols", symbols.Values :> seq<JsonNode>; "types", types.Values :> seq<JsonNode>; "occurrences", occurrences;
                    "references", references; "scopes", scopes; "diagnostics", diagnostics]
        let contributions = object ["files", result.Sources |> Seq.map relative |> strings]
        for key, values in rows do contributions[key] <- values |> Seq.map (fun row -> copy row["id"]) |> array
        contributions["relations"] <- array []
        unitValue["contributions"] <- contributions
        let facts = ["symbols"; "types"; "definitions"; "occurrences"; "references"; "scopes"; "diagnostics"; "typed_ranges"]
        unitValue["coverage"] <- object ["requested", copy (template["coverage"]["requested"]);
                                          "available", array [object ["files", result.Sources |> Seq.map relative |> strings; "modules", strings [name];
                                                                      "contexts", strings [context]; "projects", strings [projectId]; "facts", strings facts]];
                                          "missing", array []; "unresolved_count", number 0]
        object (["id", text ""; "unit", unitValue; "identities", array identities.Values; "relations", array []; "typed_ranges", array typedRanges] @ (rows |> List.map (fun (key, values) -> key, array values)))
