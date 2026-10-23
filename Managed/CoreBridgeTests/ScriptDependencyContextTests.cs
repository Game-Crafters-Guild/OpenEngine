using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using NUnit.Framework;

namespace GameEngine.CoreBridgeTests
{
    /// <summary>
    /// A project's NuGet package assemblies, staged in Dependencies/ beside its scripts assembly, load into
    /// a dependency context of their own that the scripts resolve their references from and that outlives
    /// a script reload while the staged set is unchanged.
    /// </summary>
    [NonParallelizable]
    public class ScriptDependencyContextTests
    {
        private const string kDependency = "GeTestDep";

        private static string DependencySource(int version) => $@"
[assembly: System.Reflection.AssemblyVersion(""{version}.0.0.0"")]
namespace GeTestDep
{{
    public static class Counter
    {{
        private static int s_Count;
        public static System.Func<int>? Held;
        public static int Next() => ++s_Count;
        public static int Version => {version};
    }}
}}";

        private const string kScriptsSource = @"
namespace DepTest.Scripts
{
    public static class Entry
    {
        public static int DepVersion() => GeTestDep.Counter.Version;
        public static int DepNext() => GeTestDep.Counter.Next();
        public static int Pin() { GeTestDep.Counter.Held = () => 7; return 0; }
    }
}";

        private string m_Root = string.Empty;
        private TextWriter? m_SavedError;
        private StringWriter m_Errors = new();

        /// <summary>A fresh project folder per test, and the error output captured.</summary>
        [SetUp]
        public void SetUp()
        {
            m_Root = Path.Combine(Path.GetTempPath(), "GE_ScriptDependencyContext_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Path.Combine(m_Root, "Dependencies"));
            m_SavedError = Console.Error;
            m_Errors = new StringWriter();
            Console.SetError(TextWriter.Synchronized(m_Errors));
        }

        /// <summary>The scripts unloaded and the folder removed.</summary>
        [TearDown]
        public void TearDown()
        {
            ScriptLifecycleTests.Unload();
            Console.SetError(m_SavedError!);
            try { Directory.Delete(m_Root, recursive: true); } catch { }
        }

        private static IEnumerable<MetadataReference> FrameworkReferences() =>
            ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!)
                .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)
                .Select(p => MetadataReference.CreateFromFile(p));

