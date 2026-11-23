using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Fughetta.CSharp;

internal static class Reads
{
    private static readonly ConditionalWeakTable<CSharpCompilation, Summary> Summaries = new();
    private sealed record Summary(string? Declarations, string? Inline)
    {
        public string? Get(string channel) => channel switch
        {
            "declarations" => Declarations,
            "inline" => Inline,
            _ => null
        };
    }

    private static Summary Summarize(CSharpCompilation compilation)
    {
        if (compilation.GetDeclarationDiagnostics().Any(item => item.Severity == DiagnosticSeverity.Error))
            return new(null, null);

        var declarations = new List<string>();
        var inline = new List<string>();
        declarations.Add(Attributes(compilation.Assembly.GetAttributes()).ToJsonString());
        declarations.Add(Attributes(compilation.SourceModule.GetAttributes()).ToJsonString());

        void Visit(ISymbol symbol)
        {
            if ((symbol.DeclaredAccessibility == Accessibility.Private || symbol is INamedTypeSymbol { IsFileLocal: true }) &&
                symbol is not IMethodSymbol { ExplicitInterfaceImplementations.Length: > 0 } &&
                symbol is not IPropertySymbol { ExplicitInterfaceImplementations.Length: > 0 } &&
                symbol is not IEventSymbol { ExplicitInterfaceImplementations.Length: > 0 })
                return;

            if (symbol is INamespaceSymbol ns)
            {
                foreach (var member in ns.GetMembers())
                    Visit(member);
                return;
            }

            var key = symbol.GetDocumentationCommentId() ?? Symbols.Display(symbol);
            var row = new JsonObject
            {
                ["key"] = key,
                ["kind"] = symbol.Kind.ToString(),
                ["signature"] = Symbols.Display(symbol),
                ["accessibility"] = symbol.DeclaredAccessibility.ToString(),
                ["attributes"] = Attributes(symbol.GetAttributes()),
                ["static"] = symbol.IsStatic,
                ["abstract"] = symbol.IsAbstract,
                ["virtual"] = symbol.IsVirtual,
                ["override"] = symbol.IsOverride,
                ["sealed"] = symbol.IsSealed
            };
            void Generics(IEnumerable<ITypeParameterSymbol> parameters)
            {
                row["generics"] = Wire.Rows(parameters.Select(parameter => new JsonObject
                {
                    ["name"] = parameter.Name,
                    ["qualifiers"] = Wire.Strings(Symbols.Constraints(parameter)),
                    ["variance"] = parameter.Variance.ToString(),
                    ["constraints"] = Wire.Strings(parameter.ConstraintTypes.Select(Symbols.Display)),
                    ["attributes"] = Attributes(parameter.GetAttributes())
                }));
            }

            void Parameters(IEnumerable<IParameterSymbol> parameters)
            {
                row["parameters"] = Wire.Rows(parameters.Select(parameter => new JsonObject
                {
                    ["name"] = parameter.Name,
                    ["type"] = Symbols.Display(parameter.Type),
                    ["ref"] = parameter.RefKind.ToString(),
                    ["scoped"] = parameter.ScopedKind.ToString(),
                    ["optional"] = parameter.IsOptional,
                    ["params"] = parameter.IsParams,
                    ["attributes"] = Attributes(parameter.GetAttributes())
                }));
                foreach (var parameter in parameters.Where(parameter => parameter.HasExplicitDefaultValue))
                    inline.Add(new JsonArray(key, parameter.Ordinal, Symbols.Literal(parameter.ExplicitDefaultValue)).ToJsonString());
            }

            switch (symbol)
            {
                case INamedTypeSymbol type:
                    row["type_kind"] = type.TypeKind.ToString();
                    row["record"] = type.IsRecord;
                    row["readonly"] = type.IsReadOnly;
                    row["ref_like"] = type.IsRefLikeType;
                    row["base"] = type.BaseType is null ? null : Symbols.Display(type.BaseType);
                    row["interfaces"] = Wire.Strings(type.Interfaces.Select(Symbols.Display));
                    row["underlying"] = type.EnumUnderlyingType is null ? null : Symbols.Display(type.EnumUnderlyingType);
                    Generics(type.TypeParameters);
                    foreach (var member in type.GetMembers())
                        Visit(member);
                    break;
                case IMethodSymbol method:
                    row["method_kind"] = method.MethodKind.ToString();
                    row["return"] = Symbols.Display(method.ReturnType);
                    row["return_attributes"] = Attributes(method.GetReturnTypeAttributes());
                    row["ref"] = method.RefKind.ToString();
                    row["extension"] = method.IsExtensionMethod;
                    row["readonly"] = method.IsReadOnly;
                    row["implemented"] = Wire.Strings(method.ExplicitInterfaceImplementations.Select(Symbols.Display));
                    Generics(method.TypeParameters);
                    Parameters(method.Parameters);
                    break;
                case IPropertySymbol property:
                    row["type"] = Symbols.Display(property.Type);
                    row["ref"] = property.RefKind.ToString();
                    row["getter"] = property.GetMethod?.DeclaredAccessibility.ToString();
                    row["setter"] = property.SetMethod?.DeclaredAccessibility.ToString();
                    row["init"] = property.SetMethod?.IsInitOnly ?? false;
                    row["required"] = property.IsRequired;
                    row["implemented"] = Wire.Strings(property.ExplicitInterfaceImplementations.Select(Symbols.Display));
                    Parameters(property.Parameters);
                    break;
                case IFieldSymbol field:
                    row["type"] = Symbols.Display(field.Type);
                    row["ref"] = field.RefKind.ToString();
                    row["readonly"] = field.IsReadOnly;
                    row["volatile"] = field.IsVolatile;
                    row["const"] = field.IsConst;
                    row["required"] = field.IsRequired;
                    if (field.HasConstantValue)
                        inline.Add(new JsonArray(key, Symbols.Literal(field.ConstantValue)).ToJsonString());
                    break;
                case IEventSymbol @event:
                    row["type"] = Symbols.Display(@event.Type);
                    row["implemented"] = Wire.Strings(@event.ExplicitInterfaceImplementations.Select(Symbols.Display));
                    break;
            }

            declarations.Add(row.ToJsonString());
        }

        IEnumerable<JsonNode> PrimaryConstructorCaptures(INamedTypeSymbol type)
        {
            var declarations = type.DeclaringSyntaxReferences.Select(reference => reference.GetSyntax())
                .OfType<TypeDeclarationSyntax>().ToArray();
            var primary = declarations.Where(declaration => declaration.ParameterList is not null).SelectMany(declaration =>
                declaration.ParameterList!.Parameters.Select(parameter =>
                    compilation.GetSemanticModel(declaration.SyntaxTree).GetDeclaredSymbol(parameter)))
                .OfType<IParameterSymbol>().ToHashSet<ISymbol>(SymbolEqualityComparer.Default);
            if (primary.Count == 0)
                yield break;

            var names = primary.Select(parameter => parameter.Name).ToHashSet(StringComparer.Ordinal);
            var captures = new SortedDictionary<int, IParameterSymbol>();
            foreach (var declaration in declarations)
            {
                var model = compilation.GetSemanticModel(declaration.SyntaxTree);
                foreach (var member in declaration.Members.Where(member => member is not BaseTypeDeclarationSyntax))
                {
                    foreach (var identifier in member.DescendantNodes().OfType<IdentifierNameSyntax>()
                        .Where(identifier => names.Contains(identifier.Identifier.ValueText)))
                    {
                        // A direct field initializer can read the parameter without storing it.
                        if (identifier.Ancestors().TakeWhile(node => node != declaration).Any(node => node is
                                BaseMethodDeclarationSyntax or AccessorDeclarationSyntax or ArrowExpressionClauseSyntax or AnonymousFunctionExpressionSyntax) &&
                            model.GetSymbolInfo(identifier).Symbol is IParameterSymbol parameter && primary.Contains(parameter))
                            captures.TryAdd(parameter.Ordinal, parameter);
                    }
                }
            }
            foreach (var (ordinal, parameter) in captures)
                yield return new JsonObject { ["parameter"] = ordinal, ["type"] = Symbols.Display(parameter.Type) };
        }

        // A struct's instance layout participates in C# type checking even across assembly boundaries.
        void Layouts(INamespaceOrTypeSymbol container)
        {
            foreach (var member in container.GetMembers())
            {
                if (member is INamespaceSymbol ns)
                    Layouts(ns);
                else if (member is INamedTypeSymbol type)
                {
                    if (type.TypeKind == TypeKind.Struct)
                    {
                        declarations.Add(new JsonObject
                        {
                            ["layout"] = Symbols.Display(type),
                            ["attributes"] = Attributes(type.GetAttributes()),
                            ["unmanaged"] = type.IsUnmanagedType,
                            ["primary_constructor_captures"] = Wire.Rows(PrimaryConstructorCaptures(type)),
                            ["field_events"] = Wire.Rows(type.GetMembers().OfType<IEventSymbol>().Where(symbol => !symbol.IsStatic &&
                                symbol.DeclaringSyntaxReferences.Any(reference => reference.GetSyntax().AncestorsAndSelf().Any(node => node is EventFieldDeclarationSyntax)))
                                .Select(symbol => new JsonObject { ["name"] = symbol.MetadataName, ["type"] = Symbols.Display(symbol.Type) })),
                            ["fields"] = Wire.Rows(type.GetMembers().OfType<IFieldSymbol>().Where(field => !field.IsStatic)
                                .Select(field => new JsonObject
                                {
                                    ["name"] = field.MetadataName,
                                    ["type"] = Symbols.Display(field.Type),
                                    ["ref"] = field.RefKind.ToString(),
                                    ["readonly"] = field.IsReadOnly,
                                    ["fixed_size"] = field.FixedSize,
                                    ["attributes"] = Attributes(field.GetAttributes())
                                }))
                        }.ToJsonString());
                    }
                    Layouts(type);
                }
            }
        }

        Layouts(compilation.Assembly.GlobalNamespace);
        Visit(compilation.Assembly.GlobalNamespace);
        return new(Digest(declarations), Digest(inline));
    }

