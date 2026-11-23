using System.Text.Json.Nodes;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;
using Microsoft.CodeAnalysis.Text;

namespace Fughetta.CSharp;

internal sealed class Projection
{
    private static readonly string[] Facts =
        ["symbols", "types", "definitions", "occurrences", "references", "relations", "scopes", "typed_ranges", "diagnostics"];

    private readonly Workspace workspace;
    private readonly CompilationUnit unit;
    private readonly JsonNode template;
    private readonly string project;
    private readonly string provider;
    private readonly string unitId;
    private readonly string context;
    private readonly SyntaxTree[] trees;
    private readonly HashSet<SyntaxTree> selected;
    private readonly Identities identities;
    private readonly Types types;
    private readonly Dictionary<SyntaxTree, int[]> offsets = new();
    private readonly Dictionary<string, JsonNode> symbols = new(StringComparer.Ordinal);
    private readonly List<JsonNode> occurrences = [];
    private readonly List<JsonNode> references = [];
    private readonly List<JsonObject> relations = [];
    private readonly List<JsonNode> scopes = [];
    private readonly List<JsonNode> typedRanges = [];
    private readonly List<JsonNode> diagnostics = [];
    private readonly HashSet<(string Kind, string From, string To, string? Reference)> relationKeys = new();
    private int unresolved;

    public Projection(Workspace workspace, CompilationUnit unit, JsonNode session, JsonNode template)
    {
        this.workspace = workspace;
        this.unit = unit;
        this.template = template;
        project = session.String("project");
        provider = session.String("provider");
        unitId = template.String("id");
        context = template.String("context");
        var requested = template["coverage"]?["requested"]?["files"].Items()
            .Select(file => workspace.FullPath(file.GetValue<string>())).ToHashSet(Workspace.PathComparer) ?? [];
        trees = unit.Trees.Values.Where(tree => requested.Count == 0 || requested.Contains(tree.FilePath)).ToArray();
        selected = trees.ToHashSet();
        identities = new(workspace, unit, session, selected);
        types = new(identities);
    }

    private JsonObject Place(SyntaxTree tree, TextSpan span)
    {
        if (!offsets.TryGetValue(tree, out var scalars))
        {
            var text = tree.GetText();
            scalars = new int[text.Length + 1];
            var scalar = 0;
            for (var at = 0; at < text.Length; at++)
            {
                scalars[at] = scalar;
                if (char.IsHighSurrogate(text[at]) && at + 1 < text.Length && char.IsLowSurrogate(text[at + 1]))
                    scalars[++at] = scalar;
                scalar++;
            }
            scalars[text.Length] = scalar;
            offsets.Add(tree, scalars);
        }

        return new JsonObject { ["path"] = workspace.RelativePath(tree.FilePath), ["begin"] = scalars[span.Start], ["end"] = scalars[span.End] };
    }

    private JsonObject Location(SyntaxTree? tree, TextSpan span = default)
    {
        var value = new JsonObject { ["project"] = project };
        if (tree is not null)
            value["source"] = Place(tree, span);
        return value;
    }

    private JsonObject Ownership(SyntaxTree? tree, TextSpan span = default) => new()
    {
        ["owner_unit"] = unitId, ["context"] = context,
        ["origin"] = new JsonObject
        {
            ["provider"] = provider,
            ["kind"] = tree is null ? "unknown" : unit.Generated.ContainsKey(tree.FilePath) ? "generated" : "source",
            ["location"] = Location(tree, span)
        }
    };

    private static ISymbol? Declaration(SemanticModel model, SyntaxNode node) => node switch
    {
        AnonymousFunctionExpressionSyntax lambda => (model.GetOperation(lambda) as IAnonymousFunctionOperation)?.Symbol,
        MemberDeclarationSyntax or AccessorDeclarationSyntax or LocalFunctionStatementSyntax or VariableDeclaratorSyntax or
            SingleVariableDesignationSyntax or ParameterSyntax or TypeParameterSyntax or ForEachStatementSyntax or CatchDeclarationSyntax or
            FromClauseSyntax or LetClauseSyntax or JoinClauseSyntax or JoinIntoClauseSyntax or QueryContinuationSyntax or LabeledStatementSyntax
            => model.GetDeclaredSymbol(node),
        _ => null
    };

