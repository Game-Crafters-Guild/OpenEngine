using System.Runtime.InteropServices;
using GameEngine.Scripting;

namespace WASDDemo
{
    /// <summary>
    /// The player's position in world units. WASDMovementSystem moves it and WASDDemoSetup copies it
    /// into the entity's Transform each frame.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Position : IComponent
    {
        public float X;
        public float Y;
        public float Z;

        public Position(float x, float y, float z)
        {
            X = x;
            Y = y;
            Z = z;
        }
    }
}
