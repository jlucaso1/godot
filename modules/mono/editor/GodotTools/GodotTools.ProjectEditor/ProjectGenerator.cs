using System;
using System.Globalization;
using System.IO;
using System.Text;
using Microsoft.Build.Construction;
using Microsoft.Build.Evaluation;
using GodotTools.Shared;

namespace GodotTools.ProjectEditor
{
    public static class ProjectGenerator
    {
        public static string GodotSdkAttrValue => $"Godot.NET.Sdk/{GeneratedGodotNupkgsVersions.GodotNETSdk}";

        public static string GodotMinimumRequiredTfm => "net8.0";

        /// <summary>
        /// Web exports have to match the runtime pack the export template links, which is
        /// newer than the minimum the other platforms accept.
        /// </summary>
        public static string GodotWebRequiredTfm => "net9.0";

        public static ProjectRootElement GenGameProject(string name)
        {
            if (name.Length == 0)
                throw new ArgumentException("Project name is empty.", nameof(name));

            var root = ProjectRootElement.Create(NewProjectFileOptions.None);

            root.Sdk = GodotSdkAttrValue;

            var mainGroup = root.AddPropertyGroup();
            mainGroup.AddProperty("TargetFramework", GodotMinimumRequiredTfm);

            // Non-gradle builds require .NET 9 to match the jar libraries included in the export template.
            var net9 = mainGroup.AddProperty("TargetFramework", "net9.0");
            net9.Condition = " '$(GodotTargetPlatform)' == 'android' ";

            // The Web template statically links one exact `browser-wasm` runtime pack, and the
            // interop tables generated beside it only fit that pack's BCL. Publishing against
            // an older one links cleanly and then reports the runtime and class libraries as
            // out of sync at the first managed call.
            var webTfm = mainGroup.AddProperty("TargetFramework", GodotWebRequiredTfm);
            webTfm.Condition = " '$(GodotTargetPlatform)' == 'web' ";

            mainGroup.AddProperty("EnableDynamicLoading", "true");

            string sanitizedName = IdentifierUtils.SanitizeQualifiedIdentifier(name, allowEmptyIdentifiers: true);

            // If the name is not a valid namespace, manually set RootNamespace to a sanitized one.
            if (sanitizedName != name)
                mainGroup.AddProperty("RootNamespace", sanitizedName);

            return root;
        }

        public static string GenAndSaveGameProject(string dir, string name)
        {
            if (name.Length == 0)
                throw new ArgumentException("Project name is empty.", nameof(name));

            string path = Path.Combine(dir, name + ".csproj");

            var root = GenGameProject(name);

            // Save (without BOM)
            root.Save(path, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

            return Guid.NewGuid().ToString().ToUpperInvariant();
        }
    }
}
