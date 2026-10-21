using GameEngine.Interop;

namespace GameEngine.Scripting
{
    /// <summary>
    /// A 64-bit id made from a name: the FNV-1a hash of the name's UTF-8 bytes, which native code computes for the
    /// same name (Types/StringId.h). Make it once, into a <c>static readonly</c> field, and pass the field: hashing
    /// costs tens of nanoseconds a name, several times a script API call.
    /// </summary>
    /// <param name="Value">The hash. 0 names no id, and a script API call refuses it.</param>
    public readonly record struct StringId(ulong Value)
    {
        /// <summary>The id of <paramref name="name"/>.</summary>
        public static StringId Of(string name) => new(StringIds.Hash(name));
    }
}
