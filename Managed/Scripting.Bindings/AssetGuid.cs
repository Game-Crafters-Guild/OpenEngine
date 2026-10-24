using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// The GUID of an asset: 16 bytes in the engine's text order, so byte 0 is the first two hex digits of
    /// "00112233-4455-6677-8899-aabbccddeeff". A component field that references an asset (a material, a
    /// model) holds one. Convert a <see cref="Guid"/> with <see cref="FromGuid"/>, never by its bytes:
    /// <see cref="Guid.ToByteArray()"/> writes the first three groups in the other order.
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Size = 16)]
    public readonly struct AssetGuid : IEquatable<AssetGuid>
    {
        [InlineArray(16)]
        private struct Bytes
        {
            private byte m_Element;
        }

        private readonly Bytes m_Bytes;

        private AssetGuid(ReadOnlySpan<byte> bytes)
        {
            Bytes copy = default;
            bytes.CopyTo(copy);
            m_Bytes = copy;
        }

        /// <summary>No asset: all 16 bytes zero.</summary>
        public static AssetGuid None => default;

        /// <summary>Whether this is <see cref="None"/>.</summary>
        public bool IsNone => ((ReadOnlySpan<byte>)m_Bytes).IndexOfAnyExcept((byte)0) < 0;

        /// <summary>The asset GUID with the same text as <paramref name="guid"/>.</summary>
        public static AssetGuid FromGuid(Guid guid)
        {
            Span<byte> bytes = stackalloc byte[16];
            guid.TryWriteBytes(bytes, bigEndian: true, out _);
            return new AssetGuid(bytes);
        }

        /// <summary>The <see cref="Guid"/> with the same text.</summary>
        public Guid ToGuid() => new(m_Bytes, bigEndian: true);

        /// <summary>The asset GUID of its text, "00112233-4455-6677-8899-aabbccddeeff" (the forms
        /// <see cref="Guid.Parse(string)"/> reads).</summary>
        public static AssetGuid Parse(string text) => FromGuid(Guid.Parse(text));

        /// <summary>The text the engine writes: 32 lower-case hex digits in groups of 8, 4, 4, 4 and 12.</summary>
        public override string ToString() => ToGuid().ToString("D");

        public bool Equals(AssetGuid other) => ((ReadOnlySpan<byte>)m_Bytes).SequenceEqual(other.m_Bytes);
        public override bool Equals(object? obj) => obj is AssetGuid other && Equals(other);
        public override int GetHashCode() => ToGuid().GetHashCode();
        public static bool operator ==(AssetGuid a, AssetGuid b) => a.Equals(b);
        public static bool operator !=(AssetGuid a, AssetGuid b) => !a.Equals(b);
    }
}
