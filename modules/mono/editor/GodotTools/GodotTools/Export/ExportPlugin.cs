using Godot;
using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using GodotTools.Build;
using GodotTools.Internals;
using Directory = GodotTools.Utils.Directory;
using File = GodotTools.Utils.File;
using OS = GodotTools.Utils.OS;
using Path = System.IO.Path;
using System.Globalization;

namespace GodotTools.Export
{
    public partial class ExportPlugin : EditorExportPlugin
    {
        public override string _GetName() => "C#";

        private List<string> _tempFolders = new List<string>();

        // The publish copies the runtime pack's native directory wholesale. None of it is
        // reachable from a Web export, since the template has the runtime linked in, so it
        // is dropped from the exported payload.
        //
        // Only names no application would plausibly own. Dropping a file the project meant
        // to publish costs correctness -- managed code cannot find it under
        // AppContext.BaseDirectory -- while keeping one only costs a few bytes, so generic
        // names the runtime pack also uses, runtime.c and emcc-link.rsp among them, are
        // deliberately left in. Its include/ and src/ directories are kept for the same
        // reason, though they are the pack's too and worth about half a megabyte.
        //
        // The one extension excluded at the publish root. Nearly all of the weight is here:
        // twenty-six static archives at 25.6 MiB, against 0.8 MiB for everything else the
        // pack leaves behind, and a static library is not something a game publishes as
        // content. Anything cheaper than that is matched by name below instead of by
        // extension, since .map and .ts in particular are as likely to be a project's level
        // data or a video stream as they are the pack's.
        private static readonly HashSet<string> _webRuntimePackDeadExtensions = new(StringComparer.OrdinalIgnoreCase)
        {
            ".a",
        };

        private static readonly HashSet<string> _webRuntimePackLeftovers = new(StringComparer.Ordinal)
        {
            // The .NET JavaScript host and its wasm module. The engine is the host here,
            // so none of it is loaded, but the publish copies it regardless.
            "dotnet.js",
            "dotnet.runtime.js",
            "dotnet.native.js",
            "dotnet.native.wasm",
            "dotnet.globalization.js",

            // That host's debug and authoring output, which nothing loads either.
            "dotnet.js.map",
            "dotnet.runtime.js.map",
            "dotnet.native.js.symbols",
            "dotnet.d.ts",
        };

        private static bool ProjectContainsDotNet()
        {
            return File.Exists(GodotSharpDirs.ProjectSlnPath);
        }

        public override string[] _GetExportFeatures(EditorExportPlatform platform, bool debug)
        {
            if (!ProjectContainsDotNet())
                return Array.Empty<string>();

            return new string[] { "dotnet" };
        }

        public override Godot.Collections.Array<Godot.Collections.Dictionary> _GetExportOptions(EditorExportPlatform platform)
        {
            var exportOptionList = new Godot.Collections.Array<Godot.Collections.Dictionary>();

            if (platform.GetOsName().Equals(OS.Platforms.Android, StringComparison.OrdinalIgnoreCase))
            {
                exportOptionList.Add
                (
                    new Godot.Collections.Dictionary()
                    {
                        {
                            "option", new Godot.Collections.Dictionary()
                            {
                                { "name", "dotnet/android_use_linux_bionic" },
                                { "type", (int)Variant.Type.Bool }
                            }
                        },
                        { "default_value", false }
                    }
                );
            }

            exportOptionList.Add
            (
                new Godot.Collections.Dictionary()
                {
                    {
                        "option", new Godot.Collections.Dictionary()
                        {
                            { "name", "dotnet/include_scripts_content" },
                            { "type", (int)Variant.Type.Bool }
                        }
                    },
                    { "default_value", false }
                }
            );
            exportOptionList.Add
            (
                new Godot.Collections.Dictionary()
                {
                    {
                        "option", new Godot.Collections.Dictionary()
                        {
                            { "name", "dotnet/include_debug_symbols" },
                            { "type", (int)Variant.Type.Bool }
                        }
                    },
                    { "default_value", true }
                }
            );
            exportOptionList.Add
            (
                new Godot.Collections.Dictionary()
                {
                    {
                        "option", new Godot.Collections.Dictionary()
                        {
                            { "name", "dotnet/embed_build_outputs" },
                            { "type", (int)Variant.Type.Bool }
                        }
                    },
                    { "default_value", false }
                }
            );
            return exportOptionList;
        }

