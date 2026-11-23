namespace Fughetta

open System
open System.Text
open System.Security.Cryptography
open System.Collections.Generic
open System.Text.Json.Nodes
open FSharp.Compiler.Symbols
open Json

type TypeRecords(identify: FSharpSymbol -> string) =
    let rows = Dictionary<string, JsonNode>()
    let parameterOwners = Dictionary<FSharpGenericParameter, string * int * string>(
        { new IEqualityComparer<FSharpGenericParameter> with
            member _.Equals(left, right) = left.IsEffectivelySameAs right
            member _.GetHashCode(value) = value.GetEffectivelySameAsHash() })
    let identifier (key: string) = "type-" + Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(key)))
    let fresh id kind name =
        let row = object ["id", text id; "domain", text "fsharp"; "kind", text kind; "name", text name; "extensions", object []]
        for key in ["arguments"; "constraints"; "elements"; "type_parameters"; "parameter_groups"; "qualifiers"; "application_arguments"] do row[key] <- array []
        rows[id] <- row
        row
    let rec project owner (value: FSharpType) =
        if value.IsGenericParameter then parameter owner value.GenericParameter
        else
            let name = value.Format(FSharpDisplayContext.Empty)
            let id = identifier (owner + ":" + value.FormatWithConstraints(FSharpDisplayContext.Empty))
            if not (rows.ContainsKey id) then
                let row = fresh id "unresolved" name
                if value.IsMeasureType then row["qualifiers"] <- strings ["measure"]
                if value.IsFunctionType then
                    row["kind"] <- text "function"
                    row["parameter_groups"] <- array [object ["parameters", array [object ["type", text (project owner value.GenericArguments[0]); "modifiers", array []]]]]
                    row["return_type"] <- text (project owner value.GenericArguments[1])
                elif value.IsTupleType then
                    row["kind"] <- text "tuple"
                    row["elements"] <- value.GenericArguments |> Seq.map (project owner) |> strings
                    if value.IsStructTupleType then row["qualifiers"] <- strings ["struct"]
                elif value.IsAbbreviation then
                    row["kind"] <- text "alias"
                    row["declaration"] <- text (identify value.TypeDefinition)
                    row["arguments"] <- strings [project owner value.AbbreviatedType]
                elif value.HasTypeDefinition && value.TypeDefinition.IsArrayType then
                    row["kind"] <- text "array"
                    row["arguments"] <- value.GenericArguments |> Seq.map (project owner) |> strings
                    row["extensions"] <- object ["fsharp.array_rank", number value.TypeDefinition.ArrayRank]
                elif value.HasTypeDefinition then
                    row["kind"] <- text (if value.GenericArguments.Count > 0 then "application" else "named")
                    row["declaration"] <- text (identify value.TypeDefinition)
                    row["arguments"] <- value.GenericArguments |> Seq.map (project owner) |> strings
            id
    and parameter owner (value: FSharpGenericParameter) =
        let binder = match parameterOwners.TryGetValue value with true, binding -> Some binding | _ -> None
        let key = match binder with Some(declaration, index, shape) -> declaration + ":parameter:" + string index + ":" + shape | None -> owner + ":parameter:" + value.Name
        let id = identifier key
        if not (rows.ContainsKey id) then
            let row = fresh id "parameter" value.Name
            binder |> Option.iter (fun (declaration, index, _) -> row["binder"] <- text declaration; row["parameter_index"] <- number index)
            let qualifiers = ResizeArray<string>()
            if value.IsMeasure then qualifiers.Add("measure")
            if value.IsSolveAtCompileTime then qualifiers.Add("statically_resolved")
            let constraints = ResizeArray<string>()
            let descriptions = ResizeArray<JsonNode>()
            let target kind typ =
                let target = project owner typ
                constraints.Add target
                descriptions.Add(object ["kind", text kind; "target", text target])
            for constraintValue in value.Constraints do
                if constraintValue.IsCoercesToConstraint then target "subtype" constraintValue.CoercesToTarget
                elif constraintValue.IsDefaultsToConstraint then
                    let data = constraintValue.DefaultsToConstraintData
                    let targetId = project owner data.DefaultsToTarget
                    constraints.Add targetId
                    descriptions.Add(object ["kind", text "defaults_to"; "target", text targetId; "priority", number data.DefaultsToPriority])
                elif constraintValue.IsMemberConstraint then
                    let data = constraintValue.MemberConstraintData
                    let sources = data.MemberSources |> Seq.map (project owner) |> Seq.toArray
                    let arguments = data.MemberArgumentTypes |> Seq.map (project owner) |> Seq.toArray
                    let returned = project owner data.MemberReturnType
                    constraints.AddRange sources; constraints.AddRange arguments; constraints.Add returned
                    descriptions.Add(object ["kind", text "member"; "name", text data.MemberName; "static", flag data.MemberIsStatic;
                                             "sources", strings sources; "parameters", strings arguments; "result", text returned])
                elif constraintValue.IsSimpleChoiceConstraint then
                    let choices = constraintValue.SimpleChoices |> Seq.map (project owner) |> Seq.toArray
                    constraints.AddRange choices
                    descriptions.Add(object ["kind", text "choice"; "types", strings choices])
                elif constraintValue.IsEnumConstraint then target "enum" constraintValue.EnumConstraintTarget
                elif constraintValue.IsDelegateConstraint then
                    let data = constraintValue.DelegateConstraintData
                    let argument, returned = project owner data.DelegateTupledArgumentType, project owner data.DelegateReturnType
                    constraints.Add argument; constraints.Add returned
                    descriptions.Add(object ["kind", text "delegate"; "parameter", text argument; "result", text returned])
                else
                    for enabled, kind in [constraintValue.IsSupportsNullConstraint, "supports_null"; constraintValue.IsNotSupportsNullConstraint, "not_null";
                                          constraintValue.IsNonNullableValueTypeConstraint, "value_type"; constraintValue.IsReferenceTypeConstraint, "reference_type";
                                          constraintValue.IsRequiresDefaultConstructorConstraint, "default_constructor"; constraintValue.IsComparisonConstraint, "comparison";
                                          constraintValue.IsEqualityConstraint, "equality"; constraintValue.IsUnmanagedConstraint, "unmanaged";
                                          constraintValue.IsAllowsRefStructConstraint, "allows_ref_struct"] do
                        if enabled then
                            qualifiers.Add kind
                            descriptions.Add(object ["kind", text kind])
            row["qualifiers"] <- strings qualifiers
            row["constraints"] <- constraints |> Seq.distinct |> strings
            row["extensions"] <- object ["fsharp.constraints", array descriptions]
        id
    let register owner shape (parameters: seq<FSharpGenericParameter>) =
        parameters |> Seq.iteri (fun index value -> parameterOwners[value] <- owner, index, shape)
    let parameterGroup owner (values: seq<FSharpParameter>) =
        values |> Seq.map (fun value ->
            let modifiers = [if value.IsOptionalArg then "optional"; if value.IsParamArrayArg then "param_array"; if value.IsInArg then "in"; if value.IsOutArg then "out"]
            object ["type", text (project owner value.Type); "modifiers", strings modifiers]) |> array |> fun values -> object ["parameters", values]
    member _.Rows = rows
    member _.Register(owner, shape, parameters) = register owner shape parameters
    member _.Project(owner, value) = project owner value
    member _.Parameter(owner, value) = parameter owner value
    member _.Callable(owner, value: FSharpMemberOrFunctionOrValue) =
        let shape = value.FullType.FormatWithConstraints(FSharpDisplayContext.Empty)
        register owner shape value.GenericParameters
        let id = identifier (owner + ":callable:" + shape)
        if not (rows.ContainsKey id) then
            let kind = if value.CurriedParameterGroups.Count > 1 then "fsharp:curried_signature" else "function"
            let row = fresh id kind (value.FullType.Format(FSharpDisplayContext.Empty))
            row["parameter_groups"] <- value.CurriedParameterGroups |> Seq.map (parameterGroup owner) |> array
            row["return_type"] <- text (project owner value.ReturnParameter.Type)
            row["type_parameters"] <- value.GenericParameters |> Seq.map (parameter owner) |> strings
            if value.IsInstanceMember then row["receiver"] <- text (project owner value.ApparentEnclosingType)
        id
    member _.Classifier(owner, value: FSharpEntity) =
        let typ = value.AsType()
        let shape = typ.FormatWithConstraints(FSharpDisplayContext.Empty)
        register owner shape value.GenericParameters
        let id = identifier (owner + ":classifier:" + shape)
        if not (rows.ContainsKey id) then
            let row = fresh id (if value.IsFSharpAbbreviation then "alias" else "named") (typ.Format(FSharpDisplayContext.Empty))
            row["declaration"] <- text owner
            row["type_parameters"] <- value.GenericParameters |> Seq.map (parameter owner) |> strings
            if value.IsFSharpAbbreviation then row["arguments"] <- strings [project owner value.AbbreviatedType]
            else
                row["constraints"] <- Seq.append (value.BaseType |> Option.toList) value.DeclaredInterfaces |> Seq.map (project owner) |> strings
            if value.IsMeasure then row["qualifiers"] <- strings ["measure"]
        id
