using System.ComponentModel;
using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using ModelContextProtocol.Server;

var builder = Host.CreateApplicationBuilder(args);
builder.Services
    .AddMcpServer()
    .WithStdioServerTransport()
    .WithToolsFromAssembly();

await builder.Build().RunAsync();

[McpServerToolType]
public static class WorkspaceTools
{
    private static readonly string WorkspaceRoot = Path.GetFullPath(
        Environment.GetEnvironmentVariable("IDE_MCP_WORKSPACE") ?? Directory.GetCurrentDirectory());

    [McpServerTool, Description("Returns the Git status for the complete workspace repository.")]
    public static Task<string> GetGitStatus() => RunAsync("git", ["status", "--short", "--branch"], WorkspaceRoot);

    [McpServerTool, Description("Lists files matching a search pattern anywhere in the workspace. The pattern is interpreted as a case-insensitive substring of a relative path.")]
    public static string FindFiles([Description("A non-empty substring to find in workspace-relative file paths.")] string pattern)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(pattern);

        return string.Join('\n', Directory.EnumerateFiles(WorkspaceRoot, "*", SearchOption.AllDirectories)
            .Where(path => !path.Contains(".git", StringComparison.OrdinalIgnoreCase))
            .Select(path => Path.GetRelativePath(WorkspaceRoot, path))
            .Where(path => path.Contains(pattern, StringComparison.OrdinalIgnoreCase))
            .Take(200));
    }

    [McpServerTool, Description("Lists configure, build, and workflow CMake presets for any CMake project directory below the workspace root.")]
    public static Task<string> ListCMakePresets([Description("Workspace-relative directory containing CMakePresets.json.")] string projectDirectory)
    {
        var directory = ResolveProjectDirectory(projectDirectory);
        return RunAsync("cmake", ["--list-presets", "--all"], directory);
    }

    [McpServerTool, Description("Configures a CMake project using a named configure preset. The project directory must be below the workspace root.")]
    public static Task<string> ConfigureCMake(
        [Description("Workspace-relative directory containing CMakePresets.json.")] string projectDirectory,
        [Description("Named configure preset to use.")] string preset)
    {
        var directory = ResolveProjectDirectory(projectDirectory);
        return RunAsync("cmake", ["--preset", ValidatePresetName(preset)], directory);
    }

    [McpServerTool, Description("Builds a CMake project using a named build preset. The project directory must be below the workspace root.")]
    public static Task<string> BuildCMake(
        [Description("Workspace-relative directory containing CMakePresets.json.")] string projectDirectory,
        [Description("Named build preset to use.")] string preset)
    {
        var directory = ResolveProjectDirectory(projectDirectory);
        return RunAsync("cmake", ["--build", "--preset", ValidatePresetName(preset)], directory);
    }

    private static string ResolveProjectDirectory(string projectDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(projectDirectory);
        var directory = Path.GetFullPath(Path.Combine(WorkspaceRoot, projectDirectory));

        if (!directory.StartsWith(WorkspaceRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(directory, WorkspaceRoot, StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException("The project directory must be inside the workspace.", nameof(projectDirectory));
        }

        if (!File.Exists(Path.Combine(directory, "CMakePresets.json")))
        {
            throw new DirectoryNotFoundException("No CMakePresets.json was found in the specified project directory.");
        }

        return directory;
    }

    private static string ValidatePresetName(string preset)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(preset);

        if (preset.Any(character => !(char.IsLetterOrDigit(character) || character is '-' or '_' or ' ')))
        {
            throw new ArgumentException("The preset name contains unsupported characters.", nameof(preset));
        }

        return preset;
    }

    private static async Task<string> RunAsync(string fileName, IReadOnlyList<string> arguments, string workingDirectory)
    {
        var startInfo = new ProcessStartInfo(fileName)
        {
            WorkingDirectory = workingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };

        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = Process.Start(startInfo) ?? throw new InvalidOperationException($"Unable to start {fileName}.");
        var standardOutput = process.StandardOutput.ReadToEndAsync();
        var standardError = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();

        return $"Exit code: {process.ExitCode}\n{await standardOutput}{await standardError}";
    }
}