        private void AddExceptionMessage(EditorExportPlatform platform, Exception exception)
        {
            string? exceptionMessage = exception.Message;
            if (string.IsNullOrEmpty(exceptionMessage))
            {
                exceptionMessage = $"Exception thrown: {exception.GetType().Name}";
            }

            platform.AddMessage(EditorExportPlatform.ExportMessageType.Error, "Export .NET Project", exceptionMessage);

            // We also print exceptions as we receive them to stderr.
            Console.Error.WriteLine(exception);
        }

        // With this method we can override how a file is exported in the PCK
        public override void _ExportFile(string path, string type, string[] features)
        {
            base._ExportFile(path, type, features);

            if (type != Internal.CSharpLanguageType)
                return;

            if (Path.GetExtension(path) != Internal.CSharpLanguageExtension)
                throw new ArgumentException(
                    $"Resource of type {Internal.CSharpLanguageType} has an invalid file extension: {path}",
                    nameof(path));

            if (!ProjectContainsDotNet())
            {
                GetExportPlatform().AddMessage(EditorExportPlatform.ExportMessageType.Error, "Export .NET Project", $"This project contains C# files but no solution file was found at the following path: {GodotSharpDirs.ProjectSlnPath}\n" +
                    "A solution file is required for projects with C# files. Please ensure that the solution file exists in the specified location and try again.");
                throw new InvalidOperationException($"{path} is a C# file but no solution file exists.");
            }

            // TODO: What if the source file is not part of the game's C# project?

            bool includeScriptsContent = (bool)GetOption("dotnet/include_scripts_content");

            if (!includeScriptsContent)
            {
                // We don't want to include the source code on exported games.

                // Sadly, Godot prints errors when adding an empty file (nothing goes wrong, it's just noise).
                // Because of this, we add a file which contains a line break.
                AddFile(path, System.Text.Encoding.UTF8.GetBytes("\n"), remap: false);

                // Tell the Godot exporter that we already took care of the file.
                Skip();
            }
        }

        public override void _ExportBegin(string[] features, bool isDebug, string path, uint flags)
        {
            base._ExportBegin(features, isDebug, path, flags);

            try
            {
                _ExportBeginImpl(features, isDebug, path, flags);
            }
            catch (Exception e)
            {
                AddExceptionMessage(GetExportPlatform(), e);
            }
        }

