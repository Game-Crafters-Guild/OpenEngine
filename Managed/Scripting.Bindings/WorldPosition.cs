using System.Numerics;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// An exact world position in meters, three doubles. The engine's <c>Mathematics::WorldPosition</c>
    /// layout. It has no implicit conversion from <see cref="Vector3"/>: use <see cref="FromVector3"/>.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct WorldPosition
    {
        /// <summary>Meters along the X axis.</summary>
        public double X;

        /// <summary>Meters along the Y axis.</summary>
        public double Y;

        /// <summary>Meters along the Z axis.</summary>
        public double Z;

        /// <summary>The same point as a world position. Exact: a float widens to a double.</summary>
        public static WorldPosition FromVector3(Vector3 meters) =>
            new WorldPosition { X = meters.X, Y = meters.Y, Z = meters.Z };

        /// <summary>The offset from <paramref name="b"/> to <paramref name="a"/>: subtracts in double and rounds once.</summary>
        public static Vector3 operator -(WorldPosition a, WorldPosition b) =>
            new Vector3((float)(a.X - b.X), (float)(a.Y - b.Y), (float)(a.Z - b.Z));

        /// <summary>The position moved by <paramref name="offset"/>, added in double.</summary>
        public static WorldPosition operator +(WorldPosition position, Vector3 offset) =>
            new WorldPosition { X = position.X + offset.X, Y = position.Y + offset.Y, Z = position.Z + offset.Z };
    }
}
