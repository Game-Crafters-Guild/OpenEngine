using System;
using System.Text;

namespace GameEngine.Scripting
{
    /// <summary>
    /// The text of a component mirror's <c>char name[N]</c> field: UTF-8 in N bytes, ended by a zero byte.
    /// </summary>
    internal static class ComponentText
    {
        /// <summary>The text up to the first zero byte, or all the bytes when there is none.</summary>
        public static string Read(ReadOnlySpan<byte> bytes)
        {
            int length = bytes.IndexOf((byte)0);
            return Encoding.UTF8.GetString(length < 0 ? bytes : bytes[..length]);
        }

        /// <summary>
        /// Writes <paramref name="value"/>, a zero byte and zeros to the end of <paramref name="bytes"/>. Text of
        /// more than <c>bytes.Length - 1</c> UTF-8 bytes is refused, and the field keeps its text.
        /// </summary>
        public static void Write(Span<byte> bytes, string value, string field)
        {
            ArgumentNullException.ThrowIfNull(value);
            int length = Encoding.UTF8.GetByteCount(value);
            if (length >= bytes.Length)
                throw new ArgumentException(
                    $"{field} holds at most {bytes.Length - 1} UTF-8 bytes; the text has {length}.", nameof(value));
            Encoding.UTF8.GetBytes(value, bytes);
            bytes[length..].Clear();
        }
    }
}
