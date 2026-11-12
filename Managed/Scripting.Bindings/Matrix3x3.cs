using System.Numerics;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// A 3x3 matrix of floats in column-major order: three columns. The engine's
    /// <c>Mathematics::Matrix3x3</c> layout.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Matrix3x3
    {
        /// <summary>The first column.</summary>
        public Vector3 Column0;

        /// <summary>The second column.</summary>
        public Vector3 Column1;

        /// <summary>The third column.</summary>
        public Vector3 Column2;
    }
}
