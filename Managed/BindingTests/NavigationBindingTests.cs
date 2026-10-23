using System.Numerics;
using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The generated Navigation class through the real GameEngine.Native: RequestBake and MarkDirty cross the
    /// table as members of an entity's view and reach the surface on that entity; SetDestination and GetStatus
    /// order an agent on a baked surface and follow its request. BindingTestScene (Tests/ScriptBindings) makes
    /// the surface and agent entities, reads back what the calls asked and runs the path solves.
    /// </summary>
    [NonParallelizable]
    public class NavigationBindingTests
    {
        private const string kScene = "BindingTestScene";

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_AddNavigationSurface(int kind, out uint entity);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_NavigationBakeRequested(uint entity);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_TakeNavigationDirtyBoxes(uint entity, float[] box);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_AddBakedNavigationGrid(out uint surface);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_AddNavigationAgent(uint surface, float x, float z, out uint agent);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_StepNavigation(int frames);

        // GE_TestScene_AddNavigationSurface's kind.
        private const int kMesh = 0;
        private const int kGrid = 1;
        private const int kNoSurface = 2;

        /// <summary>A generated call starts the standalone engine the scene needs.</summary>
        [OneTimeSetUp]
        public void StartEngine()
        {
            Assert.That(Entity.None.IsValid, Is.False);
            Assert.That(RenderLayers.NameOf(RenderLayerMask.Default), Is.EqualTo("Default"));
        }

        private static Entity AddSurface(int kind)
        {
            Assert.That(GE_TestScene_AddNavigationSurface(kind, out uint id), Is.EqualTo(0));
            return new Entity(id);
        }

        /// <summary>RequestBake asks the entity's surface to bake; an entity with no surface answers false.</summary>
        [Test]
        public void RequestBakeAsksTheSurfaceToBake()
        {
            Entity surface = AddSurface(kMesh);
            Assert.That(GE_TestScene_NavigationBakeRequested(surface.Id), Is.EqualTo(0));
            Assert.That(surface.Navigation.RequestBake(), Is.True);
            Assert.That(GE_TestScene_NavigationBakeRequested(surface.Id), Is.EqualTo(1));

            Assert.That(AddSurface(kNoSurface).Navigation.RequestBake(), Is.False);
        }

        /// <summary>MarkDirty queues its box for a mesh surface, in any corner order; a grid surface and an entity
        /// with no surface answer false.</summary>
        [Test]
        public void MarkDirtyQueuesTheBoxForAMeshSurface()
        {
            Entity surface = AddSurface(kMesh);
            Assert.That(surface.Navigation.MarkDirty(new Vector3(10f, 5f, -2f), new Vector3(-4f, 0f, 8f)), Is.True);
            var box = new float[4];
            Assert.That(GE_TestScene_TakeNavigationDirtyBoxes(surface.Id, box), Is.EqualTo(1));
            Assert.That(box, Is.EqualTo(new[] { -4f, -2f, 10f, 8f }));

            Assert.That(AddSurface(kGrid).Navigation.MarkDirty(Vector3.Zero, Vector3.One), Is.False);
            Assert.That(AddSurface(kNoSurface).Navigation.MarkDirty(Vector3.Zero, Vector3.One), Is.False);
        }

        /// <summary>SetDestination orders an agent on a baked surface; GetStatus reports Waiting until the solve,
        /// then Walking; the order of an agent whose Surface names no surface waits with NoSurface.</summary>
        [Test]
        public void SetDestinationOrdersAnAgentAndGetStatusFollowsTheOrder()
        {
            Assert.That(GE_TestScene_AddBakedNavigationGrid(out uint surface), Is.EqualTo(0));
            Assert.That(GE_TestScene_AddNavigationAgent(surface, 1.5f, 1.5f, out uint id), Is.EqualTo(0));
            var unit = new Entity(id);

            Assert.That(unit.Navigation.SetDestination(new Vector3(14.5f, 0f, 14.5f)), Is.True);
            Assert.That(unit.Navigation.GetStatus().State, Is.EqualTo(AgentState.Waiting));
            Assert.That(GE_TestScene_StepNavigation(1), Is.EqualTo(0));
            AgentStatus status = unit.Navigation.GetStatus();
            Assert.That(status.State, Is.EqualTo(AgentState.Walking));
            Assert.That(status.Reason, Is.EqualTo(StatusReason.None));
            Assert.That(status.PartialPath, Is.False);
            Assert.That(status.PendingTiles, Is.EqualTo(0u));

            Assert.That(GE_TestScene_AddNavigationSurface(kNoSurface, out uint nothing), Is.EqualTo(0));
            Assert.That(GE_TestScene_AddNavigationAgent(nothing, 1.5f, 1.5f, out uint strayId), Is.EqualTo(0));
            var stray = new Entity(strayId);
            Assert.That(stray.Navigation.SetDestination(new Vector3(14.5f, 0f, 14.5f)), Is.True);
            Assert.That(GE_TestScene_StepNavigation(1), Is.EqualTo(0));
            AgentStatus strayStatus = stray.Navigation.GetStatus();
            Assert.That(strayStatus.State, Is.EqualTo(AgentState.Waiting));
            Assert.That(strayStatus.Reason, Is.EqualTo(StatusReason.NoSurface));
        }
    }
}
