using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;

namespace GameEngine.HotReload
{
    /// <summary>
    /// The assemblies of a script project's NuGet packages, staged in <c>Dependencies/</c> beside the
    /// scripts assembly, in a collectible context of their own.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The scripts context resolves its references from this context (<see cref="CollectibleAssemblyContext"/>),
    /// so a package's types keep one identity across every scripts context that uses them. A script reload
    /// whose staged set is unchanged (the same file names, sizes and write times) reuses the live context, and
    /// the packages keep their static state. A changed set builds a new context; the old one unloads when the
    /// last scripts context that uses it unloads, and its collection is verified like a scripts context's.
    /// </para>
    /// <para>
    /// An assembly whose simple name the engine already has (the shared framework, an assembly in the
    /// default context, a <c>GameEngine.*</c> assembly) is not loaded: scripts get the engine's copy, and a
    /// version difference is logged. Assemblies load from bytes, so the staged files are never locked and a
    /// new restore can replace them while the editor runs. A package that needs its own native libraries or
    /// files beside its assembly is not supported.
    /// </para>
    /// </remarks>
    internal sealed class ScriptDependencyContext : AssemblyLoadContext
    {
        /// <summary>The folder name, beside the scripts assembly, that holds the staged package assemblies.</summary>
        public const string FolderName = "Dependencies";

        private static readonly object s_Lock = new();
        private static readonly Dictionary<string, ScriptDependencyContext> s_CurrentByFolder = new(StringComparer.OrdinalIgnoreCase);
        private static string? s_LastFolder;

        private readonly Dictionary<string, Assembly> m_BySimpleName = new(StringComparer.OrdinalIgnoreCase);
        private readonly string m_Fingerprint;
        private readonly string m_Description;
        private int m_Users;
        private bool m_Retired;

        private ScriptDependencyContext(string folder, string fingerprint)
            : base($"GameEngine.ScriptDependencies ({folder})", isCollectible: true)
        {
            m_Fingerprint = fingerprint;
            m_Description = folder;
        }

        /// <summary>
        /// The dependency context for the scripts assembly in <paramref name="scriptsDirectory"/>, with one more
        /// user: the live context when the staged set is unchanged, else a new one. Null when the directory holds
        /// no <c>Dependencies/</c> assemblies. Each call is paired with one <see cref="Release"/>, which the scripts
        /// context does when it unloads.
        /// </summary>
        public static ScriptDependencyContext? Acquire(string? scriptsDirectory)
        {
            if (string.IsNullOrEmpty(scriptsDirectory))
                return null;
            string folder = Path.GetFullPath(Path.Combine(scriptsDirectory, FolderName));
            string fingerprint = Fingerprint(folder);
            lock (s_Lock)
            {
                s_LastFolder = folder;
                s_CurrentByFolder.TryGetValue(folder, out var current);
                if (current != null && current.m_Fingerprint == fingerprint)
                {
                    current.m_Users++;
                    return current;
                }
                current?.RetireLocked();
                s_CurrentByFolder.Remove(folder);
                if (fingerprint.Length == 0)
                    return null;
                var created = new ScriptDependencyContext(folder, fingerprint);
                created.LoadStaged(folder);
                created.m_Users = 1;
                s_CurrentByFolder[folder] = created;
                return created;
            }
        }

        /// <summary>
        /// The context of the folder <see cref="Acquire"/> last saw, with one more user, for a scripts context
        /// created from bytes with no directory of its own. Null when there is none.
        /// </summary>
        public static ScriptDependencyContext? AcquireCurrent()
        {
            lock (s_Lock)
            {
                if (s_LastFolder == null || !s_CurrentByFolder.TryGetValue(s_LastFolder, out var current))
                    return null;
                current.m_Users++;
                return current;
            }
        }

        /// <summary>One user fewer; a replaced context with no users left unloads.</summary>
        public void Release()
        {
            lock (s_Lock)
            {
                m_Users--;
                if (m_Users <= 0 && m_Retired)
                    UnloadLocked();
            }
        }

        /// <summary>The package assembly with this simple name, or null to let the default context answer.</summary>
        public Assembly? Find(AssemblyName assemblyName)
        {
            if (string.IsNullOrEmpty(assemblyName.Name))
                return null;
            lock (m_BySimpleName)
                return m_BySimpleName.TryGetValue(assemblyName.Name, out var assembly) ? assembly : null;
        }

