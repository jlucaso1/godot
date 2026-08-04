using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using Microsoft.Build.Construction;
using Microsoft.Build.Evaluation;
using Microsoft.Build.Locator;
using NuGet.Frameworks;

namespace GodotTools.ProjectEditor
{
    public sealed class MSBuildProject
    {
        internal ProjectRootElement Root { get; set; }

        public bool HasUnsavedChanges { get; set; }

        public void Save() => Root.Save();

        public MSBuildProject(ProjectRootElement root)
        {
            Root = root;
        }
    }

    public static partial class ProjectUtils
    {
        [GeneratedRegex(@"\s*'\$\(GodotTargetPlatform\)'\s*==\s*'(?<platform>[A-z]+)'\s*", RegexOptions.IgnoreCase)]
        private static partial Regex GodotTargetPlatformConditionRegex();

        private static readonly string[] _platformNames =
        {
            "windows",
            "linuxbsd",
            "macos",
            "android",
            "ios",
            "web",
        };

        public static void MSBuildLocatorRegisterLatest(out Version version, out string path)
        {
            var instance = MSBuildLocator.QueryVisualStudioInstances()
                .OrderByDescending(x => x.Version)
                .First();
            MSBuildLocator.RegisterInstance(instance);
            version = instance.Version;
            path = instance.MSBuildPath;
        }

        public static void MSBuildLocatorRegisterMSBuildPath(string msbuildPath)
            => MSBuildLocator.RegisterMSBuildPath(msbuildPath);

        public static MSBuildProject? Open(string path)
        {
            var root = ProjectRootElement.Open(path, ProjectCollection.GlobalProjectCollection, preserveFormatting: true);
            return root != null ? new MSBuildProject(root) : null;
        }

        public static void UpgradeProjectIfNeeded(MSBuildProject project, string projectName)
        {
            // NOTE: The order in which changes are made to the project is important.

            // Migrate to MSBuild project Sdks style if using the old style.
            MigrateToProjectSdksStyle(project, projectName);

            EnsureGodotSdkIsUpToDate(project);
            EnsureTargetFrameworkMatchesMinimumRequirement(project);
            EnsureWebTargetFrameworkIsSupported(project);
        }

        [GeneratedRegex(@"^\s*'\$\(GodotTargetPlatform\)'\s*==\s*'web'\s*$", RegexOptions.IgnoreCase)]
        private static partial Regex SelectsWebPlatformRegex();

        /// <summary>
        /// Whether a TargetFramework with these conditions applies to a Web export and to
        /// nothing narrower. Exactly one of the two may select Web, and the other has to be
        /// empty: anything else either excludes Web or adds a further restriction, and in
        /// both cases the property does not apply to every Web export.
        /// </summary>
        private static bool SelectsWebPlatformOnly(string propertyCondition, string groupCondition)
        {
            bool propertySelectsWeb = SelectsWebPlatformRegex().IsMatch(propertyCondition);
            bool groupSelectsWeb = SelectsWebPlatformRegex().IsMatch(groupCondition);

            if (propertySelectsWeb)
            {
                return string.IsNullOrWhiteSpace(groupCondition);
            }

            return groupSelectsWeb && string.IsNullOrWhiteSpace(propertyCondition);
        }