    private static SyntaxToken Selection(SyntaxNode node) => node switch
    {
        BaseTypeDeclarationSyntax value => value.Identifier,
        DelegateDeclarationSyntax value => value.Identifier,
        BaseNamespaceDeclarationSyntax value => value.Name.GetLastToken(),
        MethodDeclarationSyntax value => value.Identifier,
        ConstructorDeclarationSyntax value => value.Identifier,
        DestructorDeclarationSyntax value => value.Identifier,
        OperatorDeclarationSyntax value => value.OperatorToken,
        ConversionOperatorDeclarationSyntax value => value.ImplicitOrExplicitKeyword,
        LocalFunctionStatementSyntax value => value.Identifier,
        PropertyDeclarationSyntax value => value.Identifier,
        IndexerDeclarationSyntax value => value.ThisKeyword,
        EventDeclarationSyntax value => value.Identifier,
        EnumMemberDeclarationSyntax value => value.Identifier,
        VariableDeclaratorSyntax value => value.Identifier,
        SingleVariableDesignationSyntax value => value.Identifier,
        ParameterSyntax value => value.Identifier,
        TypeParameterSyntax value => value.Identifier,
        ForEachStatementSyntax value => value.Identifier,
        CatchDeclarationSyntax value => value.Identifier,
        FromClauseSyntax value => value.Identifier,
        LetClauseSyntax value => value.Identifier,
        JoinClauseSyntax value => value.Identifier,
        JoinIntoClauseSyntax value => value.Identifier,
        QueryContinuationSyntax value => value.Identifier,
        LabeledStatementSyntax value => value.Identifier,
        AccessorDeclarationSyntax value => value.Keyword,
        SimpleLambdaExpressionSyntax value => value.ArrowToken,
        ParenthesizedLambdaExpressionSyntax value => value.ArrowToken,
        AnonymousMethodExpressionSyntax value => value.DelegateKeyword,
        _ => node.GetFirstToken()
    };

    private static SyntaxNode? Body(SyntaxNode node) => node switch
    {
        BaseMethodDeclarationSyntax method => (SyntaxNode?)method.Body ?? method.ExpressionBody?.Expression,
        LocalFunctionStatementSyntax local => (SyntaxNode?)local.Body ?? local.ExpressionBody?.Expression,
        AccessorDeclarationSyntax accessor => (SyntaxNode?)accessor.Body ?? accessor.ExpressionBody?.Expression,
        PropertyDeclarationSyntax property => (SyntaxNode?)property.AccessorList ?? property.ExpressionBody?.Expression,
        IndexerDeclarationSyntax indexer => (SyntaxNode?)indexer.AccessorList ?? indexer.ExpressionBody?.Expression,
        EventDeclarationSyntax @event => @event.AccessorList,
        AnonymousFunctionExpressionSyntax lambda => lambda.Body,
        _ => null
    };

    private sealed record Scope(JsonObject Record, Scope? Parent, string? Owner, string? Callable)
    {
        public string Id => Record.String("id");
        public JsonArray Members => (JsonArray)Record["declared_members"]!;
        public JsonArray Lookup => (JsonArray)Record["lookup"]!;
    }

    private Scope MakeScope(SyntaxNode node, string kind, Scope? parent, string? owner, string? callable)
    {
        var span = node is CompilationUnitSyntax ? node.FullSpan : node.Span;
        var scope = new JsonObject
        {
            ["id"] = $"{unitId}:scope:{scopes.Count}", ["kind"] = kind, ["location"] = Location(node.SyntaxTree, span),
            ["ownership"] = Ownership(node.SyntaxTree, span), ["lookup"] = new JsonArray(), ["declared_members"] = new JsonArray()
        };
        if (parent is not null)
            scope["lexical_parent"] = parent.Id;
        if (owner is not null)
            scope["semantic_owner"] = owner;
        scopes.Add(scope);
        return new(scope, parent, owner, callable);
    }

    private static string? ScopeKind(SyntaxNode node) => node switch
    {
        CompilationUnitSyntax => "translation_unit",
        BaseNamespaceDeclarationSyntax => "namespace",
        BaseTypeDeclarationSyntax or DelegateDeclarationSyntax => "type",
        AnonymousFunctionExpressionSyntax => "lambda",
        BaseMethodDeclarationSyntax or LocalFunctionStatementSyntax or AccessorDeclarationSyntax => "function",
        BasePropertyDeclarationSyntax => "function",
        BlockSyntax => "block",
        ForStatementSyntax or CommonForEachStatementSyntax or WhileStatementSyntax or DoStatementSyntax => "loop",
        IfStatementSyntax => "if",
        SwitchStatementSyntax or SwitchExpressionArmSyntax => "switch",
        CatchClauseSyntax => "catch",
        _ => null
    };