    private static string Digest(IEnumerable<string> values) =>
        Wire.Digest(Wire.Strings(values.Order(StringComparer.Ordinal)).ToJsonString());

    private static JsonArray Attributes(IEnumerable<AttributeData> attributes)
    {
        JsonNode Constant(TypedConstant value) => new JsonObject
        {
            ["type"] = value.Type is null ? null : Symbols.Display(value.Type),
            ["kind"] = value.Kind.ToString(),
            ["value"] = value.IsNull ? null : value.Kind == TypedConstantKind.Array
                ? Wire.Rows(value.Values.Select(Constant))
                : JsonValue.Create(value.Value is ITypeSymbol type ? Symbols.Display(type) : Symbols.Literal(value.Value))
        };

        return Wire.Rows(attributes.Select(attribute => new JsonObject
        {
            ["type"] = attribute.AttributeClass is null ? null : Symbols.Display(attribute.AttributeClass),
            ["constructor"] = attribute.AttributeConstructor?.GetDocumentationCommentId(),
            ["arguments"] = Wire.Rows(attribute.ConstructorArguments.Select(Constant)),
            ["named"] = new JsonObject(attribute.NamedArguments.OrderBy(arg => arg.Key, StringComparer.Ordinal)
                .Select(arg => KeyValuePair.Create<string, JsonNode?>(arg.Key, Constant(arg.Value))))
        }));
    }