        /// <summary>
        /// Adds the Web-specific target framework to projects created before Web export
        /// existed. The Web export template statically links one runtime pack and carries
        /// interop tables generated from its class libraries, so a project publishing an
        /// older framework's class libraries against it links and then reports the runtime
        /// and class libraries as out of sync. Godot.NET.Sdk refuses such an export; this
        /// keeps existing projects from having to be edited by hand to avoid that.
        /// </summary>
        private static void EnsureWebTargetFrameworkIsSupported(MSBuildProject project)
        {
            var root = project.Root;

            foreach (var propertyGroup in root.PropertyGroups)
            {
                foreach (var property in propertyGroup.Properties)
                {
                    if (property.Name != "TargetFramework")
                    {
                        continue;
                    }

                    // Already decided for Web, by the project or by an earlier upgrade.
                    //
                    // Both conditions have to be accounted for, and neither may say anything
                    // beyond selecting Web. Matching the literal anywhere is not enough: a
                    // condition like '$(GodotTargetPlatform)' != 'web' mentions Web precisely
                    // to exclude it, and a Web override nested in, say, a Debug-only group
                    // does not apply to the release publish an export runs, so in both cases
                    // the baseline would silently stay in force.
                    if (SelectsWebPlatformOnly(property.Condition, propertyGroup.Condition))
                    {
                        // An override only decides anything if nothing assigns
                        // TargetFramework after it. A later unconditional baseline, or a
                        // trailing import of shared properties, evaluates last and wins, so
                        // in that case this one is left alone and a fresh override is
                        // appended at the end below, where it supersedes both.
                        if (!IsLastFrameworkAssignment(root, property))
                        {
                            continue;
                        }

                        // Selecting Web is not the same as selecting the right framework.
                        // The template links exactly one runtime pack, so anything other
                        // than the required value is rejected at export -- an older one and
                        // a newer one alike -- and an override already scoped to Web is the
                        // right place to correct it.
                        if (property.Value != ProjectGenerator.GodotWebRequiredTfm)
                        {
                            property.Value = ProjectGenerator.GodotWebRequiredTfm;
                            project.HasUnsavedChanges = true;
                        }

                        return;
                    }
                }
            }

            // MSBuild evaluates properties in document order, so this has to come after every
            // other TargetFramework or a later unconditional one silently wins.
            ProjectPropertyElement? lastTargetFramework = null;
            foreach (var propertyGroup in root.PropertyGroups)
            {
                foreach (var property in propertyGroup.Properties)
                {
                    if (property.Name == "TargetFramework")
                    {
                        lastTargetFramework = property;
                    }
                }
            }

            var webTfm = root.CreatePropertyElement("TargetFramework");
            webTfm.Value = ProjectGenerator.GodotWebRequiredTfm;
            webTfm.Condition = " '$(GodotTargetPlatform)' == 'web' ";

            // Only into a group without a condition of its own: MSBuild ands the group's
            // condition with the property's, so landing in, say, an Android-only group would
            // ask for a platform that is both Android and Web and never apply.
            //
            // And only when nothing that could assign TargetFramework comes after it. A
            // project that ends with an import of shared properties is a common shape, and
            // an import evaluated later overwrites whatever was set before it: the override
            // would look saved and the export would still resolve the imported framework.
            var parentGroup = lastTargetFramework?.Parent as ProjectPropertyGroupElement;
            if (lastTargetFramework != null && parentGroup != null
                && string.IsNullOrEmpty(parentGroup.Condition) && !HasImportAfter(root, parentGroup))
            {
                parentGroup.InsertAfterChild(webTfm, lastTargetFramework);
            }
            else
            {
                // AppendChild rather than AddPropertyGroup: the latter groups the new element
                // with the existing property groups, which would put it back before a
                // trailing import.
                var group = root.CreatePropertyGroupElement();
                root.AppendChild(group);
                group.AppendChild(webTfm);
            }

            project.HasUnsavedChanges = true;
        }

        /// <summary>
        /// Whether this element could assign TargetFramework somewhere this cannot see into.
        /// An import brings in a file that is not read here; a Choose picks one of several
        /// branches at evaluation time, and which one is not known here either. Both are
        /// treated as assignments, which at worst appends an override that was not needed.
        /// </summary>
        private static bool MayAssignFrameworkOpaquely(ProjectElement element)
            => element is ProjectImportElement
                || element is ProjectImportGroupElement
                || element is ProjectChooseElement;

        /// <summary>
        /// Whether nothing after <paramref name="property"/> can assign TargetFramework.
        /// MSBuild evaluates in document order, so an override that something later
        /// overwrites decides nothing, however well scoped it is. An import counts as an
        /// assignment because its contents are not known here.
        /// </summary>
        private static bool IsLastFrameworkAssignment(ProjectRootElement root, ProjectPropertyElement property)
        {
            bool seen = false;

            foreach (var child in root.Children)
            {
                if (child is ProjectPropertyGroupElement group)
                {
                    foreach (var candidate in group.Properties)
                    {
                        if (candidate.Name != "TargetFramework")
                        {
                            continue;
                        }

                        if (ReferenceEquals(candidate, property))
                        {
                            seen = true;
                        }
                        else if (seen)
                        {
                            return false;
                        }
                    }
                }
                else if (seen && MayAssignFrameworkOpaquely(child))
                {
                    return false;
                }
            }

            return seen;
        }

        /// <summary>
        /// Whether an import follows <paramref name="group"/>, and could therefore assign
        /// TargetFramework after anything placed in it.
        /// </summary>
        private static bool HasImportAfter(ProjectRootElement root, ProjectPropertyGroupElement group)
        {
            bool seen = false;

            foreach (var child in root.Children)
            {
                if (ReferenceEquals(child, group))
                {
                    seen = true;
                }
                else if (seen && MayAssignFrameworkOpaquely(child))
                {
                    return true;
                }
            }

            return false;
        }

        private static void MigrateToProjectSdksStyle(MSBuildProject project, string projectName)
        {
            var origRoot = project.Root;

            if (!string.IsNullOrEmpty(origRoot.Sdk))
                return;

            project.Root = ProjectGenerator.GenGameProject(projectName);
            project.Root.FullPath = origRoot.FullPath;
            project.HasUnsavedChanges = true;
        }