        private void _ExportBeginImpl(string[] features, bool isDebug, string path, long flags)
        {
            _ = flags; // Unused.

            if (!ProjectContainsDotNet())
                return;

            string osName = GetExportPlatform().GetOsName();

            if (!TryDeterminePlatformFromOSName(osName, out string? platform))
                throw new NotSupportedException("Target platform not supported.");

            if (!new[] { OS.Platforms.Windows, OS.Platforms.LinuxBSD, OS.Platforms.MacOS, OS.Platforms.Android, OS.Platforms.iOS, OS.Platforms.Web }
                    .Contains(platform))
            {
                throw new NotImplementedException("Target platform not yet implemented.");
            }

            bool useAndroidLinuxBionic = (bool)GetOption("dotnet/android_use_linux_bionic");
            PublishConfig publishConfig = new()
            {
                BuildConfig = isDebug ? "ExportDebug" : "ExportRelease",
                IncludeDebugSymbols = (bool)GetOption("dotnet/include_debug_symbols"),
                RidOS = DetermineRuntimeIdentifierOS(platform, useAndroidLinuxBionic),
                Archs = [],
                UseTempDir = platform != OS.Platforms.iOS, // xcode project links directly to files in the publish dir, so use one that sticks around.
                BundleOutputs = true,
            };

            if (features.Contains("x86_64"))
            {
                publishConfig.Archs.Add("x86_64");
            }

            if (features.Contains("x86_32"))
            {
                publishConfig.Archs.Add("x86_32");
            }

            if (features.Contains("arm64"))
            {
                publishConfig.Archs.Add("arm64");
            }

            if (features.Contains("arm32"))
            {
                publishConfig.Archs.Add("arm32");
            }

            if (features.Contains("wasm32"))
            {
                publishConfig.Archs.Add("wasm32");
            }

            if (features.Contains("universal"))
            {
                if (platform == OS.Platforms.MacOS)
                {
                    publishConfig.Archs.Add("x86_64");
                    publishConfig.Archs.Add("arm64");
                }
            }

            var targets = new List<PublishConfig> { publishConfig };

            if (platform == OS.Platforms.iOS)
            {
                targets.Add(new PublishConfig
                {
                    BuildConfig = publishConfig.BuildConfig,
                    Archs = ["arm64", "x86_64"],
                    BundleOutputs = false,
                    IncludeDebugSymbols = publishConfig.IncludeDebugSymbols,
                    RidOS = OS.DotNetOS.iOSSimulator,
                    UseTempDir = false,
                });
            }

            List<string> outputPaths = new();

            // Web is like Android: the engine can only read the pck, so the managed payload has
            // to be inside it rather than sitting next to the exported page.
            bool embedBuildResults = ((bool)GetOption("dotnet/embed_build_outputs")
                || platform == OS.Platforms.Android
                || platform == OS.Platforms.Web) && platform != OS.Platforms.MacOS;

            var exportedJars = new HashSet<string>();

            foreach (PublishConfig config in targets)
            {
                string ridOS = config.RidOS;
                string buildConfig = config.BuildConfig;
                bool includeDebugSymbols = config.IncludeDebugSymbols;

                foreach (string arch in config.Archs)
                {
                    string ridArch = DetermineRuntimeIdentifierArch(arch);
                    string runtimeIdentifier = $"{ridOS}-{ridArch}";
                    string projectDataDirName = $"data_{GodotSharpDirs.CSharpProjectName}_{platform}_{arch}";
                    if (platform == OS.Platforms.MacOS)
                    {
                        projectDataDirName = Path.Combine("Contents", "Resources", projectDataDirName);
                    }

                    // Create temporary publish output directory.
                    string publishOutputDir;

                    if (config.UseTempDir)
                    {
                        publishOutputDir = Path.Combine(Path.GetTempPath(), "godot-publish-dotnet",
                            $"{System.Environment.ProcessId}-{buildConfig}-{runtimeIdentifier}");
                        _tempFolders.Add(publishOutputDir);
                    }
                    else
                    {
                        publishOutputDir = Path.Combine(GodotSharpDirs.ProjectBaseOutputPath, "godot-publish-dotnet",
                            $"{buildConfig}-{runtimeIdentifier}");
                    }

                    outputPaths.Add(publishOutputDir);

                    if (!Directory.Exists(publishOutputDir))
                        Directory.CreateDirectory(publishOutputDir);

                    // Execute dotnet publish.
                    if (!BuildManager.PublishProjectBlocking(buildConfig, platform,
                            runtimeIdentifier, publishOutputDir, includeDebugSymbols))
                    {
                        throw new InvalidOperationException("Failed to build project. Check MSBuild panel for details.");
                    }

                    string soExt = ridOS switch
                    {
                        OS.DotNetOS.Win or OS.DotNetOS.Win10 => "dll",
                        OS.DotNetOS.OSX or OS.DotNetOS.iOS or OS.DotNetOS.iOSSimulator => "dylib",
                        _ => "so"
                    };

                    string assemblyPath = Path.Combine(publishOutputDir, $"{GodotSharpDirs.ProjectAssemblyName}.dll");
                    string nativeAotPath = Path.Combine(publishOutputDir,
                        $"{GodotSharpDirs.ProjectAssemblyName}.{soExt}");

                    if (!File.Exists(assemblyPath) && !File.Exists(nativeAotPath))
                    {
                        throw new NotSupportedException(
                            $"Publish succeeded but project assembly not found at '{assemblyPath}' or '{nativeAotPath}'.");
                    }

                    // For ios simulator builds, skip packaging the build outputs.
                    if (!config.BundleOutputs)
                        continue;

                    if (platform == OS.Platforms.Web)
                    {
                        // Every other platform enters managed code through GodotPlugins.Game.Main,
                        // which a source generator emits into the game's own assembly, so
                        // GodotPlugins itself is never shipped. WebAssembly needs a trampoline
                        // generated ahead of time from the assembly holding the entry point, and
                        // the game's assembly does not exist when the template is built, so the
                        // Web template enters through GodotPlugins.Main instead. That means the
                        // editor's own GodotPlugins.dll has to travel with the project.
                        // Sibling of the editor's Tools directory; which build configuration the
                        // editor shipped its API assemblies in is not worth asserting, so take
                        // whichever is there.
                        string apiBaseDir = Path.Combine(
                            Path.GetDirectoryName(GodotSharpDirs.DataEditorToolsDir.TrimEnd('/', '\\'))!, "Api");
                        string? godotPluginsSource = new[] { "Debug", "Release" }
                            .Select(buildConfigDir => Path.Combine(apiBaseDir, buildConfigDir, "GodotPlugins.dll"))
                            .FirstOrDefault(File.Exists);

                        if (godotPluginsSource is null)
                        {
                            throw new NotSupportedException(
                                $"Web export needs GodotPlugins.dll, which was not found under '{apiBaseDir}'.");
                        }
                        // The publish may already have put something there. Copying over it
                        // would replace the project's own assembly, or a dependency, with the
                        // editor's, and the export would look fine until it ran.
                        string godotPluginsTarget = Path.Combine(publishOutputDir, "GodotPlugins.dll");
                        if (File.Exists(godotPluginsTarget))
                        {
                            throw new NotSupportedException(
                                "Web export needs to add its own GodotPlugins.dll, but the published "
                                + $"project already contains one at '{godotPluginsTarget}'. Rename the "
                                + "assembly or dependency that produces it.");
                        }
                        File.Copy(godotPluginsSource, godotPluginsTarget);
                    }

                    var manifest = new StringBuilder();

                    // Add to the exported project shared object list or packed resources.
                    RecursePublishContents(publishOutputDir,
                        filterDir: dir =>
                        {
                            if (platform == OS.Platforms.iOS)
                            {
                                // Exclude dsym folders.
                                return !dir.EndsWith(".dsym", StringComparison.OrdinalIgnoreCase);
                            }

                            return true;
                        },
                        filterFile: file =>
                        {
                            if (platform == OS.Platforms.iOS)
                            {
                                // Exclude the dylib artifact, since it's included separately as an xcframework.
                                return Path.GetFileName(file) != $"{GodotSharpDirs.ProjectAssemblyName}.dylib";
                            }

                            if (platform == OS.Platforms.Web)
                            {
                                // The publish drags along the runtime pack's own host: dotnet.native.wasm
                                // and the JavaScript that loads it. The export template has the runtime
                                // linked in and never uses either, and dotnet.native.wasm alone is
                                // several megabytes of dead payload.
                                //
                                // Matched on extension as well as prefix, so that a managed assembly
                                // whose name happens to start with "dotnet." is not dropped with them.
                                //
                                // Only at the publish root, where the runtime pack drops them. A
                                // project is free to publish content of its own called
                                // assets/runtime.c, and dropping that would leave managed code
                                // unable to find it under AppContext.BaseDirectory.
                                if (!string.Equals(Path.GetDirectoryName(file), publishOutputDir,
                                        StringComparison.Ordinal))
                                {
                                    return true;
                                }

                                // A browser cannot load a static library, and no game reads one
                                // at run time, so these are unambiguously the runtime pack's:
                                // 26 MB of libmonosgen, libicu and friends that the template
                                // already has linked in, downloaded by every player for nothing.
                                // The rest are the pack's build and debug leavings.
                                if (_webRuntimePackDeadExtensions.Contains(Path.GetExtension(file)))
                                {
                                    return false;
                                }

                                // Named exactly, never matched by prefix: a project is free to
                                // publish content of its own called dotnet.settings.js, and
                                // dropping that would leave managed code unable to find it.
                                return !_webRuntimePackLeftovers.Contains(Path.GetFileName(file));
                            }

                            return true;
                        },
                        recurseDir: dir =>
                        {
                            if (platform == OS.Platforms.iOS)
                            {
                                // Don't recurse into dsym folders.
                                return !dir.EndsWith(".dsym", StringComparison.OrdinalIgnoreCase);
                            }

                            return true;
                        },
                        addEntry: (path, isFile) =>
                        {
                            // We get called back for both directories and files, but we only package files for now.
                            if (isFile)
                            {
                                if (embedBuildResults)
                                {
                                    if (platform == OS.Platforms.Android)
                                    {
                                        string fileName = Path.GetFileName(path);

                                        if (IsSharedObject(fileName))
                                        {
                                            if (fileName.EndsWith(".so") && !fileName.StartsWith("lib"))
                                            {
                                                // Add 'lib' prefix required for all native libraries in Android.
                                                string newPath = string.Concat(path.AsSpan(0, path.Length - fileName.Length), "lib", fileName);
                                                Godot.DirAccess.RenameAbsolute(path, newPath);
                                                path = newPath;
                                            }

                                            AddSharedObject(path, tags: new string[] { arch },
                                                Path.Join(projectDataDirName,
                                                    Path.GetRelativePath(publishOutputDir,
                                                        Path.GetDirectoryName(path)!)));

                                            return;
                                        }

                                        bool IsSharedObject(string fileName)
                                        {
                                            if (fileName.EndsWith(".jar"))
                                            {
                                                // Don't export the same jar twice. Otherwise we will have conflicts.
                                                // This can happen when exporting for multiple architectures. Dotnet
                                                // stores the jars in .godot/mono/temp/bin/Export[Debug|Release] per
                                                // target architecture. Jars are cpu agnostic so only 1 is needed.
                                                var jarName = Path.GetFileName(fileName);
                                                return exportedJars.Add(jarName);
                                            }

                                            if (fileName.EndsWith(".so") || fileName.EndsWith(".a") || fileName.EndsWith(".dex"))
                                            {
                                                return true;
                                            }

                                            return false;
                                        }
                                    }

                                    string filePath = SanitizeSlashes(Path.GetRelativePath(publishOutputDir, path));
                                    byte[] fileData = File.ReadAllBytes(path);
                                    string hash = Convert.ToBase64String(SHA512.HashData(fileData));

                                    manifest.Append(CultureInfo.InvariantCulture, $"{filePath}\t{hash}\n");

                                    AddFile($"res://.godot/mono/publish/{arch}/{filePath}", fileData, false);
                                }
                                else
                                {
                                    if (platform == OS.Platforms.iOS && path.EndsWith(".dat", StringComparison.OrdinalIgnoreCase))
                                    {
                                        AddAppleEmbeddedPlatformBundleFile(path);
                                    }
                                    else
                                    {
                                        AddSharedObject(path, tags: null,
                                            Path.Join(projectDataDirName,
                                                Path.GetRelativePath(publishOutputDir,
                                                    Path.GetDirectoryName(path)!)));
                                    }
                                }
                            }
                        });

                    if (embedBuildResults)
                    {
                        byte[] fileData = Encoding.Default.GetBytes(manifest.ToString());
                        AddFile($"res://.godot/mono/publish/{arch}/.dotnet-publish-manifest", fileData, false);
                    }
                }
            }

            if (platform == OS.Platforms.iOS)
            {
                if (outputPaths.Count > 2)
                {
                    // lipo the simulator binaries together

                    string outputPath = Path.Combine(outputPaths[1], $"{GodotSharpDirs.ProjectAssemblyName}.dylib");
                    string[] files = outputPaths
                        .Skip(1)
                        .Select(path => Path.Combine(path, $"{GodotSharpDirs.ProjectAssemblyName}.dylib"))
                        .ToArray();

                    if (!Internal.LipOCreateFile(outputPath, files))
                    {
                        throw new InvalidOperationException($"Failed to 'lipo' simulator binaries.");
                    }

                    outputPaths.RemoveRange(2, outputPaths.Count - 2);
                }

                string xcFrameworkPath = Path.Combine(GodotSharpDirs.ProjectBaseOutputPath, publishConfig.BuildConfig, $"{GodotSharpDirs.ProjectAssemblyName}_aot.xcframework");
                if (!BuildManager.GenerateXCFrameworkBlocking(outputPaths, xcFrameworkPath))
                {
                    throw new InvalidOperationException("Failed to generate xcframework.");
                }

                AddAppleEmbeddedPlatformEmbeddedFramework(xcFrameworkPath);
            }
        }

