using System.Globalization;
using System.Text.Json.Nodes;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Fughetta.CSharp;

internal static class Symbols
{
    private static readonly SymbolDisplayFormat Format = SymbolDisplayFormat.FullyQualifiedFormat
        .WithGlobalNamespaceStyle(SymbolDisplayGlobalNamespaceStyle.Omitted)
        .WithMemberOptions(SymbolDisplayMemberOptions.IncludeContainingType | SymbolDisplayMemberOptions.IncludeType |
            SymbolDisplayMemberOptions.IncludeExplicitInterface | SymbolDisplayMemberOptions.IncludeParameters |
            SymbolDisplayMemberOptions.IncludeRef)
        .WithParameterOptions(SymbolDisplayParameterOptions.IncludeType | SymbolDisplayParameterOptions.IncludeName |
            SymbolDisplayParameterOptions.IncludeParamsRefOut)
        .WithMiscellaneousOptions(SymbolDisplayMiscellaneousOptions.EscapeKeywordIdentifiers |
            SymbolDisplayMiscellaneousOptions.IncludeNullableReferenceTypeModifier);

    public static string Display(ISymbol symbol) => symbol.ToDisplayString(Format);

    public static string Literal(object? value) => value switch
    {
        null => "null",
        string text => Microsoft.CodeAnalysis.CSharp.SymbolDisplay.FormatLiteral(text, quote: true),
        char character => Microsoft.CodeAnalysis.CSharp.SymbolDisplay.FormatLiteral(character, quote: true),
        bool flag => flag ? "true" : "false",
        IFormattable scalar => scalar.ToString(null, CultureInfo.InvariantCulture),
        _ => value.ToString() ?? ""
    };

    public static IEnumerable<string> Constraints(ITypeParameterSymbol parameter)
    {
        if (parameter.HasUnmanagedTypeConstraint) yield return "unmanaged";
        else if (parameter.HasValueTypeConstraint) yield return "struct";
        if (parameter.HasReferenceTypeConstraint)
            yield return parameter.ReferenceTypeConstraintNullableAnnotation == NullableAnnotation.Annotated ? "class?" : "class";
        if (parameter.HasNotNullConstraint) yield return "notnull";
        if (parameter.HasConstructorConstraint) yield return "new()";
        if (parameter.AllowsRefLikeType) yield return "allows ref struct";
    }

    public static bool IsLocal(ISymbol symbol) => symbol is ILocalSymbol or IRangeVariableSymbol or ILabelSymbol ||
        symbol is IMethodSymbol { MethodKind: MethodKind.LocalFunction or MethodKind.AnonymousFunction };

    public static ISymbol Definition(ISymbol symbol) => symbol switch
    {
        IMethodSymbol method => (method.ReducedFrom ?? method).OriginalDefinition.PartialDefinitionPart ??
            (method.ReducedFrom ?? method).OriginalDefinition,
        IPropertySymbol property => property.OriginalDefinition.PartialDefinitionPart ?? property.OriginalDefinition,
        _ => symbol.OriginalDefinition
    };

    public static string Kind(ISymbol symbol) => symbol switch
    {
        INamedTypeSymbol { IsRecord: true } => "csharp:record",
        INamedTypeSymbol { TypeKind: TypeKind.Struct } => "csharp:struct",
        INamedTypeSymbol { TypeKind: TypeKind.Interface } => "csharp:interface",
        INamedTypeSymbol { TypeKind: TypeKind.Delegate } => "csharp:delegate",
        INamedTypeSymbol => "type",
        IMethodSymbol { MethodKind: MethodKind.Constructor or MethodKind.StaticConstructor } => "constructor",
        IMethodSymbol { MethodKind: MethodKind.Destructor } => "destructor",
        IMethodSymbol { MethodKind: MethodKind.LocalFunction or MethodKind.AnonymousFunction } => "function",
        IMethodSymbol => "method",
        IPropertySymbol => "property",
        IFieldSymbol { ContainingType.TypeKind: TypeKind.Enum } => "enum_member",
        IFieldSymbol => "field",
        IEventSymbol => "csharp:event",
        IParameterSymbol or ITypeParameterSymbol => "parameter",
        ILocalSymbol or IRangeVariableSymbol => "variable",
        INamespaceSymbol => "namespace",
        ILabelSymbol => "label",
        _ => "other"
    };

