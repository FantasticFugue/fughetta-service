using System.Collections.Immutable;
using System.Reflection;
using System.Runtime.Loader;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;

namespace Fughetta.CSharp;

internal sealed class AnalyzerLoader() : AssemblyLoadContext(isCollectible: true), IAnalyzerAssemblyLoader
{
    private readonly Dictionary<string, List<string>> dependencies = new(StringComparer.OrdinalIgnoreCase);

    public void AddDependencyLocation(string fullPath)
    {
        var name = Path.GetFileNameWithoutExtension(fullPath);
        if (!dependencies.TryGetValue(name, out var paths))
            dependencies.Add(name, paths = []);
        if (!paths.Contains(fullPath, Workspace.PathComparer))
            paths.Add(fullPath);
    }

    public Assembly LoadFromPath(string fullPath)
    {
        var name = AssemblyName.GetAssemblyName(fullPath);
        var loaded = Assemblies.FirstOrDefault(assembly => AssemblyName.ReferenceMatchesDefinition(assembly.GetName(), name));
        if (loaded is not null)
            return loaded;
        using var bytes = File.OpenRead(fullPath);
        return LoadFromStream(bytes);
    }

    protected override Assembly? Load(AssemblyName name)
    {
        if (name.Name is "Microsoft.CodeAnalysis" or "Microsoft.CodeAnalysis.CSharp")
            return name.Name == "Microsoft.CodeAnalysis" ? typeof(Compilation).Assembly : typeof(Microsoft.CodeAnalysis.CSharp.CSharpCompilation).Assembly;
        return name.Name is { } simple && dependencies.TryGetValue(simple, out var paths)
            ? LoadFromPath(paths[0]) : null;
    }
}

internal sealed class AnalyzerInput(string path, SourceText? text) : AdditionalText
{
    public override string Path { get; } = path;
    public override SourceText? GetText(CancellationToken cancellationToken = default) => text;
    public bool SameContent(AnalyzerInput other) => Workspace.PathComparer.Equals(Path, other.Path) &&
        (text is null ? other.GetText() is null : other.GetText() is { } content && text.ContentEquals(content));
}

internal sealed class AnalyzerSettings(AnalyzerConfigSet configs) : AnalyzerConfigOptionsProvider
{
    private sealed class Options(ImmutableDictionary<string, string> values) : AnalyzerConfigOptions
    {
        public override bool TryGetValue(string key, out string value) => values.TryGetValue(key, out value!);
    }

    public override AnalyzerConfigOptions GlobalOptions { get; } = new Options(configs.GlobalConfigOptions.AnalyzerOptions);
    public override AnalyzerConfigOptions GetOptions(SyntaxTree tree) => ForPath(tree.FilePath);
    public override AnalyzerConfigOptions GetOptions(AdditionalText text) => ForPath(text.Path);
    private AnalyzerConfigOptions ForPath(string path) => new Options(configs.GetOptionsForSourcePath(path).AnalyzerOptions);
}

internal sealed class DiagnosticSettings(AnalyzerConfigSet configs) : SyntaxTreeOptionsProvider
{
    public override GeneratedKind IsGenerated(SyntaxTree tree, CancellationToken cancellationToken) =>
        configs.GetOptionsForSourcePath(tree.FilePath).AnalyzerOptions.TryGetValue("generated_code", out var value)
            ? value.Equals("true", StringComparison.OrdinalIgnoreCase) ? GeneratedKind.MarkedGenerated : GeneratedKind.NotGenerated
            : GeneratedKind.Unknown;

    public override bool TryGetDiagnosticValue(SyntaxTree tree, string diagnosticId, CancellationToken cancellationToken, out ReportDiagnostic severity) =>
        configs.GetOptionsForSourcePath(tree.FilePath).TreeOptions.TryGetValue(diagnosticId, out severity);

    public override bool TryGetGlobalDiagnosticValue(string diagnosticId, CancellationToken cancellationToken, out ReportDiagnostic severity) =>
        configs.GlobalConfigOptions.TreeOptions.TryGetValue(diagnosticId, out severity);
}

internal sealed record PluginInputs(
    AnalyzerLoader Loader, (string Path, long Length, DateTime LastWrite, string? Version)[] Files,
    string[] Entrypoints, ImmutableArray<DiagnosticAnalyzer> Analyzers, ImmutableArray<ISourceGenerator> Generators,
    HashSet<string> GeneratorAssemblies, ImmutableArray<Diagnostic> Diagnostics);

internal sealed record AnalyzerInputs(
    (string Path, string? Content)[] ConfigFiles, ImmutableArray<AnalyzerInput> AdditionalFiles,
    AnalyzerSettings Settings, DiagnosticSettings DiagnosticSettings, ImmutableArray<Diagnostic> Diagnostics);

internal sealed record GeneratorState(
    GeneratorDriver Driver, CSharpParseOptions ParseOptions, string OutputRoot, ImmutableArray<Diagnostic> Diagnostics,
    Dictionary<string, (SyntaxTree Original, SyntaxTree Normalized)> Trees);

internal sealed class ScriptReferences(string directory, IEnumerable<PortableExecutableReference> references) : MetadataReferenceResolver
{
    private readonly Dictionary<string, PortableExecutableReference> assemblies = references
        .Where(reference => reference.FilePath is not null)
        .GroupBy(reference => Path.GetFileName(reference.FilePath!), Workspace.PathComparer)
        .ToDictionary(group => group.Key, group => group.First(), Workspace.PathComparer);

    public override ImmutableArray<PortableExecutableReference> ResolveReference(
        string reference, string? baseFilePath, MetadataReferenceProperties properties)
    {
        var package = reference.StartsWith("nuget:", StringComparison.OrdinalIgnoreCase);
        if (package)
        {
            var specification = reference[6..].Split(',', 2, StringSplitOptions.TrimEntries);
            if (specification.Length != 1)
                return [];
            reference = specification[0];
        }
        if (!reference.Contains('/') && !reference.Contains('\\') &&
            (assemblies.TryGetValue(reference, out var assembly) || assemblies.TryGetValue(reference + ".dll", out assembly)))
            return [assembly.WithProperties(properties)];
        if (package)
            return [];
        var baseDirectory = baseFilePath is null ? directory : Path.GetDirectoryName(baseFilePath) ?? directory;
        var path = Path.GetFullPath(reference, baseDirectory);
        return File.Exists(path) ? [MetadataReference.CreateFromFile(path, properties)] : [];
    }

    public override bool Equals(object? other) => ReferenceEquals(this, other);
    public override int GetHashCode() => System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(this);
}
