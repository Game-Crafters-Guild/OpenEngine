using System.Numerics;

namespace GameEngine.Scripting
{
    /// <summary>
    /// The accessors <see cref="Transform"/> and <see cref="WorldTransform"/> share over the engine's 4x4 matrix: 16
    /// floats in column-major order, columns 0 to 2 the X, Y and Z axes, column 3 the translation. Read as a
    /// System.Numerics.Matrix4x4 the same 16 floats are its rows, with the translation in M41 to M43.
    /// </summary>
    internal static class ColumnMajorMatrix
    {
        private const int kTranslationColumn = 3;

        /// <summary>The translation.</summary>
        public static Vector3 Position(ReadOnlySpan<float> m) => Column(m, kTranslationColumn);

        /// <summary>Axis <paramref name="column"/> (0 X, 1 Y, 2 Z), normalized.</summary>
        public static Vector3 Axis(ReadOnlySpan<float> m, int column) => Vector3.Normalize(Column(m, column));

        /// <summary>The length of axis <paramref name="column"/>: the scale on that axis.</summary>
        public static float AxisLength(ReadOnlySpan<float> m, int column) => Column(m, column).Length();

        /// <summary>The matrix as a System.Numerics.Matrix4x4: the 16 floats in order.</summary>
        public static Matrix4x4 ToMatrix4x4(ReadOnlySpan<float> m) => new(
            m[0], m[1], m[2], m[3],
            m[4], m[5], m[6], m[7],
            m[8], m[9], m[10], m[11],
            m[12], m[13], m[14], m[15]);

        private static Vector3 Column(ReadOnlySpan<float> m, int column) => new(m[column * 4], m[column * 4 + 1], m[column * 4 + 2]);
    }
}