        private static void RecursePublishContents(string path, Func<string, bool> filterDir,
            Func<string, bool> filterFile, Func<string, bool> recurseDir,
            Action<string, bool> addEntry)
        {
            foreach (string file in Directory.GetFiles(path, "*", SearchOption.TopDirectoryOnly))
            {
                if (filterFile(file))
                {
                    addEntry(file, true);
                }
            }

            foreach (string dir in Directory.GetDirectories(path, "*", SearchOption.TopDirectoryOnly))
            {
                if (filterDir(dir))
                {
                    addEntry(dir, false);
                    if (recurseDir(dir))
                    {
                        RecursePublishContents(dir, filterDir, filterFile, recurseDir, addEntry);
                    }
                }
            }
        }

        private string SanitizeSlashes(string path)
        {
            if (Path.DirectorySeparatorChar == '\\')
                return path.Replace('\\', '/');
            return path;
        }

        private string DetermineRuntimeIdentifierOS(string platform, bool useAndroidLinuxBionic)
        {
            if (platform == OS.Platforms.Android && useAndroidLinuxBionic)
            {
                return OS.DotNetOS.LinuxBionic;
            }
            return OS.DotNetOSPlatformMap[platform];
        }

        private string DetermineRuntimeIdentifierArch(string arch)
        {
            return arch switch
            {
                "x86" => "x86",
                "x86_32" => "x86",
                "x64" => "x64",
                "x86_64" => "x64",
                "armeabi-v7a" => "arm",
                "arm64-v8a" => "arm64",
                "arm32" => "arm",
                "arm64" => "arm64",
                // The .NET `browser-wasm` runtime pack is wasm32-only, and the RID arch is
                // spelled "wasm" rather than "wasm32".
                "wasm32" => "wasm",
                _ => throw new ArgumentOutOfRangeException(nameof(arch), arch, "Unexpected architecture")
            };
        }