    private static Summary Channels(CompilationUnit unit) =>
        unit.ArgumentDiagnostics.Any(item => item.Severity == DiagnosticSeverity.Error)
            ? new(null, null) : Summaries.GetValue(unit.Compilation, Summarize);

    private static string Key(JsonNode session, string id, string channel) =>
        new JsonArray(session.String("provider"), session.String("stage"), id, channel).ToJsonString();

    public static Dictionary<string, string[]> Install(Workspace workspace, JsonNode session, JsonNode template, CompilationUnit unit)
    {
        var producers = new Dictionary<string, string[]>(StringComparer.Ordinal);
        var loadsExtensions = unit.Loader is not null || unit.Upstream().Any(upstream => upstream.Loader is not null);
        if (loadsExtensions)
            template["cacheable"] = false;
        var reads = new List<JsonNode>();
        var exact = new Dictionary<string, JsonNode>(Workspace.PathComparer);
        var direct = template["direct_inputs"].Items().ToArray();
        var modelBound = direct.Any(input => input.String("kind") is "artifact" or "project_model" &&
            input.String("name") == workspace.ModelPath);
        var complete = true;

        void Files(IEnumerable<string> paths, CompilationUnit producer)
        {
            foreach (var path in paths)
            {
                if (workspace.SourceObject(path) is { } value)
                    exact.TryAdd(path, new JsonObject { ["kind"] = "file", ["name"] = workspace.RelativePath(path), ["object"] = value, ["type"] = "" });
                else if (!modelBound || !producer.Generated.ContainsKey(path))
                    complete = false;
            }
        }

        Files(unit.Trees.Keys.Concat(unit.Inputs), unit);
        foreach (var dependency in unit.Upstream())
        {
            Files(dependency.Inputs, dependency);
            var channels = Channels(dependency);
            if (channels.Declarations is null)
            {
                Files(dependency.Trees.Keys, dependency);
                continue;
            }

            foreach (var channel in new[] { "declarations", "inline" })
            {
                var key = Key(session, dependency.Id, channel);
                reads.Add(new JsonObject
                {
                    ["kind"] = channel == "inline" ? "body" : "lookup", ["project"] = session.String("project"),
                    ["key"] = key, ["value"] = channels.Get(channel), ["channel"] = channel, ["scope"] = dependency.Id,
                    ["search_path"] = new JsonArray(), ["producers"] = new JsonArray()
                });
                producers.Add(key, [dependency.Id]);
            }
        }

        if (complete)
            template["direct_inputs"] = Wire.Rows(direct.Where(input => input.String("kind") != "source")
                .Select(input => input.DeepClone()).Concat(exact.Values));
        if (complete || loadsExtensions)
        {
            template["observed_dependencies"] = Wire.Rows(template["observed_dependencies"].Items().Select(input => input.DeepClone()).Concat(reads));
        }
        else
            producers.Clear();

        return producers;
    }

    public static JsonNode Observe(Workspace workspace, JsonNode session, JsonNode reads)
    {
        var lookup = workspace.Units.Values.SelectMany(unit => new[] { "declarations", "inline" }
            .Select(channel => (Key: Key(session, unit.Id, channel), Unit: unit, Channel: channel)))
            .ToDictionary(item => item.Key, StringComparer.Ordinal);
        var results = new List<JsonNode>();
        foreach (var read in reads.Items())
        {
            var key = read.String("key");
            if (read.String("project") == session.String("project") && read["producer"] is null &&
                read.String("kind") is "lookup" or "declaration" or "body" or "constant" &&
                lookup.TryGetValue(key, out var entry) && read.String("channel") == entry.Channel &&
                Channels(entry.Unit).Get(entry.Channel) is { } value)
                results.Add(new JsonObject { ["kind"] = "channel", ["name"] = key, ["object"] = value, ["type"] = "" });
        }
        return Wire.Rows(results);
    }
}
