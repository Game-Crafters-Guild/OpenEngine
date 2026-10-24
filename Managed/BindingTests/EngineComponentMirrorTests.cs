using GameEngine.ECS;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The generated mirrors of the engine's components through the real GameEngine.Native: <c>new T()</c> copies the
    /// engine's defaults, and <c>ComponentType&lt;T&gt;.Set</c> writes a mirror through the script entry, which
    /// refuses a component the engine owns.
    /// </summary>
    [NonParallelizable]
    public class EngineComponentMirrorTests
    {
        /// <summary><c>new MeshRenderer()</c> is <c>MeshRenderer{}</c> in C++: drawn, casting and receiving shadows.</summary>
        [Test]
        public void NewTakesTheEngineDefaults()
        {
            var renderer = new MeshRenderer();

            Assert.That(renderer.ShadowCasting, Is.EqualTo(ShadowCasting.On));
            Assert.That(renderer.ReceiveShadows, Is.True);
            Assert.That(renderer.MotionVectors, Is.True);
            Assert.That(default(MeshRenderer).ReceiveShadows, Is.False, "default(T) stays all zeros");
        }

        /// <summary>The Transform builders start from <c>default</c>, so they need no engine: a +Y yaw sends +Z toward +X.</summary>
        [Test]
        public void TransformBuildersNeedNoDefaults()
        {
            Assert.That(Transform.Identity.ToMatrix4x4(), Is.EqualTo(System.Numerics.Matrix4x4.Identity));

            var yaw = System.Numerics.Quaternion.CreateFromAxisAngle(System.Numerics.Vector3.UnitY, MathF.PI / 2f);
            Transform turned = Transform.FromTRS(new System.Numerics.Vector3(1f, 2f, 3f), yaw, System.Numerics.Vector3.One);
            Assert.That(turned.Forward.X, Is.EqualTo(1f).Within(1e-5f));
            Assert.That(turned.GetPosition(), Is.EqualTo(new System.Numerics.Vector3(1f, 2f, 3f)));
        }

        /// <summary>ToMatrix4x4 is a System.Numerics matrix: it moves and turns a point as Vector3.Transform expects.</summary>
        [Test]
        public void ToMatrix4x4WorksWithSystemNumerics()
        {
            var yaw = System.Numerics.Quaternion.CreateFromAxisAngle(System.Numerics.Vector3.UnitY, MathF.PI / 2f);
            var position = new System.Numerics.Vector3(1f, 2f, 3f);
            System.Numerics.Matrix4x4 matrix = Transform.FromTRS(position, yaw, System.Numerics.Vector3.One).ToMatrix4x4();

            Assert.That(matrix.Translation, Is.EqualTo(position));
            System.Numerics.Vector3 moved = System.Numerics.Vector3.Transform(System.Numerics.Vector3.UnitZ, matrix);
            Assert.That(System.Numerics.Vector3.Distance(moved, new System.Numerics.Vector3(2f, 2f, 3f)), Is.LessThan(1e-5f));

            Transform back = default;
            back.FromMatrix4x4(matrix);
            Assert.That(back.ToMatrix4x4(), Is.EqualTo(matrix));
        }

        /// <summary>SendEvent queues named events until the queue is full, then refuses.</summary>
        [Test]
        public void SendEventQueuesUntilTheQueueIsFull()
        {
            ParticlePlayback playback = default;
            int queued = 0;
            while (playback.SendEvent(new StringId(7)))
                queued++;
            Assert.That(queued, Is.EqualTo(8));
            Assert.That(playback.PendingEventCount, Is.EqualTo(8u));
            Assert.That(playback.PendingEvents[7], Is.EqualTo(new StringId(7)));
        }

        /// <summary>A script switches a proxy to Shadows Only; WorldTransform, which the engine writes, is refused.</summary>
        [Test]
        public void SetWritesAMirrorAndRefusesAComponentTheEngineOwns()
        {
            WorldHandle world = Ecs.PrimaryWorld;
            uint proxy = world.CreateEmptyEntity("EngineComponentMirrorTests.Proxy");
            try
            {
                var renderer = new MeshRenderer { ShadowCasting = ShadowCasting.ShadowsOnly };
                ComponentType<MeshRenderer>.Set(in world, proxy, in renderer);
                Assert.That(ComponentType<MeshRenderer>.Get(in world, proxy).ShadowCasting, Is.EqualTo(ShadowCasting.ShadowsOnly));

                WorldTransform placed = ComponentType<WorldTransform>.Get(in world, proxy);
                var refused = Assert.Throws<InvalidOperationException>(() => ComponentType<WorldTransform>.Set(in world, proxy, in placed));
                Assert.That(refused!.Message, Does.Contain("A script cannot write WorldTransform: it is owned by the engine and written by TransformHierarchySystem"));
            }
            finally
            {
                world.DestroyEntity(proxy);
            }
        }
    }
}
