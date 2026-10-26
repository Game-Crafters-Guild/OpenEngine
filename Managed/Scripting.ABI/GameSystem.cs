namespace GameEngine.Scripting
{
    /// <summary>
    /// Base class for global per-frame systems (spawners, managers, state machines).
    /// Not source-generated. Commands spawns prefabs and queues structural changes; World reads and writes at once.
    /// </summary>
    public abstract class GameSystem
    {
        /// <summary>The primary ECS world.</summary>
        protected internal GameEngine.ECS.WorldHandle World { get; internal set; }

        /// <summary>
        /// Commands on this system's world. Spawn and CreateEntity act at once; the other commands run after all
        /// systems complete.
        /// </summary>
        /// <exception cref="InvalidOperationException">The system has no world.</exception>
        protected EntityCommands Commands
        {
            get
            {
                if (World.Handle == 0)
                    throw new InvalidOperationException("This GameSystem has no world. The runner creates each GameSystem " +
                        "and gives it its world before OnCreate; do not create a GameSystem with new.");
                return new EntityCommands(World.Handle);
            }
        }

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
