using System.Collections.Immutable;
using System.Reflection;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Text;

namespace Fughetta.CSharp;

internal sealed class CompilationUnit(
    string id, CSharpCompilation compilation, Dictionary<string, SyntaxTree> trees,
    Dictionary<string, string> generated, string[] inputs, Dictionary<string, CompilationUnit> dependencies,
    ImmutableArray<Diagnostic> argumentDiagnostics)
{
    private Dictionary<MetadataReferenceProperties, CompilationReference> references = new();

    public string Id { get; } = id;
    public CSharpCompilation Compilation { get; } = compilation;
    public CSharpCompilation BaseCompilation { get; init; } = compilation;
    public PluginInputs? Plugins { get; init; }
    public AnalyzerLoader? Loader => Plugins?.Loader;
    public AnalyzerInputs? ExtensionInputs { get; init; }
    public GeneratorState? Generation { get; init; }
    public CompilationWithAnalyzers? AnalyzerRun { get; init; }
    private static readonly System.Runtime.CompilerServices.ConditionalWeakTable<CSharpCompilation, Lazy<ImmutableArray<Diagnostic>>> GlobalDiagnostics = new();
    public ImmutableArray<Diagnostic> ProjectDiagnostics() => GlobalDiagnostics.GetValue(Compilation, compilation => new(() =>
    {
        // Roslyn reports entry-point diagnostics during method compilation for executable outputs.
        var diagnostics = compilation.Options.OutputKind is OutputKind.ConsoleApplication or OutputKind.WindowsApplication or OutputKind.WindowsRuntimeApplication
            ? compilation.GetDiagnostics() : compilation.GetDeclarationDiagnostics();
        return diagnostics.Where(diagnostic => !diagnostic.Location.IsInSource).ToImmutableArray();
    })).Value;
    public Dictionary<string, SyntaxTree> Trees { get; } = trees;
    public Dictionary<string, string> Generated { get; } = generated;
    public string[] Inputs { get; } = inputs;
    public Dictionary<string, CompilationUnit> Dependencies { get; } = dependencies;
    public ImmutableArray<Diagnostic> ArgumentDiagnostics { get; } = argumentDiagnostics;

    public void ReuseReferences(CompilationUnit previous) => references = previous.references;

    public CompilationReference Reference(MetadataReferenceProperties properties)
    {
        if (!references.TryGetValue(properties, out var reference))
        {
            reference = Compilation.ToMetadataReference(properties.Aliases, properties.EmbedInteropTypes);
            references.Add(properties, reference);
        }

        return reference;
    }

    public CompilationUnit[] Upstream()
    {
        var visited = new Dictionary<string, CompilationUnit>(StringComparer.Ordinal);

        void Visit(CompilationUnit unit)
        {
            foreach (var dependency in unit.Dependencies.Values)
                if (visited.TryAdd(dependency.Id, dependency))
                    Visit(dependency);
        }

        Visit(this);
        return visited.Values.OrderBy(unit => unit.Id, StringComparer.Ordinal).ToArray();
    }
}

internal sealed class Workspace(string root, JsonNode host) : IDisposable
{
    internal static readonly StringComparer PathComparer = OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    private static readonly Regex InputToken = new(@"\${(tree|object:[^}]+|sdk:[^}]+)\}");
    private static readonly DiagnosticDescriptor AnalyzerLoadFailure = new(
        "CS8032", "Analyzer could not be loaded", "Could not load analyzer '{0}': {1}",
        "Compiler", DiagnosticSeverity.Warning, isEnabledByDefault: true);
    private static readonly DiagnosticDescriptor MissingAssembly = new(
        "CS0006", "Metadata file missing", "Metadata file '{0}' could not be found",
        "Compiler", DiagnosticSeverity.Error, isEnabledByDefault: true);
    private static readonly DiagnosticDescriptor InvalidAssembly = new(
        "CS0009", "Invalid metadata file", "Metadata file '{0}' could not be opened: {1}",
        "Compiler", DiagnosticSeverity.Error, isEnabledByDefault: true);

    private readonly string objects = host.String("objects");
    private readonly Dictionary<(string Path, string Version, MetadataReferenceProperties Properties), PortableExecutableReference> metadata = new();
    private readonly Dictionary<string, string> aliases = new(StringComparer.Ordinal);
    private Dictionary<string, string> documents = new(PathComparer);
    private Dictionary<string, string> sourceObjects = new(PathComparer);
    private DirectoryInfo? staging;

