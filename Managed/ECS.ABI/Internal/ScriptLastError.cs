using System.Runtime.InteropServices;
using System.Text;
using GameEngine.Interop;

namespace GameEngine.ECS.Internal
{
    /// <summary>
    /// The engine's last error text for the calling thread (GE_GetLastScriptError): what a refused or failed script
    /// call reports, such as a component write the engine refuses. The generated bindings read it through
    /// BindingErrors in GameEngine.Scripting.Bindings.
    /// </summary>
    internal static unsafe class ScriptLastError
    {
        private const int kStackBytes = 256;

        /// <summary>The engine's last error text for the calling thread, or empty when the engine has none.</summary>
        public static string Read()
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