    public static string Visibility(ISymbol symbol) => symbol.DeclaredAccessibility switch
    {
        Accessibility.Public => "public",
        Accessibility.ProtectedOrInternal => "protected_internal",
        Accessibility.Protected => "protected",
        Accessibility.Internal => "internal",
        Accessibility.ProtectedAndInternal => "private_protected",
        Accessibility.Private => "private",
        _ => symbol is INamespaceSymbol ? "public" : "local"
    };

    public static string RefKind(RefKind kind) => kind switch
    {
        Microsoft.CodeAnalysis.RefKind.None => "none",
        Microsoft.CodeAnalysis.RefKind.Ref => "ref",
        Microsoft.CodeAnalysis.RefKind.Out => "out",
        Microsoft.CodeAnalysis.RefKind.In => "in",
        Microsoft.CodeAnalysis.RefKind.RefReadOnlyParameter => "ref_readonly",
        _ => kind.ToString().ToLowerInvariant()
    };

    public static JsonNode Parameter(IParameterSymbol parameter)
    {
        var row = new JsonObject
        {
            ["group"] = 0, ["index"] = parameter.Ordinal, ["name"] = parameter.Name,
            ["annotations"] = new JsonObject
            {
                ["csharp.optional"] = parameter.IsOptional, ["csharp.params"] = parameter.IsParams,
                ["csharp.ref_kind"] = RefKind(parameter.RefKind),
                ["csharp.this"] = parameter.IsThis ||
                    (parameter.Ordinal == 0 && parameter.ContainingSymbol is IMethodSymbol { IsExtensionMethod: true })
            }
        };
        if (parameter.HasExplicitDefaultValue)
            row["default_value"] = Literal(parameter.ExplicitDefaultValue);
        return row;
    }

    public static bool Pinvoke(IMethodSymbol method, JsonObject extensions)
    {
        var attributes = method.GetAttributes();
        var import = attributes.FirstOrDefault(attribute => attribute.AttributeClass?.ToDisplayString() is
            "System.Runtime.InteropServices.DllImportAttribute" or "System.Runtime.InteropServices.LibraryImportAttribute");
        if (import is null || import.ConstructorArguments.Length == 0 ||
            import.ConstructorArguments[0].Value is not string library)
            return false;

        var entry = import.NamedArguments.FirstOrDefault(argument => argument.Key == "EntryPoint").Value.Value as string;
        var convention = import.NamedArguments.FirstOrDefault(argument => argument.Key == "CallingConvention").Value.Value as int?;
        var callConvs = attributes.FirstOrDefault(attribute =>
            attribute.AttributeClass?.ToDisplayString() == "System.Runtime.InteropServices.UnmanagedCallConvAttribute")
            ?.NamedArguments.FirstOrDefault(argument => argument.Key == "CallConvs").Value;
        var callingConvention = convention switch
        {
            2 => "cdecl",
            3 => "stdcall",
            4 => "thiscall",
            5 => "fastcall",
            _ => "platform"
        };
        if (callConvs is { Kind: TypedConstantKind.Array, IsNull: false })
        {
            foreach (var value in callConvs.Value.Values)
            {
                callingConvention = (value.Value as INamedTypeSymbol)?.ToDisplayString() switch
                {
                    "System.Runtime.CompilerServices.CallConvCdecl" => "cdecl",
                    "System.Runtime.CompilerServices.CallConvStdcall" => "stdcall",
                    "System.Runtime.CompilerServices.CallConvThiscall" => "thiscall",
                    "System.Runtime.CompilerServices.CallConvFastcall" => "fastcall",
                    _ => callingConvention
                };
            }
        }

        extensions["clr.pinvoke.library"] = library;
        extensions["clr.pinvoke.entry_point"] = entry ?? method.MetadataName;
        extensions["clr.pinvoke.convention"] = callingConvention;
        return true;
    }
}

