using System.Runtime.CompilerServices;
using GameEngine.ECS;
using GameEngine.ECS.Internal;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Buffers structural ECS operations for deferred execution.
    /// Pass as a parameter to IEntitySystem.Execute.
    /// Zero managed heap allocations — all data passed via stack pinning.
    /// </summary>
    public unsafe struct EntityCommands
    {
        internal ulong WorldHandle;

        /// <summary>Creates a new empty entity (no components). Returns a valid entity ID immediately.</summary>
        public uint CreateEntity()
        {
            return ChunkQueryNative.CreateEntityRaw(WorldHandle);
        }

        /// <summary>Queues entity destruction. Executes after all systems complete.</summary>
        public void DestroyEntity(uint entityId)
        {
            ChunkQueryNative.DeferCommand(WorldHandle, kDeferDestroyEntity, entityId, 0, null, 0);
        }

        /// <summary>
        /// Queues adding a component. Executes after all systems complete. For an engine component, the engine keeps
        /// its own value of every field the mirror does not let a script write, and a component the engine owns
        /// throws <see cref="InvalidOperationException"/> here, as <c>ComponentType&lt;T&gt;.Set</c> does.
        /// </summary>
        public void AddComponent<T>(uint entityId, in T component) where T : unmanaged, IComponent
        {
            QueueWrite(kDeferAddComponent, entityId, in component);
        }

        /// <summary>Queues removing a component. Executes after all systems complete.</summary>
        public void RemoveComponent<T>(uint entityId) where T : unmanaged, IComponent
        {
            ChunkQueryNative.DeferCommand(WorldHandle, kDeferRemoveComponent, entityId,
                ComponentType<T>.CachedId, null, 0);
        }

        /// <summary>
        /// Queues switching the entity on or off. Executes after all systems complete. A disabled
        /// entity — and every entity under it in the hierarchy — is skipped by every system; an
        /// entity switched back on is visited again at once, unless an entity above it in the
        /// hierarchy is still off.
        /// </summary>
        public void SetEnabled(uint entityId, bool enabled)
        {
            ChunkQueryNative.SetEntityEnabled(WorldHandle, entityId, enabled);
        }

        /// <summary>
        /// Queues switching component T on or off on the entity without removing its data.
        /// Executes after all systems complete. Systems skip a disabled T as if it were absent.
        /// Throws <see cref="InvalidOperationException"/> for a component with no on/off state
        /// (a built-in such as Transform); switch the entity off instead.
        /// </summary>
        public void SetEnabled<T>(uint entityId, bool enabled) where T : unmanaged, IComponent
        {
            ulong disabledTypeId = ComponentType<T>.DisabledTypeId;
            if (disabledTypeId == 0)
                throw new InvalidOperationException(
                    $"Component '{typeof(T).Name}' is not toggleable: it has no on/off state and is always on. " +
                    "Switch the entity off with SetEnabled(entityId, false) instead.");
            QueueTag(entityId, disabledTypeId, !enabled);
        }

        private void QueueTag(uint entityId, ulong tagTypeId, bool present)
        {
            if (present)
            {
                byte tag = 0;
                ChunkQueryNative.DeferCommand(WorldHandle, kDeferAddComponent, entityId, tagTypeId, &tag, 1);
            }
            else
            {
                ChunkQueryNative.DeferCommand(WorldHandle, kDeferRemoveComponent, entityId, tagTypeId, null, 0);
            }
        }

        /// <summary>
        /// Queues setting/overwriting a component value. Executes after all systems complete. For an engine component,
        /// the engine keeps its own value of every field the mirror does not let a script write, merged against the
        /// value at the flush, and a component the engine owns throws <see cref="InvalidOperationException"/> here.
        /// </summary>
        public void SetComponent<T>(uint entityId, in T component) where T : unmanaged, IComponent
        {
            QueueWrite(kDeferSetComponent, entityId, in component);
        }

        private const uint kDeferDestroyEntity = 1;
        private const uint kDeferAddComponent = 2;
        private const uint kDeferRemoveComponent = 3;
        private const uint kDeferSetComponent = 4;

        // A C# component queues its bytes as they are (kind 2 or 4); a mirror of an engine component queues a script
        // write, which adds or sets it, keeps the engine's fields and is refused at once for an engine-owned component.
        private void QueueWrite<T>(uint commandType, uint entityId, in T component) where T : unmanaged, IComponent
        {
            uint kind = ComponentType<T>.IsEngineComponent ? ChunkQueryNative.kDeferScriptWrite : commandType;
            fixed (T* ptr = &component)
            {
                ChunkQueryNative.DeferCommand(WorldHandle, kind, entityId, ComponentType<T>.CachedId, ptr, (uint)Unsafe.SizeOf<T>());
            }
        }
    }
}