        private static void Compile(string assemblyName, string source, string outputPath, params string[] references)
        {
            var compilation = CSharpCompilation.Create(
                assemblyName,
                new[] { CSharpSyntaxTree.ParseText(source, new CSharpParseOptions(LanguageVersion.Preview)) },
                FrameworkReferences().Concat(references.Select(r => MetadataReference.CreateFromFile(r))),
                new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, nullableContextOptions: NullableContextOptions.Enable));
            var result = compilation.Emit(outputPath);
            Assert.That(result.Success, Is.True, string.Join("\n", result.Diagnostics));
        }

        private string DependencyPath => Path.Combine(m_Root, "Dependencies", kDependency + ".dll");
        private string ScriptsPath => Path.Combine(m_Root, "DepTestScripts.dll");

        // Stages the dependency at `version`, compiles the scripts against it, loads and swaps them in.
        private void StageAndLoad(int version)
        {
            string built = Path.Combine(m_Root, $"build-v{version}", kDependency + ".dll");
            Directory.CreateDirectory(Path.GetDirectoryName(built)!);
            Compile(kDependency, DependencySource(version), built);
            File.Copy(built, DependencyPath, overwrite: true);
            Compile("DepTestScripts", kScriptsSource, ScriptsPath, DependencyPath);
            Load();
        }

        private void Load()
        {
            Assert.That(ScriptLifecycleTests.ManagedPreload(ScriptsPath), Is.EqualTo(0));
            Assert.That(ScriptLifecycleTests.ManagedSwap(), Is.EqualTo(0));
        }

        private static int Call(string method) => ScriptLifecycleTests.ManagedCall("DepTest.Scripts.Entry." + method);

        [MethodImpl(MethodImplOptions.NoInlining)]
        private WeakReference DependencyContextReference()
        {
            string name = $"GameEngine.ScriptDependencies ({Path.GetFullPath(Path.Combine(m_Root, "Dependencies"))})";
            var context = AssemblyLoadContext.All.Single(c => c.Name == name);
            return new WeakReference(context);
        }

        private static bool IsCollected(WeakReference reference)
        {
            for (int attempt = 0; attempt < 20 && reference.IsAlive; attempt++)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
                System.Threading.Thread.Sleep(50);
            }
            return !reference.IsAlive;
        }

        private string WaitForError(string needle)
        {
            for (int attempt = 0; attempt < 60; attempt++)
            {
                string text = m_Errors.ToString();
                if (text.Contains(needle, StringComparison.Ordinal))
                    return text;
                GC.Collect();
                System.Threading.Thread.Sleep(100);
            }
            return m_Errors.ToString();
        }

        /// <summary>A script that calls into a staged package runs.</summary>
        [Test]
        public void ScriptUsingAStagedDependencyRuns()
        {
            StageAndLoad(1);
            Assert.That(Call("DepVersion"), Is.EqualTo(1));
        }

        /// <summary>A reload with the staged set unchanged keeps the package's static state.</summary>
        [Test]
        public void ReloadWithUnchangedDependenciesKeepsTheirState()
        {
            StageAndLoad(1);
            Assert.That(Call("DepNext"), Is.EqualTo(1));
            Load();
            Assert.That(Call("DepNext"), Is.EqualTo(2), "the reload rebuilt the dependency context");
        }

        /// <summary>A new package version builds a new context, and the old one is collected.</summary>
        [Test]
        public void ChangedSetRebuildsTheContextAndTheOldOneIsCollected()
        {
            StageAndLoad(1);
            Assert.That(Call("DepNext"), Is.EqualTo(1));
            WeakReference old = DependencyContextReference();

            StageAndLoad(2);
            Assert.That(Call("DepVersion"), Is.EqualTo(2));
            Assert.That(Call("DepNext"), Is.EqualTo(1), "the new version kept the old version's state");
            Assert.That(IsCollected(old), Is.True, "the replaced dependency context was not collected");
        }

        /// <summary>
        /// A replaced dependency context keeps serving the live scripts until they unload: a package they first
        /// touch after a newer set is preloaded, and before the swap, still resolves.
        /// </summary>
        [Test]
        public void ReplacedContextServesTheLiveScriptsUntilTheyUnload()
        {
            const string second = "GeTestDepSecond";
            string secondPath = Path.Combine(m_Root, "Dependencies", second + ".dll");
            Compile(second, "namespace GeTestDepSecond { public static class Thing { public static int Value => 5; } }", secondPath);
            const string scriptsSource = @"
namespace DepTest.Scripts
{
    public static class Entry
    {
        public static int DepVersion() => GeTestDep.Counter.Version;
        public static int SecondValue() => Late.Value(0);
    }

    // Takes an argument and is not inlined, so the preload's warm-up never compiles it.
    internal static class Late
    {
        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
        public static int Value(int unused) => GeTestDepSecond.Thing.Value;
    }
}";
            string built = Path.Combine(m_Root, "build-v1", kDependency + ".dll");
            Directory.CreateDirectory(Path.GetDirectoryName(built)!);
            Compile(kDependency, DependencySource(1), built);
            File.Copy(built, DependencyPath, overwrite: true);
            Compile("DepTestScripts", scriptsSource, ScriptsPath, DependencyPath, secondPath);
            Load();
            Assert.That(Call("DepVersion"), Is.EqualTo(1));

            // A new set preloaded and not swapped in: the live scripts still run on the replaced context.
            built = Path.Combine(m_Root, "build-v2", kDependency + ".dll");
            Directory.CreateDirectory(Path.GetDirectoryName(built)!);
            Compile(kDependency, DependencySource(2), built);
            File.Copy(built, DependencyPath, overwrite: true);
            string nextScripts = Path.Combine(m_Root, "DepTestScriptsNext.dll");
            Compile("DepTestScriptsNext", scriptsSource, nextScripts, DependencyPath, secondPath);
            Assert.That(ScriptLifecycleTests.ManagedPreload(nextScripts), Is.EqualTo(0));

            Assert.That(Call("SecondValue"), Is.EqualTo(5), "the live scripts lost their packages when a newer set was preloaded");
            Assert.That(ScriptLifecycleTests.ManagedSwap(), Is.EqualTo(0));
        }

        /// <summary>
        /// A package static that still holds a delegate into the old scripts pins them past a reload, and the
        /// leak line names the package assembly and the fix.
        /// </summary>
        [Test]
        public void ScriptsPinnedByAPackageStaticLeakNamesTheAssemblyAndTheFix()
        {
            StageAndLoad(1);
            Assert.That(Call("Pin"), Is.EqualTo(0));
            Load();

            string errors = WaitForError("[OnScriptsUnload]");
            Assert.That(errors, Does.Contain("ALC LEAK"));
            Assert.That(errors, Does.Contain($"If a static field in {kDependency}.dll holds an object or a delegate from the scripts"));
        }

        /// <summary>A staged file that does not load is reported once, and the rest still load.</summary>
        [Test]
        public void DependencyThatFailsToLoadIsReportedOnce()
        {
            File.WriteAllBytes(Path.Combine(m_Root, "Dependencies", "Broken.dll"), new byte[] { 1, 2, 3, 4 });
            StageAndLoad(1);
            Assert.That(Call("DepVersion"), Is.EqualTo(1));
            string errors = m_Errors.ToString();
            int count = errors.Split('\n').Count(line => line.Contains("Broken.dll", StringComparison.Ordinal));
            Assert.That(count, Is.EqualTo(1), errors);
        }

        /// <summary>A staged assembly the engine already has stays the engine's, and a version difference is logged.</summary>
        [Test]
        public void EngineSharedNameStaysInTheDefaultContext()
        {
            Compile("System.Text.Json", "[assembly: System.Reflection.AssemblyVersion(\"1.0.0.0\")]",
                    Path.Combine(m_Root, "Dependencies", "System.Text.Json.dll"));
            StageAndLoad(1);
            Assert.That(Call("DepVersion"), Is.EqualTo(1));
            Assert.That(m_Errors.ToString(), Does.Contain("stage System.Text.Json 1.0.0.0, but the engine already uses System.Text.Json"));
            var stagedCopies = AssemblyLoadContext.All
                .Where(c => c.Name != null && c.Name.StartsWith("GameEngine.ScriptDependencies", StringComparison.Ordinal))
                .SelectMany(c => c.Assemblies)
                .Count(a => a.GetName().Name == "System.Text.Json");
            Assert.That(stagedCopies, Is.EqualTo(0));
        }
    }
}
