using System;
using System.Collections.Generic;
using System.Reflection;
using NUnit.Framework;
using GameEngine.HotReload;
using GameEngine.Scripting;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// MainThread.Post's managed queue: the batch a drain runs, and the actions an unloading scripts context
    /// takes with it. Driven through the queue's internal half (the drain the engine's pump calls), because
    /// this host's engine has no frame loop and answers every post "run inline"; the native half is pinned
    /// in AbiNativeSmokeTest (MainThreadPostTests.cpp).
    /// </summary>
    public class MainThreadPostQueueTests
    {
        /// <summary>Each test starts from an empty queue.</summary>
        [SetUp]
        public void EmptyTheQueue() => MainThread.TakeAndRun(run: false);

        /// <summary>A drain runs the queued batch in order; a discarded batch never runs.</summary>
        [Test]
        public void ADrainRunsTheBatchInOrderAndADiscardRunsNothing()
        {
            var ran = new List<int>();
            MainThread.Enqueue(() => ran.Add(1));
            MainThread.Enqueue(() => ran.Add(2));
            MainThread.TakeAndRun(run: true);
            Assert.That(ran, Is.EqualTo(new[] { 1, 2 }));

            MainThread.Enqueue(() => ran.Add(3));
            MainThread.TakeAndRun(run: false);
            MainThread.TakeAndRun(run: true);
            Assert.That(ran, Is.EqualTo(new[] { 1, 2 }), "a discarded batch never runs");
        }

        /// <summary>
        /// An action whose code lives in a scripts context that unloads before the pump is dropped: it would
        /// run code that is going away. An action of the default context queued beside it still runs.
        /// </summary>
        [Test]
        public void MainThreadPostFromAnUnloadedContextDoesNotRun()
        {
            ScriptCompileHelper.EnsureHrmLoaded();
            const string src =
                "namespace TestScripts { public static class PostProbe { public static int Runs;\n" +
                "  public static void Run() { Runs++; } } }";
            var (asm, _) = ScriptCompileHelper.CompileCustomSources(src,
                assemblyName: "MainThreadPostScripts_" + Guid.NewGuid().ToString("N"));
            int domain = HotReloadManager.LoadCompiledAssembly(asm);
            Assert.That(domain, Is.GreaterThan(0));

            Type? probe = null;
            foreach (var a in HotReloadManager.GetActiveUserAssemblies())
            {
                probe = a.GetType("TestScripts.PostProbe", throwOnError: false);
                if (probe != null) break;
            }
            Assert.That(probe, Is.Not.Null);
            var scriptAction = (Action)Delegate.CreateDelegate(typeof(Action), probe!.GetMethod("Run")!);

            int defaultRuns = 0;
            MainThread.Enqueue(scriptAction);
            MainThread.Enqueue(() => defaultRuns++);
            Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0));

            MainThread.TakeAndRun(run: true);
            // The held Type keeps the unloading assembly readable for the assert.
            var runs = (int)(probe.GetField("Runs", BindingFlags.Public | BindingFlags.Static)!.GetValue(null) ?? -1);
            Assert.That(runs, Is.EqualTo(0), "the unloaded context's action ran");
            Assert.That(defaultRuns, Is.EqualTo(1), "the default context's action was dropped with it");
        }

        /// <summary>
        /// A drain the engine refused (another one is installed: a second copy of Scripting.ABI, or a native
        /// host's own) would strand every queued post, so Post throws and names the cause. Records the refusal
        /// the engine would answer (GE_Result_Fail) rather than installing a rival drain, so the arm needs no
        /// native library. This leaves MainThread refused for the rest of this test process; nothing else here
        /// calls Post.
        /// </summary>
        [Test]
        public void PostThrowsWhenTheEngineRefusedItsDrain()
        {
            const int kRefused = -1;
            MainThread.RecordDrainInstall(kRefused);
            bool ran = false;
            var refused = Assert.Throws<InvalidOperationException>(() => MainThread.Post(() => ran = true));
            Assert.That(refused!.Message, Does.Contain("drain"));
            Assert.That(ran, Is.False, "an action whose post was refused ran");
        }
    }
}
