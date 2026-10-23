using System.Buffers;
using System.Text;

namespace GameEngine.Scripting
{
    /// <summary>
    /// The text of a generated call. Text crosses as UTF-8 with a length, never NUL-terminated. A string argument is
    /// encoded into a stack buffer of the call, or a pooled array when it is longer, and the engine reads it for the
    /// call only. A returned string is copied by the engine into a buffer the call owns: the engine copies what fits
    /// and always reports the full length, and a text that did not fit is asked again with a pooled buffer of that
    /// length. The engine allocates nothing the call frees; the one managed allocation is the returned string.
    /// </summary>
    internal static class ScriptText
    {
        /// <summary>The size of the stack buffer every text of a call starts with, in bytes.</summary>
        public const int StackBytes = 256;

        /// <summary>A string argument as UTF-8 bytes for one call. Dispose returns a pooled array.</summary>
        public ref struct In
        {
            private byte[]? m_Rented;

            /// <summary>Encodes <paramref name="text"/> into <paramref name="stack"/>, or a pooled array when it does not fit.</summary>
            public In(string text, Span<byte> stack)
            {
                m_Rented = null;
                Span<byte> buffer = stack;
                int length = Encoding.UTF8.GetByteCount(text);
                if (length > stack.Length)
                {
                    m_Rented = ArrayPool<byte>.Shared.Rent(length);
                    buffer = m_Rented;
                }
                Bytes = buffer.Slice(0, Encoding.UTF8.GetBytes(text, buffer));
            }

            /// <summary>The UTF-8 bytes of the text; empty for an empty string.</summary>
            public Span<byte> Bytes { get; }

            /// <summary>Returns the pooled array, if the text needed one.</summary>
            public void Dispose()
            {
                if (m_Rented != null)
                    ArrayPool<byte>.Shared.Return(m_Rented);
                m_Rented = null;
            }
        }

        /// <summary>The buffer a returned string is copied into. Dispose returns a pooled array.</summary>
        public ref struct Out
        {
            private byte[]? m_Rented;

            /// <summary>Starts with <paramref name="stack"/> as the buffer.</summary>
            public Out(Span<byte> stack)
            {
                m_Rented = null;
                Bytes = stack;
            }

            /// <summary>The buffer the next call writes to.</summary>
            public Span<byte> Bytes { get; private set; }

            /// <summary>
            /// The text the engine wrote, when its full <paramref name="length"/> fit the buffer. When it did not, the
            /// buffer becomes a pooled array of that length and the call is made again.
            /// </summary>
            public bool TryRead(int length, out string text)
            {
                if (length <= Bytes.Length)
                {
                    text = length <= 0 ? string.Empty : Encoding.UTF8.GetString(Bytes.Slice(0, length));
                    return true;
                }
                Dispose();
                m_Rented = ArrayPool<byte>.Shared.Rent(length);
                Bytes = m_Rented;
                text = string.Empty;
                return false;
            }

            /// <summary>Returns the pooled array, if the text needed one.</summary>
            public void Dispose()
            {
                if (m_Rented != null)
                    ArrayPool<byte>.Shared.Return(m_Rented);
                m_Rented = null;
            }
        }
    }
}
