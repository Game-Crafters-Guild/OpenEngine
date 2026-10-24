using System.Buffers.Binary;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace GameEngine.ECS
{
    /// <summary>
    /// The engine's default value of every component: the bytes of the default-constructed C++ struct, copied in one
    /// call when the managed side binds to the engine (Native.EnsureLoaded). The constructors of the generated
    /// mirrors read it through <c>ComponentType&lt;T&gt;.Default</c>. In a process with no engine it stays empty.
    /// </summary>
    internal static class ComponentDefaults
    {
        private const int kRecordHeaderBytes = sizeof(ulong) + sizeof(uint);

        private static Dictionary<ulong, byte[]> s_Table = new();

        /// <summary>Replaces the table with the engine's records ({typeId, size, bytes} back to back).</summary>
        internal static void Publish(byte[] records)
        {
            var table = new Dictionary<ulong, byte[]>();
            ReadOnlySpan<byte> rest = records;
            while (rest.Length >= kRecordHeaderBytes)
            {
                ulong typeId = BinaryPrimitives.ReadUInt64LittleEndian(rest);
                int size = (int)BinaryPrimitives.ReadUInt32LittleEndian(rest.Slice(sizeof(ulong)));
                rest = rest.Slice(kRecordHeaderBytes);
                if (size > rest.Length)
                    break;
                table[typeId] = rest.Slice(0, size).ToArray();
                rest = rest.Slice(size);
            }
            Volatile.Write(ref s_Table, table);
        }

        /// <summary>
        /// The engine's default of the component registered as <paramref name="nativeName"/>, read as
        /// <typeparamref name="T"/>. Throws with the fix when no engine runs in this process, or when the engine
        /// has no default of that name and of the size of <typeparamref name="T"/>.
        /// </summary>
        internal static T Read<T>(string nativeName) where T : unmanaged
        {
            try
            {
                Native.EnsureLoaded();
            }
            catch (Exception ex) when (ex is DllNotFoundException or MissingMethodException or InvalidOperationException
                                           or EntryPointNotFoundException or BadImageFormatException)
            {
                throw new InvalidOperationException(
                    $"{typeof(T).Name} takes its defaults from the engine, which is not running in this process. Create " +
                    "it in a system or after the engine starts, or start from `default` and set every field.", ex);
            }
            if (Native.TryGetComponentTypeIdByName(nativeName, out ulong typeId) == 0 && typeId != 0 &&
                Volatile.Read(ref s_Table).TryGetValue(typeId, out byte[]? bytes) && bytes.Length == Unsafe.SizeOf<T>())
                return MemoryMarshal.Read<T>(bytes);
            throw new InvalidOperationException(
                $"{typeof(T).Name} takes its defaults from the engine, which has no default of {Unsafe.SizeOf<T>()} bytes " +
                $"for '{nativeName}'. Build the engine and the managed assemblies from one source tree, or start from " +
                "`default` and set every field.");
        }
    }
}
