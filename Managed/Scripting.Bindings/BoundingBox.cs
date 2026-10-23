using System.Numerics;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// A box aligned with the axes: its center and its half size on each axis. The engine's
    /// <c>Mathematics::BoundingBox</c> layout, two Vector3.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BoundingBox
    {
        /// <summary>The center of the box.</summary>
        public Vector3 Center;

        /// <summary>Half the box's size on each axis.</summary>
        public Vector3 HalfExtents;
    }
}
