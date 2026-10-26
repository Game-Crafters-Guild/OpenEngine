using System;
using System.Linq;
using System.Runtime.Loader;
using System.Reflection;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Tests that [InitializeOnLoad] methods are discovered during preload but only invoked after Stage 4 swap.
    /// </summary>
    public class InitializeOnLoadInvocationTests
    {
        /// <summary>
        /// Preload must not invoke IoL; swap must invoke IoL exactly once.
        /// </summary>
        [Test]
        public void Preload_DoesNotInvoke_IoL_OnlyAfterSwap()
        {
            ScriptCompileHelper.EnsureHrmLoaded();
            var coreBridge = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false);
            coreBridge?.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)?.Invoke(null, null);

            var source = "using System;\nnamespace GameEngine.Scripting { [AttributeUsage(AttributeTargets.Method, AllowMultiple=false, Inherited=false)] public sealed class InitializeOnLoadAttribute : Attribute { } }\nnamespace TestScripts { public static class Init { public static int Marker = 0; [GameEngine.Scripting.InitializeOnLoad] public static void Boot() { Marker = Marker + 1; } } }";
            var assemblyName = "InitializeOnLoad_" + Guid.NewGuid().ToString("N");
            var (assemblyBytes, symbols) = ScriptCompileHelper.CompileCustomSources(source, assemblyName);

            var hotReloadManagerType = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var preloadMethod = hotReloadManagerType.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[]{ typeof(byte[]), typeof(byte[]) }, modifiers: null)!;
            var swapMethod = hotReloadManagerType.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;


            int preloadResult = (int)preloadMethod.Invoke(null, new object?[]{ assemblyBytes, symbols })!;
            Assert.That(preloadResult, Is.EqualTo(0), "Preload failed");
            // Inspect the unique preloaded assembly through the runtime in every configuration.
            var preloadedAssembly = AssemblyLoadContext.All.SelectMany(context => context.Assemblies)
                .Single(assembly => assembly.GetName().Name == assemblyName);
            Assert.That(preloadedAssembly, Is.Not.Null, "Preloaded assembly missing after preload");
            var discoverMethod = hotReloadManagerType.GetMethod("DiscoverInitializeOnLoadMethods", BindingFlags.Static | BindingFlags.NonPublic);
            var discovered = (System.Collections.Generic.List<MethodInfo>)discoverMethod!.Invoke(null, new object?[]{ preloadedAssembly!, null })!;
            Assert.That(discovered.Count, Is.GreaterThanOrEqualTo(1), "No [InitializeOnLoad] methods discovered during preload");

            // Read Marker before swap (should be 0)
            var preloadedInitializationType = preloadedAssembly!.GetType("TestScripts.Init", throwOnError: true)!;
            var markerFieldPre = preloadedInitializationType.GetField("Marker", BindingFlags.Public | BindingFlags.Static)!;
            var markerPre = (int)markerFieldPre.GetValue(null)!;
            Assert.That(markerPre, Is.EqualTo(0), "Marker should be 0 before swap");

            int swapResult = (int)swapMethod.Invoke(null, null)!;
            Assert.That(swapResult, Is.EqualTo(0), "Swap failed");

            // Await IoL completion without polling
            var waitForInitializationMethod = hotReloadManagerType.GetMethod("WaitForPendingInitializeOnLoadAsync", BindingFlags.Public | BindingFlags.Static)!;
            var initialization = (System.Threading.Tasks.Task<bool>)waitForInitializationMethod.Invoke(null, new object?[]{ 5000 })!;
            Assert.That(initialization.GetAwaiter().GetResult(), Is.True, "InitializeOnLoad did not complete within timeout");

            // Read the active assembly after swap; a stale collectible context cannot satisfy this lookup.
            var currentAssembly = GameEngine.HotReload.HotReloadManager.GetActiveUserAssemblies()
                .Single(assembly => assembly.GetName().Name == assemblyName);
            Assert.That(currentAssembly, Is.Not.Null, "Current assembly missing after swap");
            var initializationType = currentAssembly!.GetType("TestScripts.Init", throwOnError: true)!;
            var markerField = initializationType.GetField("Marker", BindingFlags.Public | BindingFlags.Static)!;
            int marker = (int)markerField.GetValue(null)!;
            Assert.That(marker, Is.EqualTo(1), "InitializeOnLoad should set Marker to 1 after swap");
        }
    }
}

