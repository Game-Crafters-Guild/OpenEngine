using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The generated Physics binding through the real GameEngine.Native: a ray cast, the velocity of a body, and the
    /// contact events with their subscription rules. The dotnet test host has no frame loop, so BindingTestScene
    /// (Tests/ScriptBindings) builds the bodies and runs the physics steps, which deliver the events on this thread.
    /// </summary>
    [NonParallelizable]
    public class PhysicsBindingTests
    {
        private const string kScene = "BindingTestScene";

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_Open();

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_AddBox(float x, float y, float z, float halfX, float halfY, float halfZ,
                                                      BoxKind kind, out uint entity);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_Step(int steps);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_Close();

        // GE_TestScene_AddBox's kind.
        private enum BoxKind
        {
            Static = 0,
            Dynamic = 1,
            Trigger = 2,
        }

        private int m_Begins;
        private int m_Ends;
        private int m_Unsubscribed;
        private ContactEvent m_LastBegin;
        private readonly List<TriggerEvent> m_Enters = new List<TriggerEvent>();

        /// <summary>A generated call starts the standalone engine the scene needs.</summary>
        [OneTimeSetUp]
        public void StartEngine()
        {
            Assert.That(Physics.RayCast(Vector3.Zero, Vector3.UnitZ, out RayHit outside), Is.False);
            Assert.That(outside.Entity, Is.EqualTo(Entity.None), "outside play the hit holds its defaults");
        }

        /// <summary>Each test starts a physics world of its own.</summary>
        [SetUp]
        public void OpenScene()
        {
            Assert.That(GE_TestScene_Open(), Is.EqualTo(0));
            m_Begins = 0;
            m_Ends = 0;
            m_Unsubscribed = 0;
            m_Enters.Clear();
        }

        /// <summary>Leaving play mode shuts the world down.</summary>
        [TearDown]
        public void CloseScene()
        {
            Assert.That(GE_TestScene_Close(), Is.EqualTo(0));
        }

        private static Entity AddBox(Vector3 position, Vector3 halfExtents, BoxKind kind)
        {
            Assert.That(GE_TestScene_AddBox(position.X, position.Y, position.Z, halfExtents.X, halfExtents.Y, halfExtents.Z,
                                            kind, out uint id), Is.EqualTo(0));
            return new Entity(id);
        }

        private static Entity AddGround() => AddBox(new Vector3(0f, -0.5f, 0f), new Vector3(10f, 0.5f, 10f), BoxKind.Static);

        private void OnBegin(in ContactEvent e)
        {
            ++m_Begins;
            m_LastBegin = e;
            // Launch the box off the ground from inside the step, so the contact ends.
            e.Self.Physics.SetLinearVelocity(new Vector3(0f, 8f, 0f));
        }

        private void OnEnd(in ContactEvent e) => ++m_Ends;

        private void OnUnsubscribed(in ContactEvent e) => ++m_Unsubscribed;

        private void OnEnter(in TriggerEvent e) => m_Enters.Add(e);

        /// <summary>A ray along +Z hits the box in front of it, and a ray along -Z hits nothing.</summary>
        [Test]
        public void RayCastHitsTheBoxInFrontAndMissesBehind()
        {
            Entity box = AddBox(new Vector3(0f, 0f, 10f), Vector3.One, BoxKind.Static);

            Assert.That(Physics.RayCast(Vector3.Zero, new Vector3(0f, 0f, 2f), out RayHit hit), Is.True);
            Assert.That(hit.Entity, Is.EqualTo(box));
            Assert.That(hit.Distance, Is.EqualTo(9f).Within(1e-3f));
            Assert.That(hit.Point.Z, Is.EqualTo(9f).Within(1e-3f));
            Assert.That(hit.Normal.Z, Is.EqualTo(-1f).Within(1e-3f));

            Assert.That(Physics.RayCast(Vector3.Zero, -Vector3.UnitZ, out RayHit miss), Is.False);
            Assert.That(miss.Entity, Is.EqualTo(Entity.None));
            Assert.That(Physics.RayCast(Vector3.Zero, Vector3.UnitZ, out _, maxDistance: 5f), Is.False, "beyond maxDistance");
        }

        /// <summary>
        /// A ray along +Z through three boxes writes all three, nearest first; a span of two gets the nearest two and
        /// the count of three; an empty span asks the count.
        /// </summary>
        [Test]
        public void RayCastAllWritesEveryBodyAlongTheRayNearestFirst()
        {
            Entity near = AddBox(new Vector3(0f, 0f, 5f), new Vector3(1f, 1f, 0.5f), BoxKind.Static);
            Entity middle = AddBox(new Vector3(0f, 0f, 10f), new Vector3(1f, 1f, 0.5f), BoxKind.Static);
            Entity far = AddBox(new Vector3(0f, 0f, 15f), new Vector3(1f, 1f, 0.5f), BoxKind.Static);

            Span<RayHit> hits = stackalloc RayHit[4];
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits), Is.EqualTo(3));
            Assert.That(hits[0].Entity, Is.EqualTo(near));
            Assert.That(hits[1].Entity, Is.EqualTo(middle));
            Assert.That(hits[2].Entity, Is.EqualTo(far));
            Assert.That(hits[0].Distance, Is.EqualTo(4.5f).Within(1e-3f));
            Assert.That(hits[2].Distance, Is.EqualTo(14.5f).Within(1e-3f));

            Span<RayHit> two = stackalloc RayHit[2];
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, two), Is.EqualTo(3), "the full count, never a silent truncation");
            Assert.That(two[0].Entity, Is.EqualTo(near));
            Assert.That(two[1].Entity, Is.EqualTo(middle));
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, Span<RayHit>.Empty), Is.EqualTo(3), "an empty span asks the count");
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits, maxDistance: 12f), Is.EqualTo(2), "within maxDistance");
        }

        /// <summary>
        /// A box query finds the two bodies inside it and not the one outside; a span of one gets one of them and the
        /// count of two.
        /// </summary>
        [Test]
        public void BoxOverlapFindsTheBodiesInsideTheBox()
        {
            Entity left = AddBox(new Vector3(-1f, 0f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Static);
            Entity right = AddBox(new Vector3(1f, 0f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Static);
            AddBox(new Vector3(10f, 0f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Static);

            Assert.That(Unsafe.SizeOf<Entity>(), Is.EqualTo(4), "a Span<Entity> crosses as packed 32-bit entity ids");
            Span<Entity> bodies = stackalloc Entity[4];
            Assert.That(Physics.BoxOverlap(Vector3.Zero, new Vector3(3f, 1f, 1f), Quaternion.Identity, bodies), Is.EqualTo(2));
            Assert.That(new[] { bodies[0], bodies[1] }, Is.EquivalentTo(new[] { left, right }));

            Span<Entity> one = stackalloc Entity[1];
            Assert.That(Physics.BoxOverlap(Vector3.Zero, new Vector3(3f, 1f, 1f), Quaternion.Identity, one), Is.EqualTo(2),
                        "the full count, never a silent truncation");
            Assert.That(one[0] == left || one[0] == right, Is.True);
        }

        /// <summary>
        /// A box falls onto the ground: ContactBegin fires once when it lands, its handler launches it upward through the
        /// view's velocity, and ContactEnd fires once when it leaves.
        /// </summary>
        [Test]
        public void ContactBeginFiresOnceOnContactAndContactEndOnceOnSeparation()
        {
            Entity ground = AddGround();
            Entity box = AddBox(new Vector3(0f, 1f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            box.Physics.ContactBegin += OnBegin;
            box.Physics.ContactEnd += OnEnd;

            Assert.That(GE_TestScene_Step(60), Is.EqualTo(0));
            Assert.That(m_Begins, Is.EqualTo(1));
            Assert.That(m_Ends, Is.EqualTo(1));
            Assert.That(m_LastBegin.Self, Is.EqualTo(box));
            Assert.That(m_LastBegin.Other, Is.EqualTo(ground));
            Assert.That(m_LastBegin.Normal.Y, Is.GreaterThan(0.5f), "the normal points from the ground up into the box");
            Assert.That(box.Physics.GetLinearVelocity().Y, Is.GreaterThan(0f), "the box is still rising");

            box.Physics.ContactBegin -= OnBegin;
            box.Physics.ContactEnd -= OnEnd;
        }

        /// <summary>
        /// A box falls into a trigger that lies on the ground: TriggerEnter reaches the trigger's entity and the box's, each
        /// as Self. The steps stop soon after the box lands, before it rests long enough to sleep.
        /// </summary>
        [Test]
        public void TriggerEnterReachesTheTriggerAndTheBody()
        {
            AddGround();
            Entity trigger = AddBox(Vector3.Zero, new Vector3(2f, 0.5f, 2f), BoxKind.Trigger);
            Entity box = AddBox(new Vector3(0f, 3f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            trigger.Physics.TriggerEnter += OnEnter;
            box.Physics.TriggerEnter += OnEnter;

            Assert.That(GE_TestScene_Step(45), Is.EqualTo(0));

            Assert.That(m_Enters.Exists(e => e.Self == trigger && e.Other == box && e.SelfIsTrigger), Is.True);
            Assert.That(m_Enters.Exists(e => e.Self == box && e.Other == trigger && !e.SelfIsTrigger), Is.True);
            trigger.Physics.TriggerEnter -= OnEnter;
            box.Physics.TriggerEnter -= OnEnter;
        }

        /// <summary>A handler removed with -= is not called; the one left on the same event is.</summary>
        [Test]
        public void UnsubscribedHandlerIsNotCalled()
        {
            AddGround();
            Entity box = AddBox(new Vector3(0f, 1f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            box.Physics.ContactBegin += OnUnsubscribed;
            box.Physics.ContactBegin += OnBegin;
            box.Physics.ContactBegin -= OnUnsubscribed;

            Assert.That(GE_TestScene_Step(60), Is.EqualTo(0));

            Assert.That(m_Unsubscribed, Is.Zero);
            Assert.That(m_Begins, Is.EqualTo(1));
            box.Physics.ContactBegin -= OnBegin;
        }

        /// <summary>
        /// Leaving play mode (the physics world shuts down) ends the subscription: the binding's listener is gone, and a
        /// later -= of the same handler does nothing.
        /// </summary>
        [Test]
        public void SubscriptionDoesNotSurviveTheSceneUnload()
        {
            int before = ScriptSubscriptionRegistry.Count;
            Entity box = AddBox(new Vector3(0f, 1f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            box.Physics.ContactBegin += OnBegin;
            Assert.That(ScriptSubscriptionRegistry.Count, Is.EqualTo(before + 1));

            Assert.That(GE_TestScene_Close(), Is.EqualTo(0));

            Assert.That(ScriptSubscriptionRegistry.Count, Is.EqualTo(before));
            Assert.DoesNotThrow(() => box.Physics.ContactBegin -= OnBegin);
        }

        /// <summary>
        /// A handler whose code lives in a collectible scripts context (a hot-reloaded script) is dropped and its
        /// subscription ended when that context unloads, and the context is then collected.
        /// </summary>
        [Test]
        public void UnloadingTheScriptContextEndsItsSubscriptions()
        {
            Entity box = AddBox(new Vector3(0f, 1f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            int before = ScriptSubscriptionRegistry.Count;

            WeakReference context = SubscribeFromCollectibleContext(box, out int subscribed);
            Assert.That(subscribed, Is.EqualTo(before + 1), "the script's listener before the unload");

            for (int i = 0; i < 20 && context.IsAlive; ++i)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
            Assert.That(ScriptSubscriptionRegistry.Count, Is.EqualTo(before), "the engine ended the subscription");
            Assert.That(context.IsAlive, Is.False, "nothing roots the unloaded context");
        }

        private const int kRayCastCalls = 1_000_000;
        private const int kRayCastRounds = 7;

        private static double TimeRayCast()
        {
            int hits = 0;
            Vector3 forward = Vector3.UnitZ;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            for (int i = 0; i < kRayCastCalls; i++)
                hits += Physics.RayCast(Vector3.Zero, forward, out RayHit _) ? 1 : 0;
            watch.Stop();
            Assert.That(hits, Is.EqualTo(kRayCastCalls));
            return watch.Elapsed.TotalMilliseconds * 1e6 / kRayCastCalls;
        }

        /// <summary>
        /// One million ray casts through the generated binding, each a hit on a box in front of the origin, so every
        /// call runs the full query. Run it from a Release build. Prints nanoseconds per call for each round and the
        /// median.
        /// </summary>
        [Test, Explicit("Micro-benchmark: Physics.RayCast through the generated binding")]
        public void Bench_RayCast()
        {
            AddBox(new Vector3(0f, 0f, 10f), Vector3.One, BoxKind.Static);
            TimeRayCast();
            var rounds = new double[kRayCastRounds];
            for (int round = 0; round < kRayCastRounds; round++)
            {
                rounds[round] = TimeRayCast();
                TestContext.WriteLine($"RayCast round {round}: {rounds[round]:F2} ns/call");
            }
            Array.Sort(rounds);
            TestContext.WriteLine($"RayCast median of {kRayCastRounds} x {kRayCastCalls}: {rounds[kRayCastRounds / 2]:F2} ns/call");
        }

        private const int kRayCastAllCalls = 1_000_000;
        private const float kBoxDepth = 1f;

        // Every body along the ray with one RayCastAll.
        private static double TimeRayCastAll(RayHit[] buffer)
        {
            int hits = 0;
            Vector3 forward = Vector3.UnitZ;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            for (int i = 0; i < kRayCastAllCalls; i++)
                hits += Physics.RayCastAll(Vector3.Zero, forward, buffer, maxDistance: 50f);
            watch.Stop();
            Assert.That(hits, Is.EqualTo(3 * kRayCastAllCalls));
            return watch.Elapsed.TotalMilliseconds * 1e6 / kRayCastAllCalls;
        }

        // Every body along the ray with RayCast in a loop: each cast starts past the box the last one hit.
        private static double TimeRayCastLoop(RayHit[] buffer)
        {
            int hits = 0;
            Vector3 forward = Vector3.UnitZ;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            for (int i = 0; i < kRayCastAllCalls; i++)
            {
                Vector3 origin = Vector3.Zero;
                float remaining = 50f;
                int count = 0;
                while (count < buffer.Length && Physics.RayCast(origin, forward, out RayHit hit, remaining))
                {
                    buffer[count++] = hit;
                    float step = hit.Distance + kBoxDepth + 0.01f;
                    origin += forward * step;
                    remaining -= step;
                }
                hits += count;
            }
            watch.Stop();
            Assert.That(hits, Is.EqualTo(3 * kRayCastAllCalls));
            return watch.Elapsed.TotalMilliseconds * 1e6 / kRayCastAllCalls;
        }

        /// <summary>
        /// One million queries for every body along a ray through three boxes: RayCastAll against a loop of RayCast
        /// calls that finds the same three bodies (four casts, the last a miss), in interleaved rounds. Run it from a
        /// Release build. Prints nanoseconds per query for each round and the medians.
        /// </summary>
        [Test, Explicit("Micro-benchmark: Physics.RayCastAll against a loop of Physics.RayCast")]
        public void Bench_RayCastAll()
        {
            for (int i = 0; i < 3; i++)
                AddBox(new Vector3(0f, 0f, 5f + 5f * i), new Vector3(1f, 1f, kBoxDepth / 2f), BoxKind.Static);
            var buffer = new RayHit[4];
            TimeRayCastAll(buffer);
            TimeRayCastLoop(buffer);
            var all = new double[kRayCastRounds];
            var loop = new double[kRayCastRounds];
            for (int round = 0; round < kRayCastRounds; round++)
            {
                all[round] = TimeRayCastAll(buffer);
                loop[round] = TimeRayCastLoop(buffer);
                TestContext.WriteLine($"round {round}: RayCastAll {all[round]:F2} ns/query, RayCast loop {loop[round]:F2} ns/query");
            }
            Array.Sort(all);
            Array.Sort(loop);
            TestContext.WriteLine($"median of {kRayCastRounds} x {kRayCastAllCalls}: RayCastAll {all[kRayCastRounds / 2]:F2} ns/query, " +
                                  $"RayCast loop {loop[kRayCastRounds / 2]:F2} ns/query");
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static WeakReference SubscribeFromCollectibleContext(Entity box, out int subscribed)
        {
            var context = new AssemblyLoadContext("PhysicsBindingTests.Scripts", isCollectible: true);
            Assembly scripts = context.LoadFromAssemblyPath(typeof(PhysicsBindingTests).Assembly.Location);
            Type subscriber = scripts.GetType(typeof(CollectibleSubscriber).FullName!, throwOnError: true)!;
            subscriber.GetMethod(nameof(CollectibleSubscriber.Subscribe))!.Invoke(null, new object[] { box.Id });
            subscribed = ScriptSubscriptionRegistry.Count;
            context.Unload();
            return new WeakReference(context);
        }
    }

    /// <summary>Loaded into a collectible context by the test above, as a hot-reloaded script is.</summary>
    public static class CollectibleSubscriber
    {
        /// <summary>Subscribes a handler of this assembly to the entity's ContactBegin.</summary>
        public static void Subscribe(uint entity) => new Entity(entity).Physics.ContactBegin += OnContact;

        private static void OnContact(in ContactEvent e)
        {
        }
    }
}
