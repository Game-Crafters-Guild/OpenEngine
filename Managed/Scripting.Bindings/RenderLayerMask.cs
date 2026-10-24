using System;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// The render layers a renderer belongs to, or the layers a camera or a volume takes: one bit per
    /// layer, 32 layers. Mirror of C++ <c>GameEngine::Rendering::RenderLayerMask</c> (one 32-bit word).
    /// A renderer is drawn by a camera when their layers intersect. To hide an object, turn its
    /// renderer off with <c>EntityCommands.SetEnabled</c>; an empty mask is drawn by no camera.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public readonly struct RenderLayerMask : IEquatable<RenderLayerMask>
    {
        /// <summary>Bit i is layer i.</summary>
        public readonly uint Bits;

        public RenderLayerMask(uint bits) => Bits = bits;

        /// <summary>Layer 0 only.</summary>
        public static RenderLayerMask Default => new(1u);

        /// <summary>Every layer.</summary>
        public static RenderLayerMask All => new(uint.MaxValue);

        /// <summary>No layer.</summary>
        public static RenderLayerMask None => new(0u);

        /// <summary>The mask of one layer, 0 to 31.</summary>
        public static RenderLayerMask FromLayer(int layer)
        {
            if ((uint)layer >= 32u)
                throw new ArgumentOutOfRangeException(nameof(layer), layer, "A render layer is 0 to 31.");
            return new RenderLayerMask(1u << layer);
        }

        /// <summary>Whether <paramref name="layer"/> (0 to 31) is set.</summary>
        public bool Contains(int layer) => (uint)layer < 32u && ((Bits >> layer) & 1u) != 0u;

        /// <summary>The layers of both masks.</summary>
        public static RenderLayerMask operator |(RenderLayerMask a, RenderLayerMask b) => new(a.Bits | b.Bits);

        /// <summary>Whether the two masks share a layer.</summary>
        public bool Intersects(RenderLayerMask other) => (Bits & other.Bits) != 0u;

        public bool Equals(RenderLayerMask other) => Bits == other.Bits;
        public override bool Equals(object? obj) => obj is RenderLayerMask other && Equals(other);
        public override int GetHashCode() => (int)Bits;
        public static bool operator ==(RenderLayerMask a, RenderLayerMask b) => a.Bits == b.Bits;
        public static bool operator !=(RenderLayerMask a, RenderLayerMask b) => a.Bits != b.Bits;
    }
}
