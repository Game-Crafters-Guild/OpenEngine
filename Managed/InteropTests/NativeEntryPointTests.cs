using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Every GameEngine.Native export is a C function named <c>GE_*</c> (Scripting/*ABI.h). P/Invoke binds a
    /// declaration without an <c>EntryPoint</c> to the C# method name, so a declaration whose bound name lacks
    /// the prefix names a symbol the library does not export and throws EntryPointNotFoundException on its
    /// first call. The bound names come from metadata, so no native library loads: these suites run against
    /// GameEngine.NativeShim, which does not carry every export, so a call through it cannot prove a binding.
    /// </summary>
    public class NativeEntryPointTests
    {
        private const string kNativeLibrary = "GameEngine.Native";
        private const string kExportPrefix = "GE_";

        /// <summary>The script-facing ABI assembly: Scripting.ABI (TimelineApi and the rest).</summary>
        [Test]
        public void EveryNativeImportBindsAGePrefixedExport()
        {
            Assembly[] assemblies = { typeof(TimelineApi).Assembly };
            List<(string Declaration, string BoundName)> imports = NativeImports(assemblies).ToList();
            List<string> unbound = imports.Where(import => !import.BoundName.StartsWith(kExportPrefix))
                                          .Select(import => $"{import.Declaration} binds {import.BoundName}")
                                          .ToList();

            Assert.That(imports, Is.Not.Empty, "no GameEngine.Native import found, so the scan checked nothing");
            Assert.That(unbound, Is.Empty,
                        "these declarations bind a name GameEngine.Native does not export; " +
                        "add EntryPoint = \"GE_...\" with the C export's name: " + string.Join("; ", unbound));
        }

        /// <summary>
        /// The generated bindings call the engine through its function tables only: a DllImport or LibraryImport there,
        /// of any library, would skip the table's hash check, and under the assembly's DisableRuntimeMarshalling one with
        /// an out parameter throws.
        /// </summary>
        [Test]
        public void GeneratedBindingsHoldNoNativeImport()
        {
            const BindingFlags kDeclaredStatics = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static |
                                                  BindingFlags.DeclaredOnly;
            List<string> imports = typeof(Input).Assembly.GetTypes()
                .SelectMany(type => type.GetMethods(kDeclaredStatics).Select(method => (type, method)))
                .Where(entry => entry.method.GetCustomAttribute<DllImportAttribute>() != null ||
                                entry.method.GetCustomAttribute<LibraryImportAttribute>() != null)
                .Select(entry => $"{entry.type.FullName}.{entry.method.Name}")
                .ToList();
            Assert.That(imports, Is.Empty);
        }

        private static IEnumerable<(string Declaration, string BoundName)> NativeImports(IEnumerable<Assembly> assemblies)
        {
            const BindingFlags kDeclaredStatics = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static |
                                                  BindingFlags.DeclaredOnly;
            foreach (Assembly assembly in assemblies)
            {
                foreach (System.Type type in assembly.GetTypes())
                {
                    foreach (MethodInfo method in type.GetMethods(kDeclaredStatics))
                    {
                        DllImportAttribute? import = method.GetCustomAttribute<DllImportAttribute>();
                        if (import == null || import.Value != kNativeLibrary)
                            continue;
                        yield return ($"{type.FullName}.{method.Name}", import.EntryPoint ?? method.Name);
                    }
                }
            }
        }
    }
}
