using System.Numerics;

namespace GameEngine.Scripting
{
    /// <summary>
    /// An entity's world transform: a position in doubles, a column-major 3x3 basis and a version, which
    /// TransformHierarchySystem writes from the Transform and the parent chain. A script reads it;
    /// <c>ComponentType&lt;WorldTransform&gt;.Set</c> is refused.
    /// </summary>
    public readonly partial struct WorldTransform
    {
        /// <summary>The world matrix as a System.Numerics.Matrix4x4: the world position is in M41 to M43, rounded to floats.</summary>
        public Matrix4x4 ToMatrix4x4() => ColumnMajorMatrix.ToMatrix4x4(Basis, Position);

        /// <summary>The world matrix as a System.Numerics.Matrix4x4.</summary>
        public static explicit operator Matrix4x4(WorldTransform t) => t.ToMatrix4x4();

        /// <summary>The world-space +X axis, normalized.</summary>
        public Vector3 Right => ColumnMajorMatrix.Axis(Basis, 0);

        /// <summary>The world-space -X axis, normalized.</summary>
        public Vector3 Left => -Right;

        /// <summary>The world-space +Y axis, normalized.</summary>
        public Vector3 Up => ColumnMajorMatrix.Axis(Basis, 1);

        /// <summary>The world-space -Y axis, normalized.</summary>
        public Vector3 Down => -Up;

        /// <summary>The world-space +Z axis, normalized.</summary>
        public Vector3 Forward => ColumnMajorMatrix.Axis(Basis, 2);

        /// <summary>The world-space -Z axis, normalized.</summary>
        public Vector3 Back => -Forward;

        /// <inheritdoc/>
        public override string ToString()
        {
            WorldPosition p = Position;
            return $"WorldTransform(pos=({p.X:F3}, {p.Y:F3}, {p.Z:F3}), v={Version})";
        }
    }
}