    private void Declare(ISymbol symbol, string id, SyntaxNode node, TextSpan selection, Scope? scope)
    {
        var tree = node.SyntaxTree;
        var kind = Symbols.Kind(symbol);
        var definition = Symbols.Definition(symbol);
        if (!symbols.ContainsKey(id))
        {
            var extension = new JsonObject { ["clr.assembly"] = unit.Compilation.AssemblyName };
            var modifiers = new List<string>();
            if (Symbols.IsLocal(symbol)) modifiers.Add("local");
            if (symbol.IsStatic) modifiers.Add("static");
            if (symbol.IsAbstract) modifiers.Add("abstract");
            if (symbol.IsVirtual) modifiers.Add("virtual");
            if (symbol.IsOverride) modifiers.Add("override");
            if (symbol.IsSealed) modifiers.Add("sealed");
            if (symbol is IMethodSymbol { IsAsync: true }) modifiers.Add("async");
            if (symbol is IMethodSymbol { IsExtern: true }) modifiers.Add("extern");
            if (definition is IMethodSymbol method && Symbols.Pinvoke(method, extension)) modifiers.Add("pinvoke");
            if (symbol is IFieldSymbol { HasConstantValue: true } field)
            {
                modifiers.Add("literal");
                extension["csharp.constant_value"] = Symbols.Literal(field.ConstantValue);
            }

            var value = new JsonObject
            {
                ["id"] = id, ["language"] = "csharp", ["domain"] = "csharp", ["module"] = unit.Id,
                ["name"] = symbol is IMethodSymbol { MethodKind: MethodKind.Constructor or MethodKind.StaticConstructor }
                    ? symbol.ContainingType.Name : symbol.Name is { Length: > 0 } name ? name : selection.Length == 0 ? "<anonymous>" : "lambda",
                ["qualified_name"] = Symbols.Display(symbol), ["kind"] = kind,
                ["ownership"] = Ownership(tree, node.Span), ["visibility"] = Symbols.Visibility(symbol),
                ["modifiers"] = Wire.Strings(modifiers), ["extensions"] = extension,
                ["native_key"] = identities.Rows[id]["declaration"]!.DeepClone()
            };
            if (identities.Get(symbol.ContainingSymbol) is { } owner && id != owner)
                value["owner"] = owner;
            if (scope is not null)
            {
                value["declaring_scope"] = scope.Id;
                scope.Members.Add(id);
            }
            var parameters = symbol switch
            {
                IMethodSymbol callable => callable.Parameters.AsEnumerable(),
                IPropertySymbol property => property.Parameters,
                INamedTypeSymbol { DelegateInvokeMethod: { } invoke } => invoke.Parameters,
                _ => []
            };
            value["parameters"] = Wire.Rows(parameters.Select(Symbols.Parameter));

            var type = symbol switch
            {
                INamedTypeSymbol { DelegateInvokeMethod: { } invoke } => types.Callable(invoke),
                ITypeSymbol classifier => types.Get(classifier),
                IMethodSymbol callable => types.Callable(callable),
                IPropertySymbol property => types.Get(property.Type, property.ReturnsByRefReadonly ? "ref_readonly" : Symbols.RefKind(property.RefKind)),
                IFieldSymbol fieldSymbol => types.Get(fieldSymbol.Type, Symbols.RefKind(fieldSymbol.RefKind)),
                IEventSymbol @event => types.Get(@event.Type),
                ILocalSymbol local => types.Get(local.Type, Symbols.RefKind(local.RefKind)),
                IParameterSymbol parameter => types.Get(parameter.Type, Symbols.RefKind(parameter.RefKind)),
                _ => null
            };
            if (type is not null)
                value["type"] = type;
            symbols.Add(id, value);
        }
        else if (scope is not null && !scope.Members.Any(value => value!.GetValue<string>() == id))
            scope.Members.Add(id);

        var definitionRole = !symbol.IsAbstract && !symbol.IsExtern &&
            symbol is not IMethodSymbol { IsPartialDefinition: true };
        var body = definitionRole ? Body(node) : null;
        var roles = new List<string> { "declaration" };
        if (definitionRole)
            roles.Add("definition");
        if (unit.Generated.ContainsKey(tree.FilePath) && workspace.SourceObject(tree.FilePath) is null)
            roles.Add("implicit");
        var occurrence = new JsonObject
        {
            ["id"] = $"{unitId}:occurrence:{occurrences.Count}", ["ownership"] = Ownership(tree, node.Span),
            ["symbol"] = id, ["location"] = Location(tree, node.Span), ["selection"] = Place(tree, selection),
            ["roles"] = Wire.Strings(roles), ["declaration_kind"] = kind
        };
        if (body is not null)
            occurrence["body"] = Place(tree, body.Span);
        occurrences.Add(occurrence);
    }

