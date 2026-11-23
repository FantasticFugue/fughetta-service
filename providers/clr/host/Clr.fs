namespace Fughetta

open System
open System.IO
open System.Reflection
open System.Reflection.Emit
open System.Reflection.Metadata
open System.Reflection.Metadata.Ecma335
open System.Reflection.PortableExecutable
open System.Runtime.InteropServices
open System.Collections.Generic
open System.Collections.Immutable
open System.Text.Json.Nodes
open Json

module Clr =
    let private token (handle: EntityHandle) = "token-" + string (MetadataTokens.GetToken handle)
    let private typeName (reader: MetadataReader) (handle: TypeDefinitionHandle) =
        let rec name handle =
            let row = reader.GetTypeDefinition(handle)
            let outer = row.GetDeclaringType()
            if not outer.IsNil then name outer + "+" + reader.GetString(row.Name)
            else
                let ns = reader.GetString(row.Namespace)
                (if ns = "" then "" else ns + ".") + reader.GetString(row.Name)
        name handle
    let private referenceName (reader: MetadataReader) (handle: TypeReferenceHandle) =
        let rec name handle =
            let row = reader.GetTypeReference(handle)
            let simple = (let ns = reader.GetString(row.Namespace) in (if ns = "" then "" else ns + ".") + reader.GetString(row.Name))
            match row.ResolutionScope.Kind with
            | HandleKind.TypeReference -> name (TypeReferenceHandle.op_Explicit row.ResolutionScope) + "+" + reader.GetString(row.Name)
            | HandleKind.AssemblyReference ->
                let assembly = reader.GetAssemblyReference(AssemblyReferenceHandle.op_Explicit row.ResolutionScope)
                "[" + reader.GetString(assembly.Name) + "," + assembly.Version.ToString() + "]" + simple
            | _ -> simple
        name handle

    type private Signatures(reader: MetadataReader) =
        let records = Dictionary<string, JsonNode>()
        let add id kind name arguments declaration binder index =
            if not (records.ContainsKey id) then
                let row = object ["id", text id; "domain", text "clr"; "kind", text kind; "name", text name;
                                  "arguments", strings arguments; "constraints", array []; "elements", array []; "type_parameters", array [];
                                  "parameter_groups", array []; "qualifiers", array []; "application_arguments", array []; "extensions", object []]
                declaration |> Option.iter (fun value -> row["declaration"] <- text value)
                binder |> Option.filter ((<>) "") |> Option.iter (fun value -> row["binder"] <- text value)
                index |> Option.iter (fun value -> row["parameter_index"] <- number value)
                records[id] <- row
            id
        let named name declaration = add name "named" name [] declaration None None
        member _.Records = records.Values
        interface ISignatureTypeProvider<string, string * string> with
            member _.GetPrimitiveType(code) =
                let name = "System." + (if code = PrimitiveTypeCode.TypedReference then "TypedReference" else code.ToString())
                add name "primitive" name [] None None None
            member _.GetTypeFromDefinition(_, handle, _) = named (typeName reader handle) (Some(token (TypeDefinitionHandle.op_Implicit handle)))
            member _.GetTypeFromReference(_, handle, _) = named (referenceName reader handle) (Some(token (TypeReferenceHandle.op_Implicit handle)))
            member this.GetTypeFromSpecification(_, context, handle, _) = reader.GetTypeSpecification(handle).DecodeSignature(this, context)
            member _.GetGenericInstantiation(genericType, arguments) =
                let args = arguments |> Seq.toList
                add (genericType + "<" + String.concat "," args + ">") "application" genericType args None None None
            member _.GetGenericMethodParameter((_, owner), index) = add (owner + "!!" + string index) "parameter" ("M" + string index) [] None (Some owner) (Some index)
            member _.GetGenericTypeParameter((owner, _), index) = add (owner + "!" + string index) "parameter" ("T" + string index) [] None (Some owner) (Some index)
            member _.GetArrayType(element, shape) = add (element + "[" + string shape.Rank + "]") "array" "" [element] None None None
            member _.GetSZArrayType(element) = add (element + "[]") "array" "" [element] None None None
            member _.GetByReferenceType(element) = add (element + "&") "reference" "" [element] None None None
            member _.GetPointerType(element) = add (element + "*") "pointer" "" [element] None None None
            member _.GetModifiedType(modifier, element, required) =
                add (element + (if required then " modreq(" else " modopt(") + modifier + ")") "application" modifier [element] None None None
            member _.GetPinnedType(element) = add (element + " pinned") "reference" "pinned" [element] None None None
            member _.GetFunctionPointerType(signature) =
                let args = signature.ParameterTypes |> Seq.toList
                let id = "fn(" + String.concat "," args + ")->" + signature.ReturnType
                add id "function" "" [] None None None |> ignore
                records[id]["parameter_groups"] <- array [object ["parameters", args |> Seq.map (fun value -> object ["type", text value; "modifiers", array []]) |> array]]
                records[id]["return_type"] <- text signature.ReturnType
                id

    let private documentationName (value: string) =
        let value = if value.StartsWith("[") then value.Substring(value.IndexOf(']') + 1) else value
        value.Replace('+', '.')

    type private DocumentationSignatures(reader: MetadataReader) =
        let instantiations = Dictionary<string, string list>()
        member _.Arguments name = match instantiations.TryGetValue name with true, values -> values | _ -> []
        interface ISignatureTypeProvider<string, string list> with
            member _.GetPrimitiveType(code) = "System." + code.ToString()
            member _.GetTypeFromDefinition(_, handle, _) = documentationName (typeName reader handle)
            member _.GetTypeFromReference(_, handle, _) = documentationName (referenceName reader handle)
            member this.GetTypeFromSpecification(_, context, handle, _) = reader.GetTypeSpecification(handle).DecodeSignature(this, context)
            member _.GetGenericInstantiation(genericType, arguments) =
                let mutable at = 0
                let result = System.Text.RegularExpressions.Regex.Replace(genericType, @"`([0-9]+)", System.Text.RegularExpressions.MatchEvaluator(fun count ->
                    let size = Int32.Parse(count.Groups[1].Value)
                    let selected = arguments |> Seq.skip at |> Seq.take size |> String.concat ","
                    at <- at + size
                    "{" + selected + "}"))
                instantiations[result] <- Seq.toList arguments
                result
            member _.GetGenericMethodParameter(_, index) = "``" + string index
            member _.GetGenericTypeParameter(arguments, index) = arguments |> List.tryItem index |> Option.defaultValue ("`" + string index)
            member _.GetArrayType(element, shape) = element + "[" + (Seq.replicate shape.Rank "0:" |> String.concat ",") + "]"
            member _.GetSZArrayType(element) = element + "[]"
            member _.GetByReferenceType(element) = element + "@"
            member _.GetPointerType(element) = element + "*"
            member _.GetModifiedType(_, element, _) = element
            member _.GetPinnedType(element) = element
            member _.GetFunctionPointerType(_) = "!function-pointer!"

    let read (path: string) (pdb: string option) =
        use input = File.OpenRead(path)
        use pe = new PEReader(input)
        let metadata = pe.GetMetadataReader()
        let signatures = Signatures(metadata)
        let provider = signatures :> ISignatureTypeProvider<string, string * string>
        let documentationTypes = DocumentationSignatures(metadata)
        let documentation = documentationTypes :> ISignatureTypeProvider<string, string list>
        let entities, calls, relations = ResizeArray<JsonNode>(), ResizeArray<JsonNode>(), ResizeArray<JsonNode>()
        let unresolved = ResizeArray<string>()
        let known = Dictionary<string, JsonNode>()
        let layouts = Dictionary<string, JsonNode>()
        let descriptor (handle: BlobHandle) =
            if handle.IsNil then None
            else
                let data = metadata.GetBlobBytes(handle)
                let mutable blob = metadata.GetBlobReader(handle)
                let nativeType = int (blob.ReadByte())
                let result = object ["native_type", number nativeType; "name", text ((enum<UnmanagedType> nativeType).ToString());
                                     "data", data |> Seq.map (int >> number) |> array]
                let integer (name: string) = if blob.RemainingBytes > 0 then result[name] <- number (blob.ReadCompressedInteger())
                match nativeType with
                | 0x17 -> integer "size_constant" // FIXEDSYSSTRING
                | 0x1e -> // FIXEDARRAY
                    integer "size_constant"
                    if blob.RemainingBytes > 0 then result["element_type"] <- number (int (blob.ReadByte()))
                | 0x2a -> // ARRAY: element, size parameter, constant, flags
                    if blob.RemainingBytes > 0 then result["element_type"] <- number (int (blob.ReadByte()))
                    integer "size_parameter"
                    integer "size_constant"
                | 0x2c -> // CUSTOMMARSHALER: GUID, native name, managed type, cookie
                    blob.ReadSerializedString() |> ignore
                    blob.ReadSerializedString() |> ignore
                    result["marshaler"] <- text (blob.ReadSerializedString())
                    result["cookie"] <- text (blob.ReadSerializedString())
                | _ -> ()
                Some result
        let add id key name kind owner flags imported returnType parameters =
            match known.TryGetValue id with
            | true, row -> row
            | _ ->
                let row = object ["id", text id; "key", text key; "name", text name; "kind", text kind; "owner", text owner;
                                  "flags", number flags; "imported", flag imported; "return_type", text returnType; "parameters", strings parameters;
                                  "locations", array []; "library", text ""; "entry_point", text ""; "convention", text "platform";
                                  "marshalling_parameters", array []; "documentation_id", text ""; "fsharp_documentation_id", text ""; "accessor_role", text ""; "visibility", text ""]
                known[id] <- row
                entities.Add(row)
                row
        let rec ownerName (handle: EntityHandle) =
            match handle.Kind with
            | HandleKind.TypeDefinition -> typeName metadata (TypeDefinitionHandle.op_Explicit handle)
            | HandleKind.TypeReference -> referenceName metadata (TypeReferenceHandle.op_Explicit handle)
            | HandleKind.TypeSpecification -> metadata.GetTypeSpecification(TypeSpecificationHandle.op_Explicit handle).DecodeSignature(provider, ("", ""))
            | _ -> "<module>"
        let stableType value = System.Text.RegularExpressions.Regex.Replace(value, @"token-[0-9]+(?=!!?[0-9]+)", "")
        let methodKey owner name (signature: MethodSignature<string>) = stableType (owner + "." + name + "(" + String.concat "," signature.ParameterTypes + ")->" + signature.ReturnType)
        let typeUse (handle: EntityHandle) =
            match handle.Kind with
            | HandleKind.TypeDefinition -> provider.GetTypeFromDefinition(metadata, TypeDefinitionHandle.op_Explicit handle, 0uy)
            | HandleKind.TypeReference -> provider.GetTypeFromReference(metadata, TypeReferenceHandle.op_Explicit handle, 0uy)
            | HandleKind.TypeSpecification -> provider.GetTypeFromSpecification(metadata, ("", ""), TypeSpecificationHandle.op_Explicit handle, 0uy)
            | _ -> ""
        let typeTarget (handle: EntityHandle) =
            if handle.IsNil then ""
            else
                let id = token handle
                if not (known.ContainsKey id) then
                    let name = ownerName handle
                    if handle.Kind = HandleKind.TypeDefinition then
                        let row = metadata.GetTypeDefinition(TypeDefinitionHandle.op_Explicit handle)
                        let outer = row.GetDeclaringType()
                        add id (stableType name) (metadata.GetString row.Name) "type" (if outer.IsNil then "" else token (TypeDefinitionHandle.op_Implicit outer)) (int row.Attributes) false (typeUse handle) [] |> ignore
                    else add id (stableType name) name "type" "" 0 true (typeUse handle) [] |> ignore
                id
        let parameters (types: seq<string>) =
            let types = Seq.toList types
            if types.IsEmpty then "" else "(" + String.concat "," types + ")"
        let methodDocumentation owner name (signature: MethodSignature<string>) =
            let name = if name = ".ctor" then "#ctor" elif name = ".cctor" then "#cctor" else name.Replace('.', '#')
            let generic = if signature.GenericParameterCount = 0 then "" else "``" + string signature.GenericParameterCount
            let result = "M:" + documentationName owner + "." + name + generic + parameters signature.ParameterTypes
            if name = "op_Implicit" || name = "op_Explicit" then result + "~" + signature.ReturnType else result
        let fsharpDocumentation owner (name: string) (signature: MethodSignature<string>) =
            let name = if name = ".ctor" then "#ctor" elif name = ".cctor" then "#cctor" else name
            let generic = if signature.GenericParameterCount = 0 then "" else "``" + string signature.GenericParameterCount
            "M:" + documentationName owner + "." + name + generic + parameters signature.ParameterTypes
        let document (entity: JsonNode) (value: string) =
            if not (value.Contains("!function-pointer!")) then entity["documentation_id"] <- text value
        let relate kind source target resolution =
            if source <> "" && target <> "" then relations.Add(object ["kind", text kind; "from", text source; "target", text target; "resolution", text resolution])
        let memberVisibility (flags: int) =
            match flags &&& 7 with
            | 6 -> "public" | 5 -> "protected_internal" | 4 -> "protected" | 3 -> "internal" | 2 -> "private_protected" | 1 -> "private" | _ -> ""
        let rec methodTarget (handle: EntityHandle) =
            let id = token handle
            match handle.Kind with
            | HandleKind.MethodSpecification -> methodTarget (metadata.GetMethodSpecification(MethodSpecificationHandle.op_Explicit handle).Method)
            | HandleKind.MethodDefinition ->
                let method = metadata.GetMethodDefinition(MethodDefinitionHandle.op_Explicit handle)
                let parent: EntityHandle = TypeDefinitionHandle.op_Implicit (method.GetDeclaringType())
                let name = metadata.GetString(method.Name)
                let signature = method.DecodeSignature(provider, (token parent, id))
                add id (methodKey (ownerName parent) name signature) name (if name = ".ctor" || name = ".cctor" then "constructor" else "method")
                    (token parent) (int method.Attributes) false signature.ReturnType signature.ParameterTypes |> ignore
                known[id]["visibility"] <- text (memberVisibility (int method.Attributes))
                document known[id] (methodDocumentation (ownerName parent) name (method.DecodeSignature(documentation, [])))
                known[id]["fsharp_documentation_id"] <- text (fsharpDocumentation (ownerName parent) name (method.DecodeSignature(documentation, [])))
                id
            | HandleKind.MemberReference ->
                let memberRef = metadata.GetMemberReference(MemberReferenceHandle.op_Explicit handle)
                let parent = memberRef.Parent
                let name = metadata.GetString(memberRef.Name)
                match parent.Kind with
                | HandleKind.TypeDefinition | HandleKind.TypeReference | HandleKind.TypeSpecification -> typeTarget parent |> ignore
                | _ -> ()
                let signature = memberRef.DecodeMethodSignature(provider, (token parent, id))
                add id (methodKey (ownerName parent) name signature) name (if name = ".ctor" || name = ".cctor" then "constructor" else "method")
                    (token parent) 0 true signature.ReturnType signature.ParameterTypes |> ignore
                document known[id] (methodDocumentation (ownerName parent) name (memberRef.DecodeMethodSignature(documentation, [])))
                id
            | _ -> ""
        for handle in metadata.TypeReferences do
            let row = metadata.GetTypeReference handle
            add (token (TypeReferenceHandle.op_Implicit handle)) (referenceName metadata handle) (metadata.GetString row.Name) "type" "" 0 true "" [] |> ignore
        for handle in metadata.TypeDefinitions do
            let row = metadata.GetTypeDefinition handle
            let id = token (TypeDefinitionHandle.op_Implicit handle)
            let declaring = row.GetDeclaringType()
            let entity = add id (typeName metadata handle) (metadata.GetString row.Name) "type" (if declaring.IsNil then "" else token (TypeDefinitionHandle.op_Implicit declaring)) (int row.Attributes) false (typeUse (TypeDefinitionHandle.op_Implicit handle)) []
            entity["visibility"] <- text (match row.Attributes &&& TypeAttributes.VisibilityMask with
                                          | TypeAttributes.Public | TypeAttributes.NestedPublic -> "public"
                                          | TypeAttributes.NestedPrivate -> "private"
                                          | TypeAttributes.NestedFamily -> "protected"
                                          | TypeAttributes.NestedFamORAssem -> "protected_internal"
                                          | TypeAttributes.NestedFamANDAssem -> "private_protected"
                                          | _ -> "internal")
            document entity ("T:" + documentationName (typeName metadata handle))
            if not row.BaseType.IsNil then relate "inherits" id (typeTarget row.BaseType) "resolved"
            for contract in row.GetInterfaceImplementations() do
                relate "implements" id (typeTarget (metadata.GetInterfaceImplementation(contract).Interface)) "resolved"
            let fields = ResizeArray<JsonNode>()
            for field in row.GetFields() do
                let value = metadata.GetFieldDefinition field
                let name = metadata.GetString value.Name
                let fieldType = value.DecodeSignature(provider, (id, ""))
                let symbol = add (token (FieldDefinitionHandle.op_Implicit field)) (stableType (typeName metadata handle + "." + name + ":" + fieldType)) name "field" id (int value.Attributes) false fieldType []
                symbol["visibility"] <- text (memberVisibility (int value.Attributes))
                document symbol ("F:" + documentationName (typeName metadata handle) + "." + name.Replace('.', '#'))
                if (value.Attributes &&& FieldAttributes.Static) = enum 0 then
                    let info = object ["name", text name; "type", text fieldType]
                    let offset = value.GetOffset()
                    if offset >= 0 then info["offset"] <- number offset
                    descriptor (value.GetMarshallingDescriptor()) |> Option.iter (fun native -> info["native"] <- native)
                    fields.Add(info)
            let kind = row.Attributes &&& TypeAttributes.LayoutMask
            if kind <> TypeAttributes.AutoLayout then
                let layout = row.GetLayout()
                let info = object ["kind", text (if kind = TypeAttributes.ExplicitLayout then "explicit" else "sequential");
                                   "charset", text (match row.Attributes &&& TypeAttributes.StringFormatMask with
                                                    | TypeAttributes.UnicodeClass -> "unicode"
                                                    | TypeAttributes.AutoClass -> "auto"
                                                    | _ -> "ansi");
                                   "packing", number layout.PackingSize; "size", number layout.Size; "fields", array fields]
                layouts[typeName metadata handle] <- info
                entity["layout"] <- info.DeepClone()
        let opcodes = typeof<OpCodes>.GetFields(BindingFlags.Public ||| BindingFlags.Static)
                      |> Array.map (fun field -> let op = unbox<OpCode>(field.GetValue null) in uint16 op.Value, op) |> Map.ofArray
        for handle in metadata.MethodDefinitions do
            let row = metadata.GetMethodDefinition handle
            let id = methodTarget (MethodDefinitionHandle.op_Implicit handle)
            if (row.Attributes &&& MethodAttributes.PinvokeImpl) <> enum 0 then
                let import = row.GetImport()
                let entity = known[id]
                entity["library"] <- text (metadata.GetString(metadata.GetModuleReference(import.Module).Name))
                entity["entry_point"] <- text (metadata.GetString import.Name)
                entity["convention"] <- text (
                    match import.Attributes &&& MethodImportAttributes.CallingConventionMask with
                    | MethodImportAttributes.CallingConventionCDecl -> "cdecl"
                    | MethodImportAttributes.CallingConventionStdCall -> "stdcall"
                    | MethodImportAttributes.CallingConventionThisCall -> "thiscall"
                    | MethodImportAttributes.CallingConventionFastCall -> "fastcall"
                    | _ -> "platform")
                let policy = object ["charset", text (match import.Attributes &&& MethodImportAttributes.CharSetMask with
                                                       | MethodImportAttributes.CharSetAnsi -> "ansi"
                                                       | MethodImportAttributes.CharSetUnicode -> "unicode"
                                                       | MethodImportAttributes.CharSetAuto -> "auto"
                                                       | _ -> "none");
                                     "exact_spelling", flag (int (import.Attributes &&& MethodImportAttributes.ExactSpelling) <> 0);
                                     "set_last_error", flag (int (import.Attributes &&& MethodImportAttributes.SetLastError) <> 0)]
                match import.Attributes &&& MethodImportAttributes.BestFitMappingMask with
                | MethodImportAttributes.BestFitMappingEnable -> policy["best_fit_mapping"] <- flag true
                | MethodImportAttributes.BestFitMappingDisable -> policy["best_fit_mapping"] <- flag false
                | _ -> ()
                match import.Attributes &&& MethodImportAttributes.ThrowOnUnmappableCharMask with
                | MethodImportAttributes.ThrowOnUnmappableCharEnable -> policy["throw_on_unmappable_character"] <- flag true
                | MethodImportAttributes.ThrowOnUnmappableCharDisable -> policy["throw_on_unmappable_character"] <- flag false
                | _ -> ()
                entity["marshalling"] <- policy
                let signature = row.DecodeSignature(provider, (token (TypeDefinitionHandle.op_Implicit (row.GetDeclaringType())), id))
                let parameters = row.GetParameters() |> Seq.map (fun handle -> let value = metadata.GetParameter(handle) in int value.SequenceNumber, value) |> Map.ofSeq
                entity["marshalling_parameters"] <- seq {
                    for sequence = 0 to signature.ParameterTypes.Length do
                        let managedType = if sequence = 0 then signature.ReturnType else signature.ParameterTypes[sequence - 1]
                        let attributes = match parameters.TryFind sequence with Some value -> value.Attributes | None -> enum 0
                        let input = (attributes &&& ParameterAttributes.In) <> enum 0
                        let output = (attributes &&& ParameterAttributes.Out) <> enum 0
                        let value = object ["managed_type", text managedType;
                                            "direction", text (match input, output with true, true -> "in_out" | true, false -> "in" | false, true -> "out" | _ -> "default");
                                            "optional", flag ((attributes &&& ParameterAttributes.Optional) <> enum 0);
                                            "by_reference", flag (managedType.EndsWith("&", StringComparison.Ordinal))]
                        parameters.TryFind sequence |> Option.bind (fun parameter -> descriptor (parameter.GetMarshallingDescriptor()))
                        |> Option.iter (fun native -> value["native"] <- native)
                        let name = managedType.TrimEnd('&', '*')
                        match layouts.TryGetValue name with true, layout -> value["layout"] <- layout.DeepClone() | _ -> ()
                        yield object ["sequence", number sequence; "value", value]
                } |> array
            if row.RelativeVirtualAddress <> 0 then
                let il = pe.GetMethodBody(row.RelativeVirtualAddress).GetILBytes()
                let mutable at = 0
                while at < il.Length do
                    let offset = at
                    let first = il[at]
                    at <- at + 1
                    let code = if first = 0xfeuy then let second = il[at] in at <- at + 1; 0xfe00us ||| uint16 second else uint16 first
                    let opcode = opcodes[code]
                    let size =
                        match opcode.OperandType with
                        | OperandType.InlineNone -> 0
                        | OperandType.ShortInlineBrTarget | OperandType.ShortInlineI | OperandType.ShortInlineVar -> 1
                        | OperandType.InlineVar -> 2
                        | OperandType.InlineI8 | OperandType.InlineR -> 8
                        | OperandType.InlineSwitch -> 4 + 4 * BitConverter.ToInt32(il, at)
                        | _ -> 4
                    if opcode.OperandType = OperandType.InlineMethod then
                        let target = methodTarget (MetadataTokens.EntityHandle(BitConverter.ToInt32(il, at)))
                        calls.Add(object ["from", text id; "target", text target; "offset", number offset;
                                          "call", flag (opcode = OpCodes.Call || opcode = OpCodes.Callvirt || opcode = OpCodes.Newobj || opcode = OpCodes.Jmp);
                                          "dispatch", text (if opcode = OpCodes.Callvirt then "virtual" else "direct")])
                    elif opcode = OpCodes.Calli then
                        calls.Add(object ["from", text id; "target", text ""; "offset", number offset; "call", flag true; "dispatch", text "indirect"])
                    at <- at + size
        let accessors owner (methods: seq<string * MethodDefinitionHandle>) =
            let methods = methods |> Seq.filter (fun (_, handle) -> not handle.IsNil) |> Seq.toList
            for role, handle in methods do
                let id = methodTarget (MethodDefinitionHandle.op_Implicit handle)
                known[id]["owner"] <- text owner
                known[id]["accessor_role"] <- text role
            let visibility = methods |> Seq.map (fun (_, handle) -> memberVisibility (int (metadata.GetMethodDefinition(handle).Attributes))) |> Set.ofSeq
            let ordered = ["public"; "protected_internal"; "protected"; "internal"; "private_protected"; "private"]
            known[owner]["visibility"] <- text (ordered |> List.tryFind visibility.Contains |> Option.defaultValue "")
            methods |> Seq.map (fun (_, handle) -> int (metadata.GetMethodDefinition(handle).Attributes)) |> Seq.fold (|||) 0
        for handle in metadata.TypeDefinitions do
            let row = metadata.GetTypeDefinition handle
            let owner = token (TypeDefinitionHandle.op_Implicit handle)
            let qualified = typeName metadata handle
            for handle in row.GetProperties() do
                let property = metadata.GetPropertyDefinition handle
                let id = token (PropertyDefinitionHandle.op_Implicit handle)
                let name = metadata.GetString property.Name
                let signature = property.DecodeSignature(provider, (owner, ""))
                let value = add id (stableType (qualified + ".property:" + name + parameters signature.ParameterTypes + "->" + signature.ReturnType)) name "property" owner 0 false signature.ReturnType signature.ParameterTypes
                let metadataAccessors = property.GetAccessors()
                value["flags"] <- number (accessors id (seq { yield "getter", metadataAccessors.Getter; yield "setter", metadataAccessors.Setter; for method in metadataAccessors.Others do yield "other", method }))
                let signature = property.DecodeSignature(documentation, [])
                document value ("P:" + documentationName qualified + "." + name.Replace('.', '#') + parameters signature.ParameterTypes)
                for method in seq { yield metadataAccessors.Getter; yield metadataAccessors.Setter; yield! metadataAccessors.Others } |> Seq.filter (fun handle -> not handle.IsNil) do
                    let signature = metadata.GetMethodDefinition(method).DecodeSignature(documentation, [])
                    known[token (MethodDefinitionHandle.op_Implicit method)]["fsharp_documentation_id"] <- text ("P:" + documentationName qualified + "." + name + parameters signature.ParameterTypes)
            for handle in row.GetEvents() do
                let event = metadata.GetEventDefinition handle
                let id = token (EventDefinitionHandle.op_Implicit handle)
                let name = metadata.GetString event.Name
                let eventType = typeUse event.Type
                let value = add id (stableType (qualified + ".event:" + name + ":" + eventType)) name "event" owner 0 false eventType []
                let metadataAccessors = event.GetAccessors()
                value["flags"] <- number (accessors id (seq { yield "adder", metadataAccessors.Adder; yield "remover", metadataAccessors.Remover; yield "raiser", metadataAccessors.Raiser; for method in metadataAccessors.Others do yield "other", method }))
                document value ("E:" + documentationName qualified + "." + name.Replace('.', '#'))
                for method in seq { yield metadataAccessors.Adder; yield metadataAccessors.Remover; yield metadataAccessors.Raiser; yield! metadataAccessors.Others } |> Seq.filter (fun handle -> not handle.IsNil) do
                    known[token (MethodDefinitionHandle.op_Implicit method)]["fsharp_documentation_id"] <- text ("E:" + documentationName qualified + "." + name)
            let interfaces = row.GetInterfaceImplementations() |> Seq.map (fun implementation -> ownerName (metadata.GetInterfaceImplementation(implementation).Interface)) |> Set.ofSeq
            for implementation in row.GetMethodImplementations() do
                let implementation = metadata.GetMethodImplementation implementation
                let source = methodTarget implementation.MethodBody
                let target = methodTarget implementation.MethodDeclaration
                let declaredOwner = match implementation.MethodDeclaration.Kind with
                                    | HandleKind.MethodDefinition -> ownerName (TypeDefinitionHandle.op_Implicit (metadata.GetMethodDefinition(MethodDefinitionHandle.op_Explicit implementation.MethodDeclaration).GetDeclaringType()))
                                    | HandleKind.MemberReference -> ownerName (metadata.GetMemberReference(MemberReferenceHandle.op_Explicit implementation.MethodDeclaration).Parent)
                                    | _ -> ""
                let interfaceDeclaration =
                    match implementation.MethodDeclaration.Kind with
                    | HandleKind.MethodDefinition ->
                        let definition = metadata.GetMethodDefinition(MethodDefinitionHandle.op_Explicit implementation.MethodDeclaration).GetDeclaringType()
                        (metadata.GetTypeDefinition(definition).Attributes &&& TypeAttributes.Interface) <> enum 0
                    | _ -> interfaces.Contains declaredOwner
                relate (if interfaceDeclaration then "implements" else "overrides") source target "resolved"
        let definitions = metadata.TypeDefinitions |> Seq.map (fun handle -> typeName metadata handle, handle) |> Map.ofSeq
        let rec localDefinition (handle: EntityHandle) =
            match handle.Kind with
            | HandleKind.TypeDefinition -> Some(TypeDefinitionHandle.op_Explicit handle)
            | HandleKind.TypeSpecification ->
                let mutable blob = metadata.GetBlobReader(metadata.GetTypeSpecification(TypeSpecificationHandle.op_Explicit handle).Signature)
                if blob.ReadByte() = 0x15uy then
                    blob.ReadByte() |> ignore
                    localDefinition (blob.ReadTypeHandle())
                else None
            | HandleKind.TypeReference ->
                let name = documentationName (referenceName metadata (TypeReferenceHandle.op_Explicit handle))
                let row = metadata.GetTypeReference(TypeReferenceHandle.op_Explicit handle)
                let local = match row.ResolutionScope.Kind with
                            | HandleKind.AssemblyReference -> metadata.GetString(metadata.GetAssemblyReference(AssemblyReferenceHandle.op_Explicit row.ResolutionScope).Name) = metadata.GetString(metadata.GetAssemblyDefinition().Name)
                            | HandleKind.ModuleDefinition -> true
                            | _ -> false
                if local then definitions |> Map.tryPick (fun key handle -> if documentationName key = name then Some handle else None) else None
            | _ -> None
        let sameSignature (a: MethodSignature<string>) (b: MethodSignature<string>) =
            a.GenericParameterCount = b.GenericParameterCount && a.ReturnType = b.ReturnType && Seq.toList a.ParameterTypes = Seq.toList b.ParameterTypes
        for handle in metadata.MethodDefinitions do
            let method = metadata.GetMethodDefinition handle
            let id = token (MethodDefinitionHandle.op_Implicit handle)
            let reuse = (method.Attributes &&& MethodAttributes.Virtual) <> enum 0 && (method.Attributes &&& MethodAttributes.NewSlot) = enum 0
            if reuse && not (relations |> Seq.exists (fun edge -> field edge "from" = id && (field edge "kind" = "overrides" || field edge "kind" = "implements"))) then
                let name = metadata.GetString method.Name
                let signature = method.DecodeSignature(documentation, [])
                let rec search (parent: EntityHandle) arguments seen =
                    if parent.IsNil || Set.contains (token parent) seen then false
                    else
                        let seen = Set.add (token parent) seen
                        let supplied = if parent.Kind = HandleKind.TypeSpecification then
                                           let value = documentation.GetTypeFromSpecification(metadata, arguments, TypeSpecificationHandle.op_Explicit parent, 0uy)
                                           documentationTypes.Arguments value
                                       else []
                        match localDefinition parent with
                        | Some definition ->
                            let row = metadata.GetTypeDefinition definition
                            let matches = row.GetMethods() |> Seq.filter (fun candidate ->
                                let value = metadata.GetMethodDefinition candidate
                                (value.Attributes &&& MethodAttributes.Virtual) <> enum 0 && metadata.GetString(value.Name) = name && sameSignature signature (value.DecodeSignature(documentation, supplied))) |> Seq.toArray
                            if matches.Length > 0 then
                                for candidate in matches do relate "overrides" id (methodTarget (MethodDefinitionHandle.op_Implicit candidate)) (if matches.Length = 1 then "resolved" else "candidate")
                                true
                            else search row.BaseType supplied seen
                        | None ->
                            let owner = typeTarget parent
                            let matches = metadata.MemberReferences |> Seq.filter (fun candidate ->
                                let value = metadata.GetMemberReference candidate
                                value.GetKind() = MemberReferenceKind.Method && token value.Parent = owner && metadata.GetString(value.Name) = name && sameSignature signature (value.DecodeMethodSignature(documentation, supplied))) |> Seq.toArray
                            for candidate in matches do relate "overrides" id (methodTarget (MemberReferenceHandle.op_Implicit candidate)) (if matches.Length = 1 then "resolved" else "candidate")
                            matches.Length > 0
                if not (search (metadata.GetTypeDefinition(method.GetDeclaringType()).BaseType) [] Set.empty) then unresolved.Add(id)
        let mapping (handle: EntityHandle) =
            for attributeHandle in metadata.GetCustomAttributes(handle) do
                let attribute = metadata.GetCustomAttribute attributeHandle
                let owner, signature =
                    match attribute.Constructor.Kind with
                    | HandleKind.MemberReference ->
                        let constructor = metadata.GetMemberReference(MemberReferenceHandle.op_Explicit attribute.Constructor)
                        ownerName constructor.Parent, constructor.DecodeMethodSignature(documentation, [])
                    | HandleKind.MethodDefinition ->
                        let constructor = metadata.GetMethodDefinition(MethodDefinitionHandle.op_Explicit attribute.Constructor)
                        typeName metadata (constructor.GetDeclaringType()), constructor.DecodeSignature(documentation, [])
                    | _ -> "", Unchecked.defaultof<MethodSignature<string>>
                if documentationName owner = "Microsoft.FSharp.Core.CompilationMappingAttribute" && signature.ParameterTypes.Length > 0 && signature.ParameterTypes[0] = "Microsoft.FSharp.Core.SourceConstructFlags" then
                    let mutable blob = metadata.GetBlobReader(attribute.Value)
                    if blob.ReadUInt16() <> 1us then failwith "Invalid CompilationMappingAttribute prolog"
                    let flags = blob.ReadInt32()
                    let value = object ["flags", number flags; "kind", text ((enum<Microsoft.FSharp.Core.SourceConstructFlags> (flags &&& int Microsoft.FSharp.Core.SourceConstructFlags.KindMask)).ToString())]
                    if signature.ParameterTypes.Length = 2 then value["sequence"] <- number (blob.ReadInt32())
                    elif signature.ParameterTypes.Length = 3 then
                        value["variant"] <- number (blob.ReadInt32())
                        value["sequence"] <- number (blob.ReadInt32())
                    known[token handle]["compilation_mapping"] <- value
        for handle in metadata.TypeDefinitions do
            mapping (TypeDefinitionHandle.op_Implicit handle)
            let row = metadata.GetTypeDefinition handle
            for field in row.GetFields() do mapping (FieldDefinitionHandle.op_Implicit field)
            for property in row.GetProperties() do mapping (PropertyDefinitionHandle.op_Implicit property)
            for event in row.GetEvents() do mapping (EventDefinitionHandle.op_Implicit event)
            for method in row.GetMethods() do mapping (MethodDefinitionHandle.op_Implicit method)
        let readSymbols (debug: MetadataReader) =
            for handle in metadata.MethodDefinitions do
                let info = debug.GetMethodDebugInformation(handle)
                let entity = known[token (MethodDefinitionHandle.op_Implicit handle)]
                entity["locations"] <- info.GetSequencePoints() |> Seq.filter (fun point -> not point.IsHidden) |> Seq.map (fun point ->
                    let document = if point.Document.IsNil then info.Document else point.Document
                    object ["file", text (debug.GetString(debug.GetDocument(document).Name)); "offset", number point.Offset;
                            "line", number point.StartLine; "column", number point.StartColumn; "end_line", number point.EndLine; "end_column", number point.EndColumn]) |> array
        match pdb with
        | Some path ->
            use stream = File.OpenRead path
            use symbols = MetadataReaderProvider.FromPortablePdbStream(stream)
            let debug = symbols.GetMetadataReader()
            let identity = BlobContentId(debug.DebugMetadataHeader.Id)
            let expected = pe.ReadDebugDirectory() |> Seq.filter (fun entry -> entry.Type = DebugDirectoryEntryType.CodeView)
                           |> Seq.map (fun entry -> pe.ReadCodeViewDebugDirectoryData(entry).Guid) |> Set.ofSeq
            if not (expected.Contains identity.Guid) then failwith "Portable PDB belongs to another assembly"
            readSymbols debug
        | None ->
            for entry in pe.ReadDebugDirectory() do
                if entry.Type = DebugDirectoryEntryType.EmbeddedPortablePdb then
                    use symbols = pe.ReadEmbeddedPortablePdbDebugDirectoryData(entry)
                    readSymbols (symbols.GetMetadataReader())
        object ["assembly", text (metadata.GetString(metadata.GetAssemblyDefinition().Name)); "entities", array entities; "types", array signatures.Records; "calls", array calls; "relations", array relations; "unresolved", strings unresolved]
