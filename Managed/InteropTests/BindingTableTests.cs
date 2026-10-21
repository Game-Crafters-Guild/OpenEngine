using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// A generated module class binds its table only when the table was built from the same description: the hash in
    /// the table header, the format and the size must match the binding's, or Acquire throws with both hashes.
    /// </summary>
    public unsafe class BindingTableTests
    {
        private const ulong kServedHash = 0x1111222233334444ul;
        private static byte* s_Table;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int Serve(ulong hash, void** table, uint* size)
        {
            *table = s_Table;
            *size = (uint)(16 + sizeof(void*));
            return 0;
        }

        /// <summary>A table of one entry, with the served hash.</summary>
        [SetUp]
        public void WriteTable()
        {
            s_Table = (byte*)NativeMemory.AllocZeroed((nuint)(16 + sizeof(void*)));
            *(uint*)s_Table = (uint)(16 + sizeof(void*));
            *(uint*)(s_Table + 4) = 1;
            *(ulong*)(s_Table + 8) = kServedHash;
            *(nint*)(s_Table + 16) = 0x5150;
        }

        /// <summary>Frees the table.</summary>
        [TearDown]
        public void FreeTable()
        {
            NativeMemory.Free(s_Table);
            s_Table = null;
        }

        /// <summary>A table with another hash is refused, and the message names both hashes and the fix.</summary>
        [Test]
        public void TableWithAnotherHashIsRefused()
        {
            var error = Assert.Throws<InvalidOperationException>(() => BindingTable.Acquire("Probe", 0x9999ul, 1, &Serve));

            Assert.That(error!.Message, Does.Contain("hash 0x1111222233334444"));
            Assert.That(error.Message, Does.Contain("expects hash 0x0000000000009999"));
            Assert.That(error.Message, Does.Contain("If a *ScriptApi.h changed, build the UpdateEngineApi target and rebuild"));
        }

        /// <summary>A table with the binding's hash and function count is served, its entries in order.</summary>
        [Test]
        public void TableWithTheSameHashIsServed()
        {
            BindingTable table = BindingTable.Acquire("Probe", kServedHash, 1, &Serve);

            Assert.That((nint)table[0], Is.EqualTo((nint)0x5150));
        }
    }
}