    private void Relation(string kind, string from, ISymbol? toSymbol, SyntaxNode node, string? reference = null)
    {
        if (identities.Get(toSymbol) is not { } to || !relationKeys.Add((kind, from, to, reference)))
            return;
        var row = new JsonObject
        {
            ["id"] = $"{unitId}:relation:{relations.Count}", ["kind"] = kind, ["ownership"] = Ownership(node.SyntaxTree, node.Span),
            ["from"] = from, ["to"] = to, ["from_context"] = context, ["resolution"] = "resolved"
        };
        if (reference is not null)
            row["reference"] = reference;
        relations.Add(row);
    }

    private void Hierarchy(ISymbol symbol, string id, SyntaxNode node, Scope? owned)
    {
        if (symbol is INamedTypeSymbol type && owned is not null)
        {
            void Base(INamedTypeSymbol parent, string relation)
            {
                Relation(relation, id, parent, node);
                if (identities.Get(parent) is { } target)
                    owned.Lookup.Add(new JsonObject { ["kind"] = "base", ["target"] = target });
            }

            if (type.BaseType is { } baseType &&
                baseType.SpecialType is not (SpecialType.System_Object or SpecialType.System_ValueType or
                    SpecialType.System_Enum or SpecialType.System_MulticastDelegate))
                Base(baseType, "inherits");
            foreach (var @interface in type.Interfaces)
                Base(@interface, "implements");

            // A derived type may implement an interface through an inherited member.
            foreach (var implemented in type.Interfaces.SelectMany(@interface => @interface.AllInterfaces.Prepend(@interface))
                         .Distinct<INamedTypeSymbol>(SymbolEqualityComparer.Default))
                foreach (var member in implemented.GetMembers())
                    if (type.FindImplementationForInterfaceMember(member) is { } implementation &&
                        !SymbolEqualityComparer.Default.Equals(type, implementation.ContainingType) &&
                        identities.Get(implementation) is { } implementationId)
                        Relation("implements", implementationId, member, node);
        }

        var overridden = symbol switch
        {
            IMethodSymbol method => (ISymbol?)method.OverriddenMethod,
            IPropertySymbol property => property.OverriddenProperty,
            IEventSymbol @event => @event.OverriddenEvent,
            _ => null
        };
        Relation("overrides", id, overridden, node);
        var explicitMembers = symbol switch
        {
            IMethodSymbol method => method.ExplicitInterfaceImplementations.Cast<ISymbol>(),
            IPropertySymbol property => property.ExplicitInterfaceImplementations.Cast<ISymbol>(),
            IEventSymbol @event => @event.ExplicitInterfaceImplementations.Cast<ISymbol>(),
            _ => []
        };
        foreach (var member in explicitMembers)
            Relation("implements", id, member, node);
        if (symbol is IMethodSymbol or IPropertySymbol or IEventSymbol)
            foreach (var @interface in symbol.ContainingType.AllInterfaces)
                foreach (var member in @interface.GetMembers(symbol.Name))
                    if (SymbolEqualityComparer.Default.Equals(Symbols.Definition(symbol),
                        symbol.ContainingType.FindImplementationForInterfaceMember(member) is { } implementation
                            ? Symbols.Definition(implementation) : null))
                        Relation("implements", id, member, node);
    }

    private string Reference(SyntaxNode node, TextSpan span, SymbolInfo info, IEnumerable<string> roles, string? from, string? dispatch = null)
    {
        var target = identities.Get(info.Symbol);
        var candidates = info.CandidateSymbols.Select(identities.Get).OfType<string>().Distinct(StringComparer.Ordinal).ToArray();
        var resolution = target is not null ? "resolved" : candidates.Length > 1 ? "ambiguous" : "unresolved";
        if (target is null)
            unresolved++;
        var id = $"{unitId}:reference:{references.Count}";
        var row = new JsonObject
        {
            ["id"] = id, ["ownership"] = Ownership(node.SyntaxTree, span), ["location"] = Location(node.SyntaxTree, span),
            ["roles"] = Wire.Strings(roles.Prepend("reference").Distinct()), ["resolution"] = resolution,
            ["candidates"] = Wire.Strings(candidates)
        };
        if (from is not null) row["from"] = from;
        if (target is not null) row["target"] = target;
        if (dispatch is not null) row["dispatch"] = dispatch;
        references.Add(row);
        return id;
    }