        public static void EnsureGodotSdkIsUpToDate(MSBuildProject project)
        {
            var root = project.Root;
            string godotSdkAttrValue = ProjectGenerator.GodotSdkAttrValue;

            if (!string.IsNullOrEmpty(root.Sdk) &&
                root.Sdk.Trim().Equals(godotSdkAttrValue, StringComparison.OrdinalIgnoreCase))
                return;

            root.Sdk = godotSdkAttrValue;
            project.HasUnsavedChanges = true;
        }

        private static void EnsureTargetFrameworkMatchesMinimumRequirement(MSBuildProject project)
        {
            var root = project.Root;
            string minTfmValue = ProjectGenerator.GodotMinimumRequiredTfm;
            var minTfmVersion = NuGetFramework.Parse(minTfmValue).Version;

            ProjectPropertyGroupElement? mainPropertyGroup = null;
            ProjectPropertyElement? mainTargetFrameworkProperty = null;

            var propertiesToChange = new List<ProjectPropertyElement>();

            foreach (var propertyGroup in root.PropertyGroups)
            {
                bool groupHasCondition = !string.IsNullOrEmpty(propertyGroup.Condition);

                // Check if the property group should be excluded from checking for 'TargetFramework' properties.
                if (groupHasCondition && !ConditionMatchesGodotPlatform(propertyGroup.Condition))
                {
                    continue;
                }

                // Store a reference to the first property group without conditions,
                // in case we need to add a new 'TargetFramework' property later.
                if (mainPropertyGroup == null && !groupHasCondition)
                {
                    mainPropertyGroup = propertyGroup;
                }

                foreach (var property in propertyGroup.Properties)
                {
                    // We are looking for 'TargetFramework' properties.
                    if (property.Name != "TargetFramework")
                    {
                        continue;
                    }

                    bool propertyHasCondition = !string.IsNullOrEmpty(property.Condition);

                    // Check if the property should be excluded.
                    if (propertyHasCondition && !ConditionMatchesGodotPlatform(property.Condition))
                    {
                        continue;
                    }

                    if (!groupHasCondition && !propertyHasCondition)
                    {
                        // Store a reference to the 'TargetFramework' that has no conditions
                        // because it applies to all platforms.
                        if (mainTargetFrameworkProperty == null)
                        {
                            mainTargetFrameworkProperty = property;
                        }
                        continue;
                    }

                    // If the 'TargetFramework' property is conditional, it may no longer be needed
                    // when the main one is upgraded to the new minimum version.
                    var tfmVersion = NuGetFramework.Parse(property.Value).Version;
                    if (tfmVersion <= minTfmVersion)
                    {
                        propertiesToChange.Add(property);
                    }
                }
            }

            if (mainTargetFrameworkProperty == null)
            {
                // We haven't found a 'TargetFramework' property without conditions,
                // we'll just add one in the first property group without conditions.
                if (mainPropertyGroup == null)
                {
                    // We also don't have a property group without conditions,
                    // so we'll add a new one to the project.
                    mainPropertyGroup = root.AddPropertyGroup();
                }

                mainTargetFrameworkProperty = mainPropertyGroup.AddProperty("TargetFramework", minTfmValue);
                project.HasUnsavedChanges = true;
            }
            else
            {
                var tfmVersion = NuGetFramework.Parse(mainTargetFrameworkProperty.Value).Version;
                if (tfmVersion < minTfmVersion)
                {
                    mainTargetFrameworkProperty.Value = minTfmValue;
                    project.HasUnsavedChanges = true;
                }
            }

            var mainTfmVersion = NuGetFramework.Parse(mainTargetFrameworkProperty.Value).Version;
            foreach (var property in propertiesToChange)
            {
                // If the main 'TargetFramework' property targets a version newer than
                // the minimum required by Godot, we don't want to remove the conditional
                // 'TargetFramework' properties, only upgrade them to the new minimum.
                // Otherwise, it can be removed.
                if (mainTfmVersion > minTfmVersion)
                {
                    var propertyTfmVersion = NuGetFramework.Parse(property.Value).Version;
                    if (propertyTfmVersion == minTfmVersion)
                    {
                        // The 'TargetFramework' property already matches the minimum version.
                        continue;
                    }

                    property.Value = minTfmValue;
                }
                else
                {
                    property.Parent.RemoveChild(property);
                }

                project.HasUnsavedChanges = true;
            }

            static bool ConditionMatchesGodotPlatform(string condition)
            {
                // Check if the condition is checking the 'GodotTargetPlatform' for one of the
                // Godot platforms with built-in support in the Godot.NET.Sdk.
                var match = GodotTargetPlatformConditionRegex().Match(condition);
                if (match.Success)
                {
                    string platform = match.Groups["platform"].Value;
                    return _platformNames.Contains(platform, StringComparer.OrdinalIgnoreCase);
                }

                return false;
            }
        }
    }
}
