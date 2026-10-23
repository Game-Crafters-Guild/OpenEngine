using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// A curve of up to 32 keys, sorted by time, as a component field holds it: the engine's <c>Math::Curve</c>
    /// layout, 644 bytes (the key count, 3 bytes of padding, then the keys).
    /// </summary>
    [StructLayout(LayoutKind.Explicit, Size = 644)]
    public struct Curve
    {
        /// <summary>The most keys a curve holds.</summary>
        public const int Capacity = 32;

        /// <summary>The keys of a curve: the first <see cref="KeyCount"/> are in use.</summary>
        [InlineArray(Capacity)]
        public struct KeysArray
        {
            private CurveKey m_Element;
        }

        /// <summary>How many of <see cref="Keys"/> are in use, 0 to <see cref="Capacity"/>.</summary>
        [FieldOffset(0)] public byte KeyCount;

        /// <summary>The keys, sorted by time.</summary>
        [FieldOffset(4)] public KeysArray Keys;
    }
}