    private void Typed(SyntaxNode node, TextSpan span, string type, string? overload = null, JsonObject? substitutions = null)
    {
        var row = new JsonObject
        {
            ["ownership"] = Ownership(node.SyntaxTree, span), ["range"] = Place(node.SyntaxTree, span),
            ["type"] = type, ["substitutions"] = substitutions ?? new JsonObject()
        };
        if (overload is not null)
            row["overload"] = overload;
        typedRanges.Add(row);
    }

    private void Call(SemanticModel model, SyntaxNode node, SyntaxNode? callee, string? from)
    {
        var info = model.GetSymbolInfo(node);
        var span = callee?.Span ?? node.GetFirstToken().Span;
        var method = info.Symbol as IMethodSymbol;
        var operation = model.GetOperation(node);
        var dispatch = method switch
        {
            { MethodKind: MethodKind.DelegateInvoke or MethodKind.FunctionPointerSignature } => "indirect",
            { ContainingType.TypeKind: TypeKind.Interface } => "interface",
            _ when operation is IInvocationOperation { IsVirtual: true } => "virtual",
            _ when method is not null => "direct",
            _ => null
        };
        var reference = Reference(node, span, info, ["call"], from, dispatch);
        if (method is null)
            return;

        if (from is not null)
        {
            Relation("calls", from, method, node, reference);
            if (method.Name == "As" && method.ContainingType?.ToDisplayString() == "System.Runtime.CompilerServices.Unsafe")
                Relation("unsafe_cast", from, method, node, reference);
        }

        var group = node is InvocationExpressionSyntax invocation
            ? model.GetMemberGroup(invocation.Expression).OfType<IMethodSymbol>()
            : method is { MethodKind: MethodKind.Constructor, ContainingType: { } owner } ? owner.InstanceConstructors : [];
        if (method.IsGenericMethod || method.IsVirtual || method.IsOverride || method.IsAbstract ||
            dispatch is "virtual" or "interface" || group.Take(2).Count() > 1)
        {
            var allTypeArguments = new JsonObject();
            for (var type = method.ContainingType; type is not null; type = type.ContainingType)
                for (var index = 0; index < type.TypeArguments.Length; index++)
                    allTypeArguments[types.Parameter(type.TypeParameters[index])] = types.Get(type.TypeArguments[index]);

            var original = (IMethodSymbol)Symbols.Definition(method);
            for (var index = 0; index < method.TypeArguments.Length; index++)
                allTypeArguments[types.Parameter(original.TypeParameters[index])] = types.Get(method.TypeArguments[index]);

            var result = model.GetTypeInfo(node).Type;
            var typeId = result is null ? types.Get(method.ReturnType, Types.ReturnRef(method)) : types.Get(result, Types.ReturnRef(method));
            Typed(node, node.Span, typeId, identities.Get(method), allTypeArguments);
        }
    }

    private static SimpleNameSyntax? LastName(SyntaxNode syntax) => syntax switch
    {
        SimpleNameSyntax name => name,
        MemberAccessExpressionSyntax member => member.Name,
        MemberBindingExpressionSyntax member => member.Name,
        QualifiedNameSyntax qualified => qualified.Right,
        AliasQualifiedNameSyntax alias => alias.Name,
        ParenthesizedExpressionSyntax parentheses => LastName(parentheses.Expression),
        _ => null
    };

    private static bool CallName(SimpleNameSyntax name)
    {
        SyntaxNode expression = name;
        if (name.Parent is MemberAccessExpressionSyntax access && access.Name == name)
            expression = access;
        else if (name.Parent is MemberBindingExpressionSyntax binding)
            expression = binding;
        while (expression.Parent is ParenthesizedExpressionSyntax parent)
            expression = parent;
        return expression.Parent is InvocationExpressionSyntax invocation && invocation.Expression == expression;
    }