        public override void _ExportEnd()
        {
            base._ExportEnd();

            string aotTempDir = Path.Combine(Path.GetTempPath(), $"godot-aot-{System.Environment.ProcessId}");

            if (Directory.Exists(aotTempDir))
                Directory.Delete(aotTempDir, recursive: true);

            foreach (string folder in _tempFolders)
            {
                Directory.Delete(folder, recursive: true);
            }
            _tempFolders.Clear();
        }

        /// <summary>
        /// Tries to determine the platform from the export preset's platform OS name.
        /// </summary>
        /// <param name="osName">Name of the export operating system.</param>
        /// <param name="platform">Platform name for the recognized supported platform.</param>
        /// <returns>
        /// <see langword="true"/> when the platform OS name is recognized as a supported platform,
        /// <see langword="false"/> otherwise.
        /// </returns>
        private static bool TryDeterminePlatformFromOSName(string osName, [NotNullWhen(true)] out string? platform)
        {
            if (OS.PlatformFeatureMap.TryGetValue(osName, out platform))
            {
                return true;
            }

            platform = null;
            return false;
        }

        private struct PublishConfig
        {
            public bool UseTempDir;
            public bool BundleOutputs;
            public string RidOS;
            public HashSet<string> Archs;
            public string BuildConfig;
            public bool IncludeDebugSymbols;
        }
    }
}
