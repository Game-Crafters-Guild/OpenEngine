using System;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// A reference to a prefab asset by its GUID, for a component field. Assign the prefab to the field in the
    /// Inspector; a component in a loaded scene that holds it keeps the prefab loaded, so
    /// <see cref="EntityCommands.Spawn"/> can spawn it. The same 16 bytes as the engine's PrefabRef.
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Size = 16)]
    public readonly struct PrefabRef : IEquatable<PrefabRef>
    {
        /// <summary>The GUID of the prefab asset.</summary>
        public readonly AssetGuid Guid;

        /// <summary>A reference to the prefab with the GUID <paramref name="guid"/>.</summary>
        public PrefabRef(AssetGuid guid)
        {
            Guid = guid;
        }

        /// <summary>No prefab.</summary>
        public static PrefabRef None => default;

        /// <summary>Whether this is <see cref="None"/>.</summary>
        public bool IsNone => Guid.IsNone;

        public bool Equals(PrefabRef other) => Guid == other.Guid;
        public override bool Equals(object? obj) => obj is PrefabRef other && Equals(other);
        public override int GetHashCode() => Guid.GetHashCode();
        public override string ToString() => Guid.ToString();
        public static bool operator ==(PrefabRef a, PrefabRef b) => a.Equals(b);
        public static bool operator !=(PrefabRef a, PrefabRef b) => !a.Equals(b);
    }
}