    private static IEnumerable<string> NameRoles(SyntaxNode name, ISymbol? symbol)
    {
        if (name.Ancestors().OfType<UsingDirectiveSyntax>().Any())
        {
            yield return "import";
            yield break;
        }

        if (symbol is ITypeSymbol)
            yield return "type";
        if (symbol is not (ILocalSymbol or IFieldSymbol or IPropertySymbol or IParameterSymbol or IRangeVariableSymbol or IEventSymbol))
            yield break;

        SyntaxNode value = name;
        if (name.Parent is MemberAccessExpressionSyntax access && access.Name == name)
            value = access;
        else if (name.Parent is MemberBindingExpressionSyntax binding)
            value = binding;
        while (value.Parent is ParenthesizedExpressionSyntax parent)
            value = parent;

        var usage = value;
        while (usage.Parent is ArgumentSyntax argument && argument.Expression == usage && argument.Parent is TupleExpressionSyntax tuple)
            usage = tuple;
        var assignment = usage.Parent as AssignmentExpressionSyntax;
        var simple = assignment?.Left == usage && assignment.IsKind(SyntaxKind.SimpleAssignmentExpression);
        var compound = assignment?.Left == usage && !simple;
        var increment = value.Parent?.Kind() is SyntaxKind.PreIncrementExpression or SyntaxKind.PreDecrementExpression or
            SyntaxKind.PostIncrementExpression or SyntaxKind.PostDecrementExpression;
        var refKind = (value.Parent as ArgumentSyntax)?.RefKindKeyword.Kind();
        if (!simple && refKind != SyntaxKind.OutKeyword)
            yield return "read";
        if (simple || compound || increment || refKind is SyntaxKind.OutKeyword or SyntaxKind.RefKeyword)
            yield return "write";
    }

    private void Import(SemanticModel model, UsingDirectiveSyntax directive, Scope scope)
    {
        var name = directive.NamespaceOrType;
        if (name is null || identities.Get(model.GetSymbolInfo(name).Symbol) is not { } target)
            return;
        var edge = new JsonObject { ["kind"] = "import", ["target"] = target, ["wildcard"] = directive.Alias is null };
        if (directive.Alias is not null)
            edge["name"] = directive.Alias.Name.Identifier.ValueText;
        scope.Lookup.Add(edge);
    }

