using System.Text.Json.Nodes;
using Microsoft.CodeAnalysis;

namespace Fughetta.CSharp;

internal sealed class Types(Identities identities)
{
    private readonly Dictionary<string, string> parameters = new(StringComparer.Ordinal);
    public Dictionary<string, JsonNode> Rows { get; } = new(StringComparer.Ordinal);

    private static JsonObject Row(string kind, string? name = null)
    {
        var row = new JsonObject
        {
            ["domain"] = "csharp", ["kind"] = kind, ["arguments"] = new JsonArray(), ["constraints"] = new JsonArray(),
            ["elements"] = new JsonArray(), ["type_parameters"] = new JsonArray(), ["parameter_groups"] = new JsonArray(),
            ["qualifiers"] = new JsonArray(), ["application_arguments"] = new JsonArray(), ["extensions"] = new JsonObject()
        };
        if (name is not null)
            row["name"] = name;
        return row;
    }

    private string Add(JsonObject row, string? id = null)
    {
        id ??= "type-" + Wire.Digest(row.ToJsonString());
        row["id"] = id;
        Rows.TryAdd(id, row);
        return id;
    }

    public string Parameter(ITypeParameterSymbol parameter)
    {
        parameter = (ITypeParameterSymbol)parameter.OriginalDefinition;
        var binder = identities.Get(parameter.ContainingSymbol);
        var key = (binder ?? Symbols.Display(parameter.ContainingSymbol)) + ":" + parameter.Ordinal;
        if (parameters.TryGetValue(key, out var existing))
            return existing;

        // The binder and ordinal identify the parameter before walking recursive constraints.
        var id = "type-" + Wire.Digest("parameter:" + key);
        parameters.Add(key, id);
        var row = Row("parameter", parameter.Name);
        if (binder is not null)
            row["binder"] = binder;
        row["parameter_index"] = parameter.Ordinal;
        row["qualifiers"] = Wire.Strings(Symbols.Constraints(parameter));
        row["constraints"] = Wire.Strings(parameter.ConstraintTypes.Select(type => Get(type)));
        return Add(row, id);
    }

    public string Get(ITypeSymbol type, string byRef) => byRef == "none" ? Get(type) : Wrap("reference", Get(type), byRef);

    private string Wrap(string kind, string nested, string? qualifier = null)
    {
        var row = Row(kind);
        row["arguments"] = Wire.Strings([nested]);
        if (qualifier is not null)
            row["qualifiers"] = Wire.Strings([qualifier]);
        return Add(row);
    }

    public string Get(ITypeSymbol type)
    {
        if (type is INamedTypeSymbol { OriginalDefinition.SpecialType: SpecialType.System_Nullable_T } nullable)
            return Wrap("nullable", Get(nullable.TypeArguments[0]));
        if ((type.IsReferenceType || type is ITypeParameterSymbol { HasValueTypeConstraint: false }) &&
            type.NullableAnnotation == NullableAnnotation.Annotated)
            return Wrap("nullable", Get(type.WithNullableAnnotation(NullableAnnotation.NotAnnotated)));

        switch (type)
        {
            case ITypeParameterSymbol parameter:
                return Parameter(parameter);
            case IDynamicTypeSymbol:
                return Add(Row("dynamic", "dynamic"));
            case IArrayTypeSymbol array:
                var rank = Row("array");
                rank["arguments"] = Wire.Strings([Get(array.ElementType)]);
                rank["extensions"]!["csharp.rank"] = array.Rank;
                return Add(rank);
            case IPointerTypeSymbol pointer:
                return Wrap("pointer", Get(pointer.PointedAtType));
            case IFunctionPointerTypeSymbol pointer:
                return Wrap("pointer", Callable(pointer.Signature));
            case INamedTypeSymbol { IsTupleType: true } tuple:
                var elements = Row("tuple");
                elements["elements"] = Wire.Strings(tuple.TupleElements.Select(element => Get(element.Type)));
                elements["extensions"]!["csharp.element_names"] = Wire.Strings(tuple.TupleElements.Select(element => element.Name));
                return Add(elements);
            case IErrorTypeSymbol:
                return Add(Row("unresolved", Symbols.Display(type)));
            case INamedTypeSymbol named:
                return Named(named);
            default:
                return Add(Row("unknown", Symbols.Display(type)));
        }
    }

    private string Named(INamedTypeSymbol type)
    {
        if (type.SpecialType is SpecialType.System_Object or >= SpecialType.System_Void and <= SpecialType.System_String or
            SpecialType.System_IntPtr or SpecialType.System_UIntPtr)
            return Add(Row("primitive", Symbols.Display(type)));

        var definition = type.OriginalDefinition;
        var definitionName = definition.GetDocumentationCommentId() is { } documentation
            ? documentation[2..] : Symbols.Display(definition);
        var name = string.Join('.', definitionName.Split('.').Select(part => part.Split('`')[0]));
        var arguments = TypeArguments(type).Select(Get).ToArray();
        var named = Row(arguments.Length == 0 ? "named" : "application", name);
        var declaration = identities.Get(definition);
        if (declaration is not null)
            named["declaration"] = declaration;
        named["arguments"] = Wire.Strings(arguments);
        named["application_arguments"] = Wire.Rows(arguments.Select(argument => new JsonObject
        {
            ["kind"] = "type", ["value"] = "", ["type"] = argument, ["elements"] = new JsonArray()
        }));
        return Add(named);
    }

    private static IEnumerable<ITypeSymbol> TypeArguments(INamedTypeSymbol type)
    {
        if (type.ContainingType is { } owner)
            foreach (var argument in TypeArguments(owner))
                yield return argument;
        foreach (var argument in type.TypeArguments)
            yield return argument;
    }

    public string Callable(IMethodSymbol method)
    {
        var function = Row("function");
        var constructor = method.MethodKind is MethodKind.Constructor or MethodKind.StaticConstructor;
        ITypeSymbol? receiver = null;
        var parameters = method.Parameters.AsEnumerable();
        if (method.ReducedFrom is not null)
            receiver = method.ReceiverType;
        else if (method.IsExtensionMethod && method.Parameters.Length > 0)
        {
            receiver = method.Parameters[0].Type;
            parameters = parameters.Skip(1);
        }
        else if (!method.IsStatic && !constructor && method.MethodKind is not
                 (MethodKind.AnonymousFunction or MethodKind.LocalFunction or MethodKind.DelegateInvoke or MethodKind.FunctionPointerSignature))
            receiver = method.ReceiverType ?? method.ContainingType;

        if (receiver is not null)
            function["receiver"] = Get(receiver, method.IsExtensionMethod && method.ReducedFrom is null
                ? Symbols.RefKind(method.Parameters[0].RefKind) : "none");
        function["return_type"] = constructor ? Get(method.ContainingType) : Get(method.ReturnType, ReturnRef(method));
        function["type_parameters"] = Wire.Strings(method.TypeParameters.Select(Parameter));
        function["parameter_groups"] = new JsonArray(new JsonObject
        {
            ["parameters"] = Wire.Rows(parameters.Select(parameter => new JsonObject
            {
                ["type"] = Get(parameter.Type, Symbols.RefKind(parameter.RefKind)),
                ["modifiers"] = Wire.Strings(parameter.IsParams ? ["params"] : [])
            }))
        });
        return Add(function);
    }

    public static string ReturnRef(IMethodSymbol method) =>
        method.ReturnsByRefReadonly ? "ref_readonly" : Symbols.RefKind(method.RefKind);
}
