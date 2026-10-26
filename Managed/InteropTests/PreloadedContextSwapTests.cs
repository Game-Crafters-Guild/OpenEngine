using System;
using System.IO;
using System.Reflection;
using NUnit.Framework;
using GameEngine;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Verifies the managed preloaded context flow can be invoked via CoreBridge wrappers
    /// and that the façade remains functional after swap.
    /// </summary>
    public class PreloadedContextSwapTests
    {
        private static Type GetCoreBridgeType()
        {
            var type = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false);
            Assert.That(type, Is.Not.Null, "CoreBridge type not found");
            return type!;
        }

        /// <summary>
        /// Preloads then swaps a preloaded context through managed wrappers and validates façade functionality.
        /// </summary>
        [Test]
        public void Managed_Preload_And_Swap_DoesNotBreakFacade()
        {
            var world = EcsTestHelpers.RequirePrimaryWorld("PreloadedContextSwap.Managed_Preload_And_Swap_DoesNotBreakFacade");
            Assert.That(world.EntityCount, Is.GreaterThanOrEqualTo(0));

            var type = GetCoreBridgeType();
            var preload = type.GetMethod("ManagedPreloadAssemblyContext", BindingFlags.Static | BindingFlags.NonPublic);
            var swap = type.GetMethod("ManagedSwapPreloadedContext", BindingFlags.Static | BindingFlags.NonPublic);
            Assert.That(preload, Is.Not.Null);
            Assert.That(swap, Is.Not.Null);
            ScriptCompileHelper.EnsureHrmLoaded();
            var (assembly, _) = ScriptCompileHelper.CompileAssemblyReturning(7);
            var directory = Directory.CreateTempSubdirectory("ge-preloaded-context-");
            try
            {
                var assemblyPath = Path.Combine(directory.FullName, "TestScripts.dll");
                File.WriteAllBytes(assemblyPath, assembly);
                var preloadResult = (int)preload!.Invoke(null, new object[] { assemblyPath })!;
                Assert.That(preloadResult, Is.EqualTo(0), $"Preload failed rc={preloadResult}");

                var swapResult = (int)swap!.Invoke(null, null)!;
                Assert.That(swapResult, Is.EqualTo(0), $"Swap failed rc={swapResult}");

                var worldAfterSwap = EcsTestHelpers.RequirePrimaryWorld("PreloadedContextSwap.Managed_Preload_And_Swap_DoesNotBreakFacade.PostSwap");
                Assert.That(worldAfterSwap.EntityCount, Is.GreaterThanOrEqualTo(0));
            }
            finally
            {
                GameEngine.CoreBridge.CoreBridge.UnloadUserScripts();
                directory.Delete(recursive: true);
            }
        }
    }
}