    private void Walk(SemanticModel model, SyntaxNode node, Scope? parent, bool skipName = false)
    {
        var declared = Declaration(model, node);
        var token = declared is null ? default : Selection(node);
        var id = declared is not null && token.Span.Length > 0 ? identities.Get(declared) : null;
        var callable = declared is IMethodSymbol or IPropertySymbol or IEventSymbol ? id : null;
        var scopeOwner = node is ForEachStatementSyntax ? parent?.Owner : id ?? parent?.Owner;
        var owned = ScopeKind(node) is { } kind
            ? MakeScope(node, kind, parent, scopeOwner, callable ?? parent?.Callable)
            : null;
        var scope = owned ?? parent!;
        if (declared is not null && id is not null)
        {
            // A catch variable is a member of the catch scope, and a foreach variable of the loop scope.
            Declare(declared, id, node, token.Span, node is ForEachStatementSyntax ? owned : parent);
            if (declared is IRangeVariableSymbol && node is FromClauseSyntax or LetClauseSyntax or JoinClauseSyntax)
            {
                var expression = node switch
                {
                    FromClauseSyntax from => from.Expression,
                    LetClauseSyntax let => let.Expression,
                    JoinClauseSyntax join => join.InExpression,
                    _ => null
                };
                if (expression is not null)
                {
                    var type = model.GetTypeInfo(expression).Type;
                    var element = node is LetClauseSyntax ? type : type switch
                    {
                        IArrayTypeSymbol array => array.ElementType,
                        INamedTypeSymbol named when named.OriginalDefinition.SpecialType == SpecialType.System_Collections_Generic_IEnumerable_T
                            => named.TypeArguments[0],
                        INamedTypeSymbol named => named.AllInterfaces.FirstOrDefault(candidate =>
                            candidate.OriginalDefinition.SpecialType == SpecialType.System_Collections_Generic_IEnumerable_T)?.TypeArguments[0],
                        _ => null
                    };
                    if (element is not null)
                        symbols[id]["type"] = types.Get(element);
                }
            }

            Hierarchy(declared, id, node, owned);
            if (declared is ILocalSymbol local)
            {
                var inferred = node switch
                {
                    VariableDeclaratorSyntax { Parent: VariableDeclarationSyntax { Type.IsVar: true } variable } =>
                        model.GetAliasInfo((IdentifierNameSyntax)variable.Type) is null &&
                        model.GetSymbolInfo(variable.Type).Symbol is not INamedTypeSymbol { Name: "var" },
                    ForEachStatementSyntax { Type.IsVar: true } => true,
                    SingleVariableDesignationSyntax designation => designation.Ancestors().Any(ancestor => ancestor is
                        DeclarationExpressionSyntax { Type.IsVar: true } or VarPatternSyntax),
                    _ => false
                };
                if (inferred)
                    Typed(node, token.Span, types.Get(local.Type, Symbols.RefKind(local.RefKind)));
            }
            if (declared is IParameterSymbol parameter && node is ParameterSyntax { Type: null } &&
                node.Ancestors().Any(ancestor => ancestor is AnonymousFunctionExpressionSyntax))
                Typed(node, token.Span, types.Get(parameter.Type, Symbols.RefKind(parameter.RefKind)));
        }

        switch (node)
        {
            case UsingDirectiveSyntax directive:
                Import(model, directive, scope);
                break;
            case InvocationExpressionSyntax invocation:
                Call(model, node, LastName(invocation.Expression) ?? invocation.Expression, scope.Callable);
                break;
            case ObjectCreationExpressionSyntax creation:
                Call(model, node, (SyntaxNode?)LastName(creation.Type) ?? creation.Type, scope.Callable);
                break;
            case ImplicitObjectCreationExpressionSyntax or ConstructorInitializerSyntax:
                Call(model, node, null, scope.Callable);
                break;
            case AttributeSyntax attribute:
                Call(model, node, (SyntaxNode?)LastName(attribute.Name) ?? attribute.Name, scope.Callable);
                break;
            case ElementAccessExpressionSyntax or ElementBindingExpressionSyntax:
                var indexer = model.GetSymbolInfo(node);
                if (indexer.Symbol is not null || indexer.CandidateSymbols.Length != 0)
                    Reference(node, node.Span, indexer, NameRoles(node, indexer.Symbol), scope.Callable);
                break;
            case SimpleNameSyntax name when !skipName && !CallName(name) &&
                name.Parent is not NameEqualsSyntax &&
                !name.Ancestors().OfType<BaseNamespaceDeclarationSyntax>().Any(ns => ns.Name.Span.Contains(name.Span)):
                var info = model.GetSymbolInfo(name);
                var alias = name is IdentifierNameSyntax identifier ? model.GetAliasInfo(identifier) : null;
                if (info.Symbol is not null || info.CandidateSymbols.Length != 0 || alias is not null ||
                    !name.IsVar && !name.Identifier.IsMissing)
                    Reference(name, name.Identifier.Span, info, NameRoles(name, alias?.Target ?? info.Symbol), scope.Callable);
                break;
        }

        foreach (var child in node.ChildNodes())
            Walk(model, child, scope, node is UsingDirectiveSyntax { Alias: { } aliasName } && child == aliasName);
    }

    private static readonly DiagnosticDescriptor UnresolvedScriptPackage = new(
        "FUGCS1001", "Script package is not available", "Script package '{0}' is not in the owning project's resolved references",
        "Script", DiagnosticSeverity.Warning, isEnabledByDefault: true);

