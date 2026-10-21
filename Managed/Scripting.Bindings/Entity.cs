namespace GameEngine
{
    /// <summary>
    /// An entity of the scene: the engine's 32-bit entity id, which packs the entity's index and a version that
    /// changes when the index is reused, so a stale entity names no entity. Each script API module adds its view of
    /// an entity as a property (<c>ball.Physics</c>).
    /// </summary>
    /// <remarks>
    /// <see cref="None"/> is the entity that names no entity. A <c>default(Entity)</c> is the entity with id 0, which
    /// can be a real entity: use <see cref="None"/> for "no entity".
    /// </remarks>
    public readonly partial struct Entity : IEquatable<Entity>
    {
        private const uint kNoEntity = 0xFFFFFFFFu;

        /// <summary>The entity that names no entity.</summary>
        public static Entity None => new Entity(kNoEntity);

        /// <summary>Wraps the engine's packed entity id.</summary>
        public Entity(uint id)
        {
            Id = id;
        }

        /// <summary>The engine's packed entity id (index and version).</summary>
        public uint Id { get; }

        /// <summary>False for <see cref="None"/>. True for every other id, also for an entity that no longer exists.</summary>
        public bool IsValid => Id != kNoEntity;

        /// <inheritdoc/>
        public bool Equals(Entity other) => Id == other.Id;

        /// <inheritdoc/>
        public override bool Equals(object? obj) => obj is Entity other && Equals(other);

        /// <inheritdoc/>
        public override int GetHashCode() => (int)Id;

        /// <inheritdoc/>
        public override string ToString() => IsValid ? $"Entity({Id})" : "Entity.None";

        /// <summary>True when both name the same entity id.</summary>
        public static bool operator ==(Entity left, Entity right) => left.Id == right.Id;

        /// <summary>True when the two name different entity ids.</summary>
        public static bool operator !=(Entity left, Entity right) => left.Id != right.Id;
    }
}
