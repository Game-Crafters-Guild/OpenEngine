using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using NUnit.Framework;
using GameEngine.HotReload;
using GameEngine.Scripting;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Explicit micro-benchmarks: the HRM Query/Invoke fast paths, and the per-call cost of a script API call.
    /// Skipped by default.
    /// </summary>
    [TestFixture]
    internal class MicroBenchmarks
    {
        private static void EnsureHrmLoaded()
        {
            var t = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: false);
            if (t != null) return;
            var baseDir = AppContext.BaseDirectory ?? Environment.CurrentDirectory;
            var built = Path.Combine(baseDir, "GameEngine.HotReload.dll");
            if (!File.Exists(built))
            {
                built = Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, "HotReload", "bin", "Debug", "net10.0", "GameEngine.HotReload.dll"));
            }
            Assert.That(File.Exists(built), Is.True, $"HotReload assembly not found: {built}");
            Environment.SetEnvironmentVariable("GE_HRM_PATH", built);
            AssemblyLoadContext.Default.LoadFromAssemblyPath(built);
        }

        private static byte[] CompileSimpleAssembly(int retVal)
        {
            return ScriptCompileHelper.CompileAssemblyReturning(retVal).asm;
        }

        private static void LoadAssemblyBytes(byte[] asmBytes)
        {
            int loadRc = HotReloadHelper.LoadFromBytes(asmBytes);
            Assert.That(loadRc, Is.GreaterThan(0));
        }

        [Test, Explicit("Micro-benchmark: Query warm path")]
        public void Bench_Query_Warm()
        {
            EnsureHrmLoaded();
            var cbType = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: true)!;
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);
            var asmBytes = CompileSimpleAssembly(1);
            LoadAssemblyBytes(asmBytes);

            // Warm-up: one query to ensure mapping exists
            ulong tok;
            Assert.That(HotReloadHelper.TryQuery("HotReloadTest.TestMethod", out tok), Is.EqualTo(0));

            const int iters = 200_000;
            var sw = Stopwatch.StartNew();
            for (int i = 0; i < iters; i++)
            {
                HotReloadHelper.TryQuery("HotReloadTest.TestMethod", out tok);
            }
            sw.Stop();
            TestContext.WriteLine($"Query warm: {iters} iters in {sw.Elapsed.TotalMilliseconds:F2} ms ({iters / Math.Max(1.0, sw.Elapsed.TotalSeconds):F0} ops/s)");
        }

        [Test, Explicit("Micro-benchmark: Invoke warm path")]
        public void Bench_Invoke_Warm()
        {
            EnsureHrmLoaded();
            var cbType = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: true)!;
            cbType.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, null);
            var asmBytes = CompileSimpleAssembly(1);
            LoadAssemblyBytes(asmBytes);

            ulong tok;
            Assert.That(HotReloadHelper.TryQuery("HotReloadTest.TestMethod", out tok), Is.EqualTo(0));

            int sum = 0;
            const int iters = 500_000;
            var sw = Stopwatch.StartNew();
            for (int i = 0; i < iters; i++)
            {
                sum += HotReloadHelper.TryInvoke(tok);
            }
            sw.Stop();
            TestContext.WriteLine($"Invoke warm: {iters} iters in {sw.Elapsed.TotalMilliseconds:F2} ms ({iters / Math.Max(1.0, sw.Elapsed.TotalSeconds):F0} ops/s), sum={sum}");
        }

        private const int kKeyDownCalls = 1_000_000;
        private const int kKeyDownRounds = 7;

        private static double TimeIsKeyDown()
        {
            int held = 0;
            var sw = Stopwatch.StartNew();
            for (int i = 0; i < kKeyDownCalls; i++)
                held += Input.IsKeyDown(Key.W) ? 1 : 0;
            sw.Stop();
            Assert.That(held, Is.Zero);
            return sw.Elapsed.TotalMilliseconds * 1e6 / kKeyDownCalls;
        }

        /// <summary>
        /// One million IsKeyDown calls through the generated binding against the real GameEngine.Native (the
        /// standalone engine the first call starts): run it from a Release build with the real library staged in
        /// place of the test shim. Prints nanoseconds per call for each round and the median.
        /// </summary>
        [Test, Explicit("Micro-benchmark: IsKeyDown through the generated binding")]
        public void Bench_InputIsKeyDown()
        {
            Assert.That(Input.IsKeyDown(Key.W), Is.False, "the real GameEngine.Native is not loaded");
            TimeIsKeyDown();
            var rounds = new double[kKeyDownRounds];
            for (int round = 0; round < kKeyDownRounds; round++)
            {
                rounds[round] = TimeIsKeyDown();
                TestContext.WriteLine($"IsKeyDown round {round}: {rounds[round]:F2} ns/call");
            }
            Array.Sort(rounds);
            TestContext.WriteLine($"IsKeyDown median of {kKeyDownRounds} x {kKeyDownCalls}: {rounds[kKeyDownRounds / 2]:F2} ns/call");
        }

        private const int kTransformPairs = 1_000_000;
        private const int kTransformRounds = 7;
        private const int kSystemEntities = 10_000;
        private const int kSystemUpdates = 1_000;

        private static double Median(double[] values)
        {
            var sorted = (double[])values.Clone();
            Array.Sort(sorted);
            return sorted[sorted.Length / 2];
        }

        // Nanoseconds per ComponentType<Transform>.Get of one entity, and per Get and Set pair.
        private static (double get, double pair) TimeTransformGetSet(in GameEngine.ECS.WorldHandle world, uint entity)
        {
            float sum = 0f;
            var sw = Stopwatch.StartNew();
            for (int i = 0; i < kTransformPairs; i++)
                sum += GameEngine.ECS.ComponentType<Transform>.Get(in world, entity).X;
            double get = sw.Elapsed.TotalMilliseconds * 1e6 / kTransformPairs;
            sw.Restart();
            for (int i = 0; i < kTransformPairs; i++)
            {
                Transform t = GameEngine.ECS.ComponentType<Transform>.Get(in world, entity);
                t.Translate(1e-6f, 0f, 0f);
                GameEngine.ECS.ComponentType<Transform>.Set(in world, entity, in t);
            }
            double pair = sw.Elapsed.TotalMilliseconds * 1e6 / kTransformPairs;
            Assert.That(sum, Is.GreaterThanOrEqualTo(0f));
            return (get, pair);
        }

        /// <summary>
        /// One million ComponentType&lt;Transform&gt;.Get and Set pairs on one entity against the real GameEngine.Native
        /// (the standalone engine). Prints nanoseconds per pair for each round, the median, and the Set share of the
        /// pair (pair minus a Get alone). Run it from a Release build with the real library staged in place of the
        /// test shim.
        /// </summary>
        [Test, Explicit("Micro-benchmark: Transform Get and Set through ComponentType<T>")]
        public void Bench_TransformGetSet()
        {
            GameEngine.ECS.WorldHandle world = GameEngine.ECS.Ecs.PrimaryWorld;
            uint entity = world.CreateEmptyEntity("Bench.TransformGetSet");
            try
            {
                TimeTransformGetSet(in world, entity);
                var pairs = new double[kTransformRounds];
                var sets = new double[kTransformRounds];
                for (int round = 0; round < kTransformRounds; round++)
                {
                    (double get, double pair) = TimeTransformGetSet(in world, entity);
                    pairs[round] = pair;
                    sets[round] = pair - get;
                    TestContext.WriteLine($"TransformGetSet round {round}: {pair:F2} ns/pair, Set {pair - get:F2} ns");
                }
                TestContext.WriteLine($"TransformGetSet median of {kTransformRounds} x {kTransformPairs}: {Median(pairs):F2} ns/pair, " +
                                      $"Set {Median(sets):F2} ns ({100.0 * Median(sets) / Median(pairs):F1} % of the pair)");
            }
            finally
            {
                world.DestroyEntity(entity);
            }
        }

        // Milliseconds per update of a ref Transform system over every entity with a Transform, iterated the way
        // the generated system iterates: one cached query, a chunk span per chunk, a ref per row.
        private static double TimeTransformSystem(ulong query)
        {
            ulong transformId = GameEngine.ECS.ComponentType<Transform>.CachedId;
            var sw = Stopwatch.StartNew();
            for (int update = 0; update < kSystemUpdates; update++)
            {
                GameEngine.ECS.Internal.ChunkQueryNative.ResetQuery(query, out int archetypeCount);
                for (int a = 0; a < archetypeCount; a++)
                {
                    GameEngine.ECS.Internal.ChunkQueryNative.GetArchetypeInfo(query, a, out _, out int chunkCount);
                    for (int c = 0; c < chunkCount; c++)
                    {
                        Span<Transform> span = GameEngine.ECS.Internal.ChunkQueryNative.GetChunkSpan<Transform>(query, a, c, transformId);
                        for (int i = 0; i < span.Length; i++)
                            span[i].Translate(1e-6f, 0f, 0f);
                    }
                }
            }
            return sw.Elapsed.TotalMilliseconds / kSystemUpdates;
        }

        /// <summary>
        /// A ref Transform system over 10,000 entities, 1,000 updates, against the real GameEngine.Native. Prints
        /// milliseconds per update for each round and the median. Run it as Bench_TransformGetSet.
        /// </summary>
        [Test, Explicit("Micro-benchmark: a ref Transform system over 10,000 entities")]
        public void Bench_TransformSystemIteration()
        {
            GameEngine.ECS.WorldHandle world = GameEngine.ECS.Ecs.PrimaryWorld;
            var entities = new uint[kSystemEntities];
            for (int i = 0; i < kSystemEntities; i++)
                entities[i] = world.CreateEmptyEntity("Bench.TransformSystem");
            ulong query = GameEngine.ECS.Internal.ChunkQueryNative.CreateCachedQuery(
                world.Handle, new[] { GameEngine.ECS.ComponentType<Transform>.CachedId }, ReadOnlySpan<ulong>.Empty);
            try
            {
                TimeTransformSystem(query);
                var rounds = new double[kTransformRounds];
                for (int round = 0; round < kTransformRounds; round++)
                {
                    rounds[round] = TimeTransformSystem(query);
                    TestContext.WriteLine($"TransformSystemIteration round {round}: {rounds[round] * 1000.0:F2} us/update");
                }
                TestContext.WriteLine($"TransformSystemIteration median of {kTransformRounds} x {kSystemUpdates} updates over " +
                                      $"{kSystemEntities} entities: {Median(rounds) * 1000.0:F2} us/update");
            }
            finally
            {
                GameEngine.ECS.Internal.ChunkQueryNative.DestroyCachedQuery(query);
                foreach (uint entity in entities)
                    world.DestroyEntity(entity);
            }
        }
    }
}
