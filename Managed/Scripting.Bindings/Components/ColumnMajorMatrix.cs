using System.Numerics;

namespace GameEngine.Scripting
{
    /// <summary>
    /// The accessors <see cref="Transform"/> and <see cref="WorldTransform"/> share over the engine's pose: a
    /// position in doubles and a column-major 3x3 basis, columns 0 to 2 the X, Y and Z axes. Read as a
    /// System.Numerics.Matrix4x4 the columns are its rows, with the translation in M41 to M43.
    /// </summary>
    internal static class ColumnMajorMatrix
    {
        /// <summary>Axis <paramref name="column"/> (0 X, 1 Y, 2 Z), normalized.</summary>
        public static Vector3 Axis(in Matrix3x3 b, int column) => Vector3.Normalize(Column(b, column));

        /// <summary>The length of axis <paramref name="column"/>: the scale on that axis.</summary>
        public static float AxisLength(in Matrix3x3 b, int column) => Column(b, column).Length();

        /// <summary>The pose as a System.Numerics.Matrix4x4, with the position rounded to floats.</summary>
        public static Matrix4x4 ToMatrix4x4(in Matrix3x3 b, in WorldPosition p)
            => new(
                b.Column0.X, b.Column0.Y, b.Column0.Z, 0f,
                b.Column1.X, b.Column1.Y, b.Column1.Z, 0f,
                b.Column2.X, b.Column2.Y, b.Column2.Z, 0f,
                (float)p.X, (float)p.Y, (float)p.Z, 1f);

        private static Vector3 Column(in Matrix3x3 b, int column) => column switch
        {
            0 => b.Column0,
            1 => b.Column1,
            _ => b.Column2,
        };
    }
}