    private void Diagnostics()
    {
        var seen = new HashSet<(string Code, string Message, SyntaxTree? Tree, TextSpan Span)>();
        var compilerDiagnostics = selected.Count == unit.Trees.Count
            ? unit.Compilation.GetDiagnostics().AsEnumerable()
            : unit.ProjectDiagnostics().Concat(trees.SelectMany(tree => unit.Compilation.GetSemanticModel(tree).GetDiagnostics()));
        var analyzerDiagnostics = unit.AnalyzerRun is { } analyzers
            ? selected.Count == unit.Trees.Count
                ? analyzers.GetAnalyzerDiagnosticsAsync().GetAwaiter().GetResult().AsEnumerable()
                : trees.SelectMany(tree => analyzers.GetAnalyzerSyntaxDiagnosticsAsync(tree, CancellationToken.None).GetAwaiter().GetResult()
                    .Concat(analyzers.GetAnalyzerSemanticDiagnosticsAsync(unit.Compilation.GetSemanticModel(tree), null, CancellationToken.None).GetAwaiter().GetResult()))
            : Enumerable.Empty<Diagnostic>();
        foreach (var reported in unit.ArgumentDiagnostics.Concat(compilerDiagnostics).Concat(analyzerDiagnostics))
        {
            var diagnostic = reported;
            if (reported.Id == "CS0006" && reported.Location.SourceTree is { } script &&
                script.Options.Kind == SourceCodeKind.Script &&
                script.GetRoot().DescendantTrivia(reported.Location.SourceSpan).Select(trivia => trivia.GetStructure())
                    .OfType<ReferenceDirectiveTriviaSyntax>().FirstOrDefault(directive =>
                        directive.File.ValueText.StartsWith("nuget:", StringComparison.OrdinalIgnoreCase)) is { } package)
                diagnostic = Diagnostic.Create(UnresolvedScriptPackage, reported.Location, package.File.ValueText);
            var location = diagnostic.Location;
            if (location.IsInSource && !selected.Contains(location.SourceTree!))
                continue;
            var message = diagnostic.GetMessage(System.Globalization.CultureInfo.InvariantCulture);
            if (!seen.Add((diagnostic.Id, message, location.SourceTree, location.SourceSpan)))
                continue;
            diagnostics.Add(new JsonObject
            {
                ["id"] = $"{unitId}:diagnostic:{diagnostics.Count}", ["producer"] = provider, ["code"] = diagnostic.Id,
                ["severity"] = diagnostic.Severity switch
                {
                    DiagnosticSeverity.Error => "error", DiagnosticSeverity.Warning => "warning",
                    DiagnosticSeverity.Info => "information", _ => "hint"
                },
                ["message"] = message, ["ownership"] = Ownership(location.SourceTree, location.SourceSpan),
                ["location"] = Location(location.SourceTree, location.SourceSpan),
                ["related_locations"] = Wire.Rows(diagnostic.AdditionalLocations
                    .Where(value => value.SourceTree is null || selected.Contains(value.SourceTree))
                    .Select(value => Location(value.SourceTree, value.SourceSpan))),
                ["fixes"] = new JsonObject()
            });
        }
    }

    public JsonObject Project()
    {
        var globalImports = unit.Trees.Values.SelectMany(tree => ((CompilationUnitSyntax)tree.GetRoot()).Usings)
            .Where(directive => directive.GlobalKeyword.IsKind(SyntaxKind.GlobalKeyword)).ToArray();
        foreach (var tree in trees)
        {
            var model = unit.Compilation.GetSemanticModel(tree);
            var root = tree.GetRoot();
            var scopeIndex = scopes.Count;
            Walk(model, root, null);
            var rootScope = new Scope((JsonObject)scopes[scopeIndex], null, null, null);
            foreach (var import in globalImports)
                if (import.SyntaxTree != tree)
                    Import(unit.Compilation.GetSemanticModel(import.SyntaxTree), import, rootScope);
        }
        Diagnostics();

        foreach (var reference in references)
            if (symbols.ContainsKey(reference.String("target")))
                reference["target_context"] = context;
        foreach (var relation in relations)
        {
            if (!symbols.ContainsKey(relation.String("from")))
                relation.Remove("from_context");
            if (symbols.ContainsKey(relation.String("to")))
                relation["to_context"] = context;
        }

        var rows = new Dictionary<string, IEnumerable<JsonNode>>
        {
            ["symbols"] = symbols.Values, ["types"] = types.Rows.Values, ["occurrences"] = occurrences,
            ["references"] = references, ["relations"] = relations, ["scopes"] = scopes, ["diagnostics"] = diagnostics
        };
        var files = trees.Select(tree => workspace.RelativePath(tree.FilePath)).ToArray();
        var contributions = new JsonObject { ["files"] = Wire.Strings(files) };
        var shard = new JsonObject
        {
            ["id"] = "", ["unit"] = template, ["identities"] = Wire.Rows(identities.Rows.Values),
            ["typed_ranges"] = Wire.Rows(typedRanges)
        };
        foreach (var (key, records) in rows)
        {
            contributions[key] = Wire.Strings(records.Select(row => row.String("id")));
            shard[key] = Wire.Rows(records);
        }

        template["contributions"] = contributions;
        template["modules"] = Wire.Strings([unit.Id]);
        template["coverage"] = new JsonObject
        {
            ["requested"] = template["coverage"]?["requested"]?.DeepClone(),
            ["available"] = new JsonArray(new JsonObject
            {
                ["files"] = Wire.Strings(files), ["modules"] = Wire.Strings([unit.Id]),
                ["contexts"] = Wire.Strings([context]), ["projects"] = Wire.Strings([project]), ["facts"] = Wire.Strings(Facts)
            }),
            ["missing"] = new JsonArray(), ["unresolved_count"] = unresolved
        };
        return shard;
    }
}
