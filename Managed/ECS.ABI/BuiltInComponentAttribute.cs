using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Marks a C# struct as the mirror of a C++ engine component. The struct resolves to the native
    /// component by its registered name, and a system iterates it over the native chunks.
    /// </summary>
    [AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
    public sealed class BuiltInComponentAttribute : Attribute
    {
        /// <summary>
        /// The C++ component's registered name, with its namespace
        /// ("GameEngine::Components::MeshRenderer").
        /// </summary>
        public string NativeName { get; }

        /// <summary>Marks the struct as the mirror of the engine component <paramref name="nativeName"/>.</summary>
        public BuiltInComponentAttribute(string nativeName)
        {
            NativeName = nativeName ?? throw new ArgumentNullException(nameof(nativeName));
        }
    }
}