    public Dictionary<string, CompilationUnit> Units { get; private set; } = new(StringComparer.Ordinal);
    public string ModelPath { get; private set; } = "";
    public string FullPath(string path) => Path.GetFullPath(path, root);
    public string RelativePath(string path) => Path.GetRelativePath(root, path).Replace('\\', '/');
    public string? SourceObject(string path) => sourceObjects.GetValueOrDefault(path);

    private string ObjectPath(string hash)
    {
        if (hash.Length == 0 || hash is "." or ".." || Path.GetFileName(hash) != hash)
            throw new InvalidOperationException("Invalid project model object name");
        return Path.Combine(objects, hash);
    }
    private string Sdk(string name) => host["sdks"]?[name]?.GetValue<string>()
        ?? throw new InvalidOperationException($"SDK is not installed: {name}");

    private string AssemblyAlias(string hash, string? name = null)
    {
        name ??= hash + ".dll";
        if (name is "" or "." or ".." || Path.GetFileName(name) != name)
            throw new InvalidOperationException("Invalid assembly file name");
        var key = hash + "/" + name;
        if (!aliases.TryGetValue(key, out var path))
        {
            staging ??= Directory.CreateTempSubdirectory("fughetta-csharp-");
            path = Path.Combine(staging.FullName, hash, name);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.Copy(ObjectPath(hash), path, overwrite: true);
            aliases.Add(key, path);
        }

        return path;
    }

    private string Expand(string argument, Dictionary<string, string> objectPaths) => InputToken.Replace(argument, match =>
    {
        var token = match.Groups[1].Value;
        if (token == "tree")
            return root;
        var name = token[(token.IndexOf(':') + 1)..];
        return token.StartsWith("object:", StringComparison.Ordinal)
            ? objectPaths.GetValueOrDefault(name) ?? ObjectPath(name)
            : Sdk(name);
    });

