using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using GameEngine.Interop;

namespace GameEngine.Scripting
{
    /// <summary>
    /// What a generated call does with a result other than GE_Result_Ok. NotInitialized (no engine, or no system for
    /// the call, such as input before play) is an answer, not an error: the out values hold their defaults and a
    /// setter does nothing, so a generated call returns without calling here. InvalidArg throws ArgumentException;
    /// every other code throws EngineCallException. Both carry the engine's last error for the calling thread
    /// (GE_GetLastScriptError).
    /// </summary>
    internal static unsafe class BindingErrors
    {
        /// <summary>GE_Result_NotInitialized.</summary>
        public const int NotInitialized = -4;

        private const int kInvalidArg = -2;
        private const int kStackBytes = 256;

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Throw(int code, string function)
        {
            string reason = LastError();
            if (code == kInvalidArg)
                throw new ArgumentException(reason.Length != 0 ? reason : $"{function}: the engine refused an argument");
            throw new EngineCallException(code, function, reason);
        }

        /// <summary>The engine's last error text for the calling thread, or empty when the engine has none.</summary>
        public static string LastError()
        {
            delegate* unmanaged[Cdecl]<byte*, int, int*, int> getLastError = LastErrorExport.Pointer;
            if (getLastError == null)
                return string.Empty;
            byte* buffer = stackalloc byte[kStackBytes];
            int length = 0;
            if (getLastError(buffer, kStackBytes, &length) != 0 || length <= 0)
                return string.Empty;
            if (length <= kStackBytes)
                return Encoding.UTF8.GetString(buffer, length);
            byte[] heap = new byte[length];
            fixed (byte* text = heap)
            {
                if (getLastError(text, length, &length) != 0)
                    return string.Empty;
            }
            return Encoding.UTF8.GetString(heap, 0, Math.Min(length, heap.Length));
        }

        // Resolved on the first error, never on a call that succeeds.
        private static class LastErrorExport
        {
            public static readonly delegate* unmanaged[Cdecl]<byte*, int, int*, int> Pointer = Resolve();

            private static delegate* unmanaged[Cdecl]<byte*, int, int*, int> Resolve()
            {
                try
                {
                    nint library = NativeEngineLibrary.Load(null, out _);
                    return NativeLibrary.TryGetExport(library, "GE_GetLastScriptError", out nint export)
                        ? (delegate* unmanaged[Cdecl]<byte*, int, int*, int>)export
                        : null;
                }
                catch (DllNotFoundException)
                {
                    return null;
                }
            }
        }
    }
}
