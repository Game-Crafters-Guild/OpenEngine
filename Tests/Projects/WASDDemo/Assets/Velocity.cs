using System.Runtime.InteropServices;
using GameEngine.Scripting;

namespace WASDDemo
{
    /// <summary>
    /// The player's movement. WASDMovementSystem reads X as the speed in world units per second;
    /// Y and Z are unused.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Velocity : IComponent
    {
        public float X;
        public float Y;
        public float Z;

        public Velocity(float x, float y, float z)
        {
            X = x;
            Y = y;
            Z = z;
        }
    }
}
