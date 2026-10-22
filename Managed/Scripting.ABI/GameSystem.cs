namespace GameEngine.Scripting
{
    /// <summary>
    /// Base class for global per-frame systems (spawners, managers, state machines).
    /// Not source-generated. Access ECS manually via World property if needed.
    /// </summary>
    public abstract class GameSystem
    {
        /// <summary>The primary ECS world.</summary>
        protected internal GameEngine.ECS.WorldHandle World { get; internal set; }

        /// <summary>Structural ECS commands for this system's world.</summary>
        /// <remarks>
        /// The hand-off a spawned visual needs: the world could always create an entity, and nothing a game
        /// writes could reach the commands to do it. Deferred like every other structural operation - the
        /// runner takes the fence around a tick and flushes when the systems are done - so a system never
        /// mutates the chunk it is iterating.
        /// </remarks>
        public EntityCommands Commands => EntityCommands.For(World);

        /// <summary>Enable/disable this system at runtime.</summary>
        public bool Enabled { get; set; } = true;

        /// <summary>Execution order. Lower runs first. Default 0.</summary>
        public virtual int Order => 0;

        /// <summary>Called once when the system is created (play mode enter).</summary>
        public virtual void OnCreate() { }

        /// <summary>Called when the system is enabled.</summary>
        public virtual void OnEnable() { }

        /// <summary>Called every frame while enabled.</summary>
        public virtual void OnUpdate(float deltaTime) { }

        /// <summary>Called when the system is disabled.</summary>
        public virtual void OnDisable() { }

        /// <summary>Called once when the system is destroyed (play mode exit).</summary>
        public virtual void OnDestroy() { }
    }
}
