using System.Runtime.InteropServices;
using GameEngine.Interop;

namespace GameEngine.Scripting
{
    /// <summary>
    /// A module's native function table (GE_&lt;Module&gt;_Table_v1, from GE_&lt;Module&gt;_GetInterface): a 16-byte
    /// header (the size in bytes, the description format, the module hash) and one function pointer per function in
    /// the order of the description. Each generated module class acquires its table once, in its static
    /// initializer, and reads its function pointers from it into static fields.
    /// </summary>
    internal readonly unsafe struct BindingTable
    {
        private const int kHeaderBytes = 16;
        private const string kFix =
            "If a *ScriptApi.h changed, build the UpdateEngineApi target and rebuild; otherwise rebuild the engine and " +
            "the managed assemblies from one source tree.";
        private const uint kFormat = 1;

        private readonly void** m_Entries;

        private BindingTable(void** entries)
        {
            m_Entries = entries;
        }

        /// <summary>The function pointer at <paramref name="index"/>, in the order of the description.</summary>
        public void* this[int index] => m_Entries[index];

        /// <summary>
        /// The table of <paramref name="module"/> from GameEngine.Native. Throws when the library has no table for the
        /// module, or serves one built from a description with another hash or another number of functions.
        /// </summary>
        public static BindingTable Acquire(string module, ulong hash, int functionCount)
        {
            nint library = NativeEngineLibrary.Load(null, out string loadedFrom);
            string export = "GE_" + module + "_GetInterface";
            if (!NativeLibrary.TryGetExport(library, export, out nint getInterface) || getInterface == 0)
                throw new InvalidOperationException(
                    $"{loadedFrom} has no {export}, so the {module} script API cannot be bound. " +
                    kFix);
            return Acquire(module, hash, functionCount, (delegate* unmanaged[Cdecl]<ulong, void**, uint*, int>)getInterface);
        }

        /// <summary>
        /// The table <paramref name="getInterface"/> serves for <paramref name="hash"/>, checked against the
        /// header and the function count. Throws with both hashes when the native side refuses the hash.
        /// </summary>
        internal static BindingTable Acquire(string module, ulong hash, int functionCount,
                                             delegate* unmanaged[Cdecl]<ulong, void**, uint*, int> getInterface)
        {
            void* table = null;
            uint size = 0;
            int code = getInterface(hash, &table, &size);
            if (code != 0 || table == null)
            {
                string reason = BindingErrors.LastError();
                throw new InvalidOperationException(reason.Length != 0
                    ? reason
                    : $"The engine refused the {module} script API table for hash 0x{hash:x16} (result {code}). " +
                      kFix);
            }

            byte* header = (byte*)table;
            uint sizeBytes = *(uint*)header;
            uint format = *(uint*)(header + 4);
            ulong tableHash = *(ulong*)(header + 8);
            uint expectedSize = (uint)(kHeaderBytes + functionCount * sizeof(void*));
            if (tableHash != hash || format != kFormat || sizeBytes != expectedSize || size != expectedSize)
                throw new InvalidOperationException(
                    $"The {module} script API table does not match this binding: hash 0x{tableHash:x16}, format {format}, " +
                    $"{sizeBytes} bytes; this binding expects hash 0x{hash:x16}, format {kFormat}, {expectedSize} bytes. " +
                    kFix);
            return new BindingTable((void**)(header + kHeaderBytes));
        }
    }
}