    public void Install(JsonNode parameters)
    {
        var artifacts = parameters["artifacts"].Properties().ToDictionary(entry => entry.Key, entry => entry.Value!.GetValue<string>());
        var models = artifacts.Keys.Where(path => path.EndsWith("model.json", StringComparison.Ordinal)).ToArray();
        if (models.Length != 1)
            throw new InvalidOperationException("C# helper requires one project_model");

        var model = JsonNode.Parse(File.ReadAllText(FullPath(models[0])))!;
        if (model["schema"]?.GetValue<int>() != 1)
            throw new InvalidOperationException("Unknown project model schema");
        var allUnits = model["units"].Items().ToDictionary(unit => unit.String("id"), StringComparer.Ordinal);
        var csharp = allUnits.Where(entry => entry.Value.String("language") == "csharp")
            .ToDictionary(StringComparer.Ordinal);
        if (csharp.Count == 0)
            throw new InvalidOperationException("project_model has no C# compilation units");

        var nextSourceObjects = parameters["sources"].Properties().ToDictionary(
            entry => FullPath(entry.Key), entry => entry.Value!.GetValue<string>(), PathComparer);
        var changed = parameters["changed"].Items().Select(value => FullPath(value.GetValue<string>())).ToHashSet(PathComparer);
        var nextDocuments = new Dictionary<string, string>(documents, PathComparer);
        var provided = parameters["documents"].Items().ToDictionary(doc => FullPath(doc.String("path")), doc => doc.String("text"), PathComparer);
        var currentSources = csharp.Values.SelectMany(unit => Paths(unit["sources"])).ToHashSet(PathComparer);
        var scripts = currentSources.Concat(nextSourceObjects.Keys).Concat(provided.Keys)
            .Where(path => IsInTree(path) && Path.GetExtension(path).Equals(".csx", StringComparison.OrdinalIgnoreCase))
            .Distinct(PathComparer).Order(PathComparer).ToArray();
        currentSources.UnionWith(scripts);
        foreach (var path in nextDocuments.Keys.ToArray())
            if (!currentSources.Contains(path) ||
                nextSourceObjects.GetValueOrDefault(path) != sourceObjects.GetValueOrDefault(path))
                nextDocuments.Remove(path);
        foreach (var (path, value) in provided)
            if (currentSources.Contains(path))
                nextDocuments[path] = value;

        var built = new Dictionary<string, CompilationUnit>(StringComparer.Ordinal);
        var visiting = new HashSet<string>(StringComparer.Ordinal);
        var newLoaders = new List<AnalyzerLoader>();
        var usedMetadata = new HashSet<(string Path, string Version, MetadataReferenceProperties Properties)>();
        var installedMetadata = new Dictionary<(string Path, MetadataReferenceProperties Properties), PortableExecutableReference>();

        IEnumerable<string> Paths(JsonNode? paths) => paths.Items().Select(value => FullPath(value.GetValue<string>()));

        JsonNode ProjectTarget(JsonNode reference)
        {
            var matches = allUnits.Values.Where(unit => unit.String("project") == reference.String("project") &&
                (reference.String("unit") is { Length: > 0 } unitId
                    ? unit.String("id") == unitId
                    : unit["variants"].Items().Any(variant => JsonNode.DeepEquals(variant, reference["attributes"])))).ToArray();
            return matches.Length == 1 ? matches[0] : throw new InvalidOperationException("C# project reference requires one unit");
        }

        CompilationUnit Build(string id)
        {
            if (built.TryGetValue(id, out var result))
                return result;
            if (!visiting.Add(id))
                throw new InvalidOperationException("Cyclic C# project references");

            var unit = csharp[id];
            var directory = FullPath(unit.String("directory") is { Length: > 0 } unitDirectory ? unitDirectory : ".");
            var generated = unit["generated"].Properties().ToDictionary(
                entry => FullPath(entry.Key), entry => entry.Value!.String("hash"), PathComparer);
            var previous = Units.GetValueOrDefault(id);
            var inputs = new HashSet<string>(Paths(unit["extensions"]?["additional_files"]), PathComparer);
            var dependencies = new Dictionary<string, CompilationUnit>(StringComparer.Ordinal);
            var objectPaths = new Dictionary<string, string>(StringComparer.Ordinal);
            var imports = new Dictionary<string, CompilationUnit?>(PathComparer);
            var binaryPaths = new Dictionary<string, string>(PathComparer);
            var plugins = new HashSet<string>(PathComparer);
            var pluginDependencies = new HashSet<string>(PathComparer);

            foreach (var reference in unit["references"].Items())
            {
                var kind = reference.String("kind");
                var role = reference.String("role");
                var isClasspath = role is "" or "classpath";
                if (role is "plugin" or "plugin_dependency")
                {
                    string ResolvePlugin() => kind switch
                    {
                        "artifact" => AssemblyAlias(reference["object"]!.String("hash"), reference["object"]!.String("name")),
                        "sdk" => Path.GetFullPath(reference.String("path"), Sdk(reference.String("name"))),
                        "tree" => FullPath(reference.String("path")),
                        _ => throw new InvalidOperationException("C# analyzer reference requires an assembly")
                    };
                    var plugin = ResolvePlugin();
                    if (kind == "tree")
                        inputs.Add(plugin);
                    (role == "plugin" ? plugins : pluginDependencies).Add(plugin);
                    if (kind == "artifact")
                        objectPaths[reference["object"]!.String("hash")] = plugin;
                    continue;
                }
                if (kind == "artifact")
                {
                    var hash = reference["object"]!.String("hash");
                    if (isClasspath)
                        imports[objectPaths[hash] = AssemblyAlias(hash, reference["object"]!.String("name"))] = null;
                    continue;
                }

                if (kind == "tree")
                    inputs.Add(FullPath(reference.String("path")));
                if (!isClasspath)
                    continue;

                switch (kind)
                {
                    case "sdk":
                        imports[Path.GetFullPath(reference.String("path"), Sdk(reference.String("name")))] = null;
                        break;
                    case "tree":
                        imports[FullPath(reference.String("path"))] = null;
                        break;
                    case "project":
                        var target = ProjectTarget(reference);
                        var targetId = target.String("id");
                        var declaredPath = reference.String("path");
                        var outputs = Paths(target["outputs"]).ToArray();
                        if (csharp.ContainsKey(targetId))
                        {
                            var upstream = Build(targetId);
                            dependencies[targetId] = upstream;
                            if (declaredPath != "")
                                imports[FullPath(declaredPath)] = upstream;
                            foreach (var output in outputs)
                                imports[output] = upstream;
                        }
                        else
                        {
                            var paths = declaredPath == "" ? outputs : [FullPath(declaredPath), .. outputs];
                            var binary = paths.FirstOrDefault(File.Exists) ?? paths.FirstOrDefault()
                                ?? throw new InvalidOperationException($"Project reference has no assembly output: {targetId}");
                            inputs.Add(binary);
                            imports[binary] = null;
                            foreach (var alias in paths)
                                binaryPaths[alias] = binary;
                        }
                        break;
                }
            }

            var sources = Paths(unit["sources"]).Where(source => !scripts.Contains(source, PathComparer)).Distinct(PathComparer).ToArray();
            var args = unit["compiler_arguments"].Items().Select(value => Expand(value.GetValue<string>(), objectPaths)).ToArray();
            var parsed = CSharpCommandLineParser.Default.Parse(
                args.Length == 0 ? ["/target:library", .. sources] : args, directory, sdkDirectory: null);
            var parseOptions = parsed.ParseOptions;
            var compilationOptions = parsed.CompilationOptions;
            if (args.Length == 0)
            {
                var extensions = unit["extensions"];
                if (extensions is not null && LanguageVersionFacts.TryParse(extensions.String("language_version"), out var version))
                    parseOptions = parseOptions.WithLanguageVersion(version);
                parseOptions = parseOptions.WithPreprocessorSymbols(extensions?["defines"].Items()
                    .Select(define => define.GetValue<string>()) ?? []);
                compilationOptions = compilationOptions.WithOutputKind(OutputKind.DynamicallyLinkedLibrary);
            }

            var diagnostics = parsed.Errors.ToBuilder();
            PluginInputs? pluginState = null;
            AnalyzerInputs? extensionInputs = null;
            AnalyzerOptions? analyzerOptions = null;
            var analyzers = ImmutableArray<DiagnosticAnalyzer>.Empty;
            plugins.UnionWith(parsed.AnalyzerReferences.Select(reference => Path.GetFullPath(reference.FilePath, directory)));
            if (plugins.Count != 0)
            {
                var entrypoints = plugins.Order(PathComparer).ToArray();
                var files = plugins.Concat(pluginDependencies).Distinct(PathComparer).Order(PathComparer).Select(path =>
                {
                    var file = new FileInfo(path);
                    return (Path: path, Length: file.Exists ? file.Length : -1, file.LastWriteTimeUtc,
                        Version: nextSourceObjects.GetValueOrDefault(path) ?? artifacts.GetValueOrDefault(RelativePath(path)));
                }).ToArray();
                pluginState = previous?.Plugins;
                if (pluginState is null || !entrypoints.SequenceEqual(pluginState.Entrypoints, PathComparer) ||
                    !files.SequenceEqual(pluginState.Files) || files.Any(file => changed.Contains(file.Path)))
                {
                    var loader = new AnalyzerLoader();
                    newLoaders.Add(loader);
                    foreach (var file in files)
                        loader.AddDependencyLocation(file.Path);
                    var errors = ImmutableArray.CreateBuilder<Diagnostic>();
                    var loadedAnalyzers = ImmutableArray.CreateBuilder<DiagnosticAnalyzer>();
                    var loadedGenerators = ImmutableArray.CreateBuilder<ISourceGenerator>();
                    var generatorAssemblies = new HashSet<string>(PathComparer);
                    foreach (var path in entrypoints)
                    {
                        var analyzer = new AnalyzerFileReference(path, loader);
                        void LoadFailed(object? sender, AnalyzerLoadFailureEventArgs failure)
                        {
                            if (failure.ErrorCode != AnalyzerLoadFailureEventArgs.FailureErrorCode.NoAnalyzers)
                                errors.Add(Diagnostic.Create(AnalyzerLoadFailure, Location.None, RelativeOrAbsolute(path), failure.Message));
                        }
                        analyzer.AnalyzerLoadFailed += LoadFailed;
                        var generators = analyzer.GetGenerators(LanguageNames.CSharp);
                        if (!generators.IsEmpty)
                            generatorAssemblies.Add(AssemblyName.GetAssemblyName(path).Name!);
                        loadedGenerators.AddRange(generators);
                        loadedAnalyzers.AddRange(analyzer.GetAnalyzers(LanguageNames.CSharp));
                        analyzer.AnalyzerLoadFailed -= LoadFailed;
                    }
                    pluginState = new PluginInputs(loader, files, entrypoints, loadedAnalyzers.ToImmutable(),
                        loadedGenerators.ToImmutable(), generatorAssemblies, errors.ToImmutable());
                }
                diagnostics.AddRange(pluginState.Diagnostics);
                if (!parsed.SkipAnalyzers)
                    analyzers = pluginState.Analyzers;

                var configs = parsed.AnalyzerConfigPaths.Select(path => Path.GetFullPath(path, directory))
                    .Select(path => (Path: path, Content: File.Exists(path) ? File.ReadAllText(path) : null)).ToArray();
                inputs.UnionWith(configs.Select(config => config.Path));
                var additional = parsed.AdditionalFiles.Select(file => Path.GetFullPath(file.Path, directory)).Concat(
                    Paths(unit["extensions"]?["additional_files"])).Distinct(PathComparer).ToArray();
                inputs.UnionWith(additional);
                var previousInputs = previous?.ExtensionInputs;
                var previousFiles = previousInputs?.AdditionalFiles.ToDictionary(file => file.Path, PathComparer);
                var additionalFiles = additional.Select(path =>
                {
                    var input = new AnalyzerInput(path, File.Exists(path) ? SourceText.From(File.ReadAllText(path)) : null);
                    return previousFiles?.GetValueOrDefault(path) is { } old && old.SameContent(input) ? old : input;
                }).ToImmutableArray();
                if (previousInputs is not null && configs.SequenceEqual(previousInputs.ConfigFiles))
                {
                    extensionInputs = additionalFiles.SequenceEqual(previousInputs.AdditionalFiles)
                        ? previousInputs : previousInputs with { AdditionalFiles = additionalFiles };
                }
                else
                {
                    var configSet = AnalyzerConfigSet.Create(configs.Where(config => config.Content is not null)
                        .Select(config => AnalyzerConfig.Parse(config.Content!, config.Path)).ToArray(), out var configDiagnostics);
                    extensionInputs = new AnalyzerInputs(configs, additionalFiles, new AnalyzerSettings(configSet),
                        new DiagnosticSettings(configSet), configDiagnostics);
                }
                diagnostics.AddRange(extensionInputs.Diagnostics);
                analyzerOptions = new AnalyzerOptions([.. extensionInputs.AdditionalFiles], extensionInputs.Settings);
                compilationOptions = compilationOptions.WithSyntaxTreeOptionsProvider(extensionInputs.DiagnosticSettings);
            }
            var references = new List<MetadataReference>();
            var added = new HashSet<MetadataReference>(ReferenceEqualityComparer.Instance);
            var representedProjects = new HashSet<string>(StringComparer.Ordinal);
            var explicitPaths = new HashSet<string>(PathComparer);

            void AddReference(string file, MetadataReferenceProperties properties)
            {
                if (imports.TryGetValue(file, out var upstream) && upstream is not null)
                {
                    var reference = upstream.Reference(properties);
                    if (added.Add(reference))
                        references.Add(reference);
                    representedProjects.Add(upstream.Id);
                    return;
                }

                try
                {
                    if (!File.Exists(file))
                    {
                        diagnostics.Add(Diagnostic.Create(MissingAssembly, Location.None, RelativeOrAbsolute(file)));
                        return;
                    }

                    var relative = RelativePath(file);
                    var version = nextSourceObjects.GetValueOrDefault(file) ?? artifacts.GetValueOrDefault(relative);
                    PortableExecutableReference reference;
                    if (version is null && IsInTree(file))
                    {
                        // Build outputs can change independently of source notifications.
                        var installedKey = (file, properties);
                        if (!installedMetadata.TryGetValue(installedKey, out reference!))
                        {
                            reference = MetadataReference.CreateFromImage(File.ReadAllBytes(file), properties, filePath: file);
                            installedMetadata.Add(installedKey, reference);
                        }
                    }
                    else
                    {
                        var key = (file, version ?? "", properties);
                        if (!metadata.TryGetValue(key, out reference!))
                        {
                            reference = IsInTree(file)
                                ? MetadataReference.CreateFromImage(File.ReadAllBytes(file), properties, filePath: file)
                                : MetadataReference.CreateFromFile(file, properties);
                            metadata.Add(key, reference);
                        }
                        usedMetadata.Add(key);
                    }

                    if (added.Add(reference))
                        references.Add(reference);
                }
                catch (Exception error) when (error is IOException or BadImageFormatException)
                {
                    diagnostics.Add(Diagnostic.Create(InvalidAssembly, Location.None, RelativeOrAbsolute(file), error.Message));
                }
            }

            foreach (var reference in parsed.MetadataReferences)
            {
                var file = Path.GetFullPath(reference.Reference, directory);
                if (!imports.ContainsKey(file) && !File.Exists(file) && !Path.IsPathFullyQualified(reference.Reference))
                    file = parsed.ReferencePaths.Select(search => Path.GetFullPath(reference.Reference, search))
                        .FirstOrDefault(candidate => imports.ContainsKey(candidate) || File.Exists(candidate)) ?? file;
                file = binaryPaths.GetValueOrDefault(file) ?? file;
                explicitPaths.Add(file);
                AddReference(file, reference.Properties);
                // Command-line-only binaries in the installed tree are exact inputs as well.
                if (!imports.ContainsKey(file) && IsInTree(file))
                    inputs.Add(file);
            }

            foreach (var (file, upstream) in imports)
                if (!explicitPaths.Contains(file) &&
                    (upstream is null || !representedProjects.Contains(upstream.Id)))
                    AddReference(file, MetadataReferenceProperties.Assembly);

            foreach (var dependency in dependencies.Values)
            {
                if (representedProjects.Add(dependency.Id))
                    references.Add(dependency.Reference(MetadataReferenceProperties.Assembly));

                foreach (var transitive in dependency.Upstream())
                    if (representedProjects.Add(transitive.Id))
                        references.Add(transitive.Reference(MetadataReferenceProperties.Assembly));
            }

            var trees = new Dictionary<string, SyntaxTree>(PathComparer);
            var generatorOutput = unit["extensions"]?.String("compiler_generated_files");
            var generatorDirectory = string.IsNullOrEmpty(generatorOutput) ? null : FullPath(generatorOutput);
            bool ReplacedByGenerator(string source)
            {
                if (generatorDirectory is null || pluginState is null || !Within(generatorDirectory, source))
                    return false;
                var path = Path.GetRelativePath(generatorDirectory, source);
                var segment = path.IndexOf(Path.DirectorySeparatorChar);
                return segment > 0 && pluginState.GeneratorAssemblies.Contains(path[..segment]);
            }
            var previousBaseTrees = previous?.BaseCompilation.SyntaxTrees.ToDictionary(tree => tree.FilePath, PathComparer);
            foreach (var source in sources.Where(source => !ReplacedByGenerator(source)))
            {
                var oldTree = previousBaseTrees?.GetValueOrDefault(source);
                var generatedHash = generated.GetValueOrDefault(source);
                var oldGeneratedHash = previous?.Generated.GetValueOrDefault(source);
                var content = provided.GetValueOrDefault(source) ??
                    (generatedHash == oldGeneratedHash ? nextDocuments.GetValueOrDefault(source) : null);
                SourceText text;
                if (content is not null)
                    text = SourceText.From(content, Encoding.UTF8);
                else if (generatedHash is not null)
                {
                    nextDocuments.Remove(source);
                    text = generatedHash == oldGeneratedHash && oldTree is not null
                        ? oldTree.GetText()
                        : SourceText.From(File.ReadAllText(ObjectPath(generatedHash)), Encoding.UTF8);
                }
                else if (oldTree is not null && !changed.Contains(source) &&
                         nextSourceObjects.GetValueOrDefault(source) == sourceObjects.GetValueOrDefault(source) &&
                         !documents.ContainsKey(source) && oldGeneratedHash is null)
                    text = oldTree.GetText();
                else
                {
                    var encoding = parsed.Encoding ?? Encoding.UTF8;
                    text = SourceText.From(File.ReadAllText(source, encoding), encoding);
                }

                trees.Add(source, oldTree is not null && oldTree.Options.Equals(parseOptions)
                    ? oldTree.GetText().ContentEquals(text) ? oldTree : oldTree.WithChangedText(text)
                    : CSharpSyntaxTree.ParseText(text, parseOptions, source));
            }

            var assembly = unit["extensions"]?["assembly"]?.GetValue<string>();
            if (string.IsNullOrEmpty(assembly))
                assembly = parsed.CompilationName ?? Path.GetFileNameWithoutExtension(unit.String("project"));
            var compilation = previous?.BaseCompilation ?? CSharpCompilation.Create(assembly, options: compilationOptions);
            if (compilation.AssemblyName != assembly)
                compilation = compilation.WithAssemblyName(assembly);
            if (!compilation.Options.Equals(compilationOptions))
                compilation = compilation.WithOptions(compilationOptions);
            if (!compilation.References.SequenceEqual(references))
                compilation = compilation.WithReferences(references);
            if (compilation.SyntaxTrees.Select(tree => tree.FilePath).SequenceEqual(trees.Keys, PathComparer))
            {
                foreach (var tree in compilation.SyntaxTrees)
                    if (!ReferenceEquals(tree, trees[tree.FilePath]))
                        compilation = compilation.ReplaceSyntaxTree(tree, trees[tree.FilePath]);
            }
            else
                compilation = compilation.RemoveAllSyntaxTrees().AddSyntaxTrees(trees.Values);

            var baseCompilation = compilation;
            GeneratorState? generation = null;
            if (pluginState is { Generators.Length: > 0 })
            {
                var outputRoot = generatorDirectory ?? Path.Combine(root, "obj", "fughetta-generated");
                var prior = ReferenceEquals(previous?.Plugins, pluginState) ? previous?.Generation : null;
                if (prior is not null && ReferenceEquals(previous!.BaseCompilation, baseCompilation) &&
                    ReferenceEquals(previous.ExtensionInputs, extensionInputs) && prior.ParseOptions.Equals(parseOptions) &&
                    PathComparer.Equals(prior.OutputRoot, outputRoot))
                {
                    generation = prior;
                    compilation = previous.Compilation;
                }
                else
                {
                    var driver = prior?.Driver ?? CSharpGeneratorDriver.Create(pluginState.Generators,
                        analyzerOptions!.AdditionalFiles, parseOptions, extensionInputs!.Settings);
                    if (prior is not null)
                    {
                        var previousInputs = previous!.ExtensionInputs!;
                        if (!ReferenceEquals(previousInputs.Settings, extensionInputs!.Settings))
                            driver = driver.WithUpdatedAnalyzerConfigOptions(extensionInputs.Settings);
                        if (!previousInputs.AdditionalFiles.SequenceEqual(extensionInputs.AdditionalFiles))
                            driver = driver.ReplaceAdditionalTexts(analyzerOptions!.AdditionalFiles);
                        if (!prior.ParseOptions.Equals(parseOptions))
                            driver = driver.WithUpdatedParseOptions(parseOptions);
                    }
                    driver = driver.RunGeneratorsAndUpdateCompilation(baseCompilation, out _, out var generatorDiagnostics);
                    var outputs = new Dictionary<string, (SyntaxTree Original, SyntaxTree Normalized)>(PathComparer);
                    foreach (var tree in driver.GetRunResult().GeneratedTrees)
                    {
                        var path = Path.GetFullPath(tree.FilePath, outputRoot);
                        var normalized = prior?.Trees.GetValueOrDefault(path) is { } old && ReferenceEquals(tree, old.Original)
                            ? old.Normalized : tree.WithFilePath(path);
                        outputs.Add(path, (tree, normalized));
                    }
                    generation = new GeneratorState(driver, parseOptions, outputRoot, generatorDiagnostics, outputs);
                    // Generator output owns its path even if MSBuild also enumerated a saved copy.
                    compilation = baseCompilation.RemoveSyntaxTrees(outputs.Keys.Where(trees.ContainsKey).Select(path => trees[path]))
                        .AddSyntaxTrees(outputs.Values.Select(output => output.Normalized));
                }
                diagnostics.AddRange(generation.Diagnostics);
                foreach (var (path, tree) in generation.Trees)
                {
                    trees[path] = tree.Normalized;
                    generated[path] = "";
                }
            }
            var analyzerRun = analyzers.Length == 0 ? null :
                ReferenceEquals(previous?.Compilation, compilation) && ReferenceEquals(previous.ExtensionInputs, extensionInputs) &&
                ReferenceEquals(previous.Plugins, pluginState) && previous.AnalyzerRun is not null
                    ? previous.AnalyzerRun : compilation.WithAnalyzers(analyzers, analyzerOptions);
            result = new CompilationUnit(id, compilation, trees, generated, inputs.ToArray(), dependencies, diagnostics.ToImmutable())
            {
                BaseCompilation = baseCompilation, Plugins = pluginState, ExtensionInputs = extensionInputs, Generation = generation,
                AnalyzerRun = analyzerRun
            };
            // Carry the compilation reference cache across unchanged epochs.
            if (previous is not null && ReferenceEquals(compilation, previous.Compilation))
                result.ReuseReferences(previous);
            built.Add(id, result);
            visiting.Remove(id);
            return result;
        }

        try
        {
            foreach (var id in csharp.Keys.Order(StringComparer.Ordinal))
                Build(id);

            foreach (var script in scripts)
            {
                var owner = csharp.Values.Select(unit => (Unit: unit, Directory: FullPath(unit.String("directory") is { Length: > 0 } dir ? dir : ".")))
                    .Where(entry => Within(entry.Directory, script)).OrderByDescending(entry => entry.Directory.Length)
                    .ThenBy(entry => entry.Unit.String("id"), StringComparer.Ordinal).FirstOrDefault();
                if (owner.Unit is null)
                    continue;
                var parent = built[owner.Unit.String("id")];
                var scriptId = "script:" + RelativePath(script);
                if (built.ContainsKey(scriptId))
                    throw new InvalidOperationException($"C# script unit conflicts with a project unit: {scriptId}");
                var content = nextDocuments.GetValueOrDefault(script) ?? File.ReadAllText(script);
                var options = parent.BaseCompilation.SyntaxTrees.FirstOrDefault()?.Options as CSharpParseOptions ?? CSharpParseOptions.Default;
                var tree = CSharpSyntaxTree.ParseText(content, options.WithKind(SourceCodeKind.Script), script);
                var folder = Path.GetDirectoryName(script)!;
                var scriptReferences = parent.Compilation.References.Append(parent.Reference(MetadataReferenceProperties.Assembly)).ToArray();
                var scriptOptions = parent.Compilation.Options.WithOutputKind(OutputKind.DynamicallyLinkedLibrary)
                    .WithSourceReferenceResolver(new SourceFileResolver([], folder))
                    .WithMetadataReferenceResolver(new ScriptReferences(folder, scriptReferences.OfType<PortableExecutableReference>()));
                var compilation = CSharpCompilation.CreateScriptCompilation("script", tree, scriptReferences, scriptOptions);
                var scriptTrees = compilation.SyntaxTrees.Where(item => IsInTree(item.FilePath)).ToDictionary(item => item.FilePath, PathComparer);
                var dependencies = new Dictionary<string, CompilationUnit> { [parent.Id] = parent };
                built.Add(scriptId, new CompilationUnit(scriptId, compilation, scriptTrees, new(PathComparer),
                    scriptTrees.Keys.ToArray(), dependencies, []));
            }

        }
        catch
        {
            foreach (var loader in newLoaders)
                loader.Unload();
            throw;
        }

        foreach (var key in metadata.Keys.Where(key => !usedMetadata.Contains(key)).ToArray())
            metadata.Remove(key);
        documents = nextDocuments;
        sourceObjects = nextSourceObjects;
        ModelPath = models[0];
        foreach (var unit in Units.Values)
            if (unit.Loader is { } loader && !ReferenceEquals(loader, built.GetValueOrDefault(unit.Id)?.Loader))
                loader.Unload();
        Units = built;
    }

    private static bool Within(string directory, string path)
    {
        var relative = Path.GetRelativePath(directory, path);
        return relative != ".." && !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) &&
            !Path.IsPathRooted(relative);
    }

    private string RelativeOrAbsolute(string path) => IsInTree(path) ? RelativePath(path) : path;
    private bool IsInTree(string path)
    {
        var relative = RelativePath(path);
        return relative != ".." && !relative.StartsWith("../", StringComparison.Ordinal) && !Path.IsPathRooted(relative);
    }

    public void Dispose()
    {
        foreach (var unit in Units.Values)
            unit.Loader?.Unload();
        Units.Clear();
        metadata.Clear();
        if (staging is not null)
            staging.Delete(recursive: true);
    }
}