        /// <summary>
        /// The leak line's advice for a context that does not unload while this one is loaded: a static field of
        /// a package assembly that still holds an object or a delegate from the old scripts pins them.
        /// </summary>
        public string LeakAdvice
        {
            get
            {
                string files;
                lock (m_BySimpleName)
                    files = string.Join(", ", m_BySimpleName.Keys.OrderBy(n => n, StringComparer.OrdinalIgnoreCase).Select(n => n + ".dll"));
                return $"If a static field in {files} holds an object or a delegate from the scripts, clear it in an [OnScriptsUnload] method of the scripts.";
            }
        }

        /// <inheritdoc />
        protected override Assembly? Load(AssemblyName assemblyName) => Find(assemblyName);

        private void RetireLocked()
        {
            m_Retired = true;
            if (m_Users <= 0)
                UnloadLocked();
        }

        private void UnloadLocked()
        {
            string advice = LeakAdvice;
            lock (m_BySimpleName)
                m_BySimpleName.Clear();
            Unload();
            HotReloadManager.ScheduleUnloadVerification(this, 0, $"script dependencies in {m_Description}", advice);
        }

        // The staged set: every *.dll by name, with its size and write time. Empty when there is none.
        private static string Fingerprint(string folder)
        {
            if (!Directory.Exists(folder))
                return string.Empty;
            var parts = new List<string>();
            foreach (var path in Directory.EnumerateFiles(folder, "*.dll", SearchOption.TopDirectoryOnly).OrderBy(p => p, StringComparer.OrdinalIgnoreCase))
            {
                var info = new FileInfo(path);
                parts.Add($"{info.Name}|{info.Length}|{info.LastWriteTimeUtc.Ticks}");
            }
            return string.Join(";", parts);
        }

        private void LoadStaged(string folder)
        {
            var engineAssemblies = EngineAssemblyVersions();
            bool IsOwnedByTheEngine(string dll)
            {
                AssemblyName staged;
                try { staged = AssemblyName.GetAssemblyName(dll); }
                catch { return false; } // not an assembly: the loader reports it
                string name = staged.Name ?? string.Empty;
                if (!name.StartsWith("GameEngine.", StringComparison.OrdinalIgnoreCase) && !engineAssemblies.ContainsKey(name))
                    return false;
                Version? engineVersion = EngineVersion(engineAssemblies, name);
                if (engineVersion != null && staged.Version != null && staged.Version != engineVersion)
                {
                    Console.Error.WriteLine(
                        $"[Scripts] The project's packages stage {name} {staged.Version}, but the engine already uses {name} {engineVersion}: scripts get the engine's version. Reference version {engineVersion} to compile against what runs.");
                }
                return true;
            }

            var loaded = new List<(Assembly Asm, string Path)>();
            HotReloadManager.LoadFolderAssemblies(this, folder, "package", loaded, dll => !IsOwnedByTheEngine(dll));
            lock (m_BySimpleName)
            {
                foreach (var (assembly, _) in loaded)
                {
                    string? name = assembly.GetName().Name;
                    if (!string.IsNullOrEmpty(name))
                        m_BySimpleName[name] = assembly;
                }
            }
        }

        // Simple name to the file of every assembly the engine has without the project's packages: the
        // shared framework (the trusted platform assemblies) and what the default context has loaded
        // (an in-memory assembly maps to an empty path; its version is read from the loaded assembly).
        private static Dictionary<string, string> EngineAssemblyVersions()
        {
            var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") is string tpa)
            {
                foreach (var path in tpa.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
                    files[Path.GetFileNameWithoutExtension(path)] = path;
            }
            foreach (var assembly in Default.Assemblies)
            {
                string? name = assembly.GetName().Name;
                if (!string.IsNullOrEmpty(name) && !files.ContainsKey(name))
                    files[name] = string.Empty;
            }
            return files;
        }

        private static Version? EngineVersion(Dictionary<string, string> engineAssemblies, string name)
        {
            foreach (var assembly in Default.Assemblies)
            {
                var loaded = assembly.GetName();
                if (string.Equals(loaded.Name, name, StringComparison.OrdinalIgnoreCase))
                    return loaded.Version;
            }
            if (engineAssemblies.TryGetValue(name, out var path) && path.Length > 0)
            {
                try { return AssemblyName.GetAssemblyName(path).Version; }
                catch { return null; }
            }
            return null;
        }
    }
}