internal sealed class Identities
{
    private readonly Workspace workspace;
    private readonly CompilationUnit unit;
    private readonly string project;
    private readonly string provider;
    private readonly HashSet<SyntaxTree> selected;
    private readonly Dictionary<IAssemblySymbol, string> modules = new(SymbolEqualityComparer.Default);
    private readonly Dictionary<ISymbol, string?> ids = new(SymbolEqualityComparer.Default);
    public Dictionary<string, JsonNode> Rows { get; } = new(StringComparer.Ordinal);

    public Identities(Workspace workspace, CompilationUnit unit, JsonNode session, HashSet<SyntaxTree> selected)
    {
        this.workspace = workspace;
        this.unit = unit;
        this.selected = selected;
        project = session.String("project");
        provider = session.String("provider");
        modules[unit.Compilation.Assembly] = unit.Id;
        var upstream = unit.Upstream().ToDictionary(value => value.Compilation, value => value.Id);
        foreach (var reference in unit.Compilation.References.OfType<CompilationReference>())
            if (upstream.TryGetValue((CSharpCompilation)reference.Compilation, out var id) &&
                unit.Compilation.GetAssemblyOrModuleSymbol(reference) is IAssemblySymbol assembly)
                modules[assembly] = id;
    }

    public string? Get(ISymbol? symbol)
    {
        if (symbol is IAliasSymbol alias)
            symbol = alias.Target;
        if (symbol is null or INamespaceSymbol { IsGlobalNamespace: true } or IErrorTypeSymbol)
            return null;

        var definition = Symbols.Definition(symbol);
        if (ids.TryGetValue(definition, out var id))
            return id;

        var assembly = definition.ContainingAssembly;
        var declaration = definition.Locations.FirstOrDefault(location => location.IsInSource);
        var module = assembly is not null && modules.TryGetValue(assembly, out var sourceModule)
            ? sourceModule : assembly?.Identity.GetDisplayName() ?? (declaration is null ? "namespace" : unit.Id);
        var identity = new JsonObject { ["project"] = project, ["module"] = module, ["provider"] = provider };
        var documentation = Symbols.IsLocal(definition) ? null : definition.GetDocumentationCommentId();
        if (documentation is not null)
        {
            identity["domain"] = "csharp";
            identity["native_key"] = documentation;
        }
        else if (declaration?.SourceTree is { } tree && selected.Contains(tree))
        {
            var path = workspace.RelativePath(tree.FilePath);
            var anchor = declaration.SourceSpan;
            // Generated objects have no source anchor in the workspace's text store.
            if (unit.Generated.ContainsKey(tree.FilePath) && workspace.SourceObject(tree.FilePath) is null)
            {
                identity["domain"] = "csharp";
                identity["native_key"] = $"generated:{path}:{anchor.Start}:{definition.Kind}:{definition.MetadataName}";
            }
            else
            {
                identity["anchor"] = new JsonObject
                {
                    ["file"] = path,
                    ["declaration"] = $"pending-{anchor.Start}-{anchor.Length}-{definition.Kind}"
                };
            }
        }
        else if (declaration is null && definition is IParameterSymbol parameter && Get(parameter.ContainingSymbol) is { } owner)
        {
            identity["domain"] = "csharp";
            identity["native_key"] = $"parameter:{parameter.Ordinal}";
            identity["owner"] = owner;
        }
        else
        {
            ids[definition] = null;
            return null;
        }

        id = "symbol-" + Wire.Digest(identity.ToJsonString());
        ids[definition] = id;
        Rows.TryAdd(id, new JsonObject { ["symbol"] = id, ["declaration"] = identity });
        return id;
    }
}
