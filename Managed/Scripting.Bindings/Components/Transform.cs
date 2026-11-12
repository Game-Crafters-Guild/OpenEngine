using System.Numerics;

namespace GameEngine.Scripting
{
    /// <summary>
    /// An entity's local transform, as the engine stores it: an exact <see cref="Position"/> in doubles and a
    /// column-major <see cref="Basis"/>, whose columns are the X, Y and Z axes.
    /// Left-handed, +Y up, +Z forward. System.Numerics.Matrix4x4 multiplies a row vector, so the basis columns are
    /// its rows: <see cref="ToMatrix4x4"/> and <see cref="FromMatrix4x4"/> copy them, and the translation is
    /// <c>Matrix4x4.Translation</c>, in floats.
    /// </summary>
    public partial struct Transform
    {
        /// <summary>The identity transform: at the origin, no rotation, scale 1.</summary>
        public static Transform Identity
        {
            get
            {
                Transform t = default;
                t.Basis.Column0 = Vector3.UnitX;
                t.Basis.Column1 = Vector3.UnitY;
                t.Basis.Column2 = Vector3.UnitZ;
                return t;
            }
        }

        /// <summary>
        /// A transform from an exact position, a rotation and a scale, as the engine's Transform::FromTRS builds it:
        /// <c>matrix * v == rotation.Rotate(v)</c>, so a +Y yaw sends +Z toward +X. Needs no engine.
        /// </summary>
        public static Transform FromTRS(WorldPosition position, Quaternion rotation, Vector3 scale)
        {
            float x2 = rotation.X + rotation.X, y2 = rotation.Y + rotation.Y, z2 = rotation.Z + rotation.Z;
            float xx = rotation.X * x2, xy = rotation.X * y2, xz = rotation.X * z2;
            float yy = rotation.Y * y2, yz = rotation.Y * z2, zz = rotation.Z * z2;
            float wx = rotation.W * x2, wy = rotation.W * y2, wz = rotation.W * z2;

            Transform t = default;
            t.Basis.Column0 = new Vector3(1f - (yy + zz), xy + wz, xz - wy) * scale.X;
            t.Basis.Column1 = new Vector3(xy - wz, 1f - (xx + zz), yz + wx) * scale.Y;
            t.Basis.Column2 = new Vector3(xz + wy, yz - wx, 1f - (xx + yy)) * scale.Z;
            t.Position = position;
            return t;
        }

        /// <summary>The rotation as a quaternion, with the scale divided out of the columns first, as the engine's Transform::GetRotation.</summary>
        public readonly Quaternion GetRotation()
        {
            Vector3 scale = GetScale();
            float sx = scale.X, sy = scale.Y, sz = scale.Z;

            if (sx <= 0f || sy <= 0f || sz <= 0f)
                return Quaternion.Identity;

            float invSx = 1f / sx, invSy = 1f / sy, invSz = 1f / sz;

            float r00 = Basis.Column0.X * invSx, r10 = Basis.Column0.Y * invSx, r20 = Basis.Column0.Z * invSx;
            float r01 = Basis.Column1.X * invSy, r11 = Basis.Column1.Y * invSy, r21 = Basis.Column1.Z * invSy;
            float r02 = Basis.Column2.X * invSz, r12 = Basis.Column2.Y * invSz, r22 = Basis.Column2.Z * invSz;

            float trace = r00 + r11 + r22;
            float qw, qx, qy, qz;

            if (trace > 0f)
            {
                float s = MathF.Sqrt(trace + 1f) * 2f;
                qw = 0.25f * s;
                qx = (r21 - r12) / s;
                qy = (r02 - r20) / s;
                qz = (r10 - r01) / s;
            }
            else if (r00 > r11 && r00 > r22)
            {
                float s = MathF.Sqrt(1f + r00 - r11 - r22) * 2f;
                qw = (r21 - r12) / s;
                qx = 0.25f * s;
                qy = (r01 + r10) / s;
                qz = (r02 + r20) / s;
            }
            else if (r11 > r22)
            {
                float s = MathF.Sqrt(1f + r11 - r00 - r22) * 2f;
                qw = (r02 - r20) / s;
                qx = (r01 + r10) / s;
                qy = 0.25f * s;
                qz = (r12 + r21) / s;
            }
            else
            {
                float s = MathF.Sqrt(1f + r22 - r00 - r11) * 2f;
                qw = (r10 - r01) / s;
                qx = (r02 + r20) / s;
                qy = (r12 + r21) / s;
                qz = 0.25f * s;
            }

            // The quaternion whose Rotate() matches this matrix (the FromTRS convention).
            return new Quaternion(qx, qy, qz, qw);
        }

        /// <summary>The scale on each axis: the lengths of the columns.</summary>
        public readonly Vector3 GetScale() => new(
            ColumnMajorMatrix.AxisLength(Basis, 0), ColumnMajorMatrix.AxisLength(Basis, 1), ColumnMajorMatrix.AxisLength(Basis, 2));

        /// <summary>The local +X axis, normalized.</summary>
        public readonly Vector3 Right => ColumnMajorMatrix.Axis(Basis, 0);

        /// <summary>The local -X axis, normalized.</summary>
        public readonly Vector3 Left => -Right;

        /// <summary>The local +Y axis, normalized.</summary>
        public readonly Vector3 Up => ColumnMajorMatrix.Axis(Basis, 1);

        /// <summary>The local -Y axis, normalized.</summary>
        public readonly Vector3 Down => -Up;

        /// <summary>The local +Z axis, normalized.</summary>
        public readonly Vector3 Forward => ColumnMajorMatrix.Axis(Basis, 2);

        /// <summary>The local -Z axis, normalized.</summary>
        public readonly Vector3 Back => -Forward;

        /// <summary>Resets the transform to the identity.</summary>
        public void SetIdentity() => this = Identity;

        /// <summary>Adds an offset to the position, in doubles.</summary>
        public void Translate(float x, float y, float z)
        {
            Position.X += x;
            Position.Y += y;
            Position.Z += z;
        }

        /// <summary>Adds an offset to the translation.</summary>
        public void Translate(Vector3 offset) => Translate(offset.X, offset.Y, offset.Z);

        /// <summary>Replaces the rotation (a quaternion, x y z w), keeping the position and the scale, as the engine's Transform::Rotate.</summary>
        public void Rotate(float x, float y, float z, float w) => Rotate(new Quaternion(x, y, z, w));

        /// <summary>Replaces the rotation, keeping the position and the scale.</summary>
        public void Rotate(Quaternion q) => this = FromTRS(Position, q, GetScale());

        /// <summary>Writes the diagonal scale values (replaces them, does not multiply), as the engine's Transform::SetScale.</summary>
        public void SetScale(float x, float y, float z)
        {
            Basis.Column0.X = x;
            Basis.Column1.Y = y;
            Basis.Column2.Z = z;
        }

        /// <summary>Writes the diagonal scale values.</summary>
        public void SetScale(Vector3 scale) => SetScale(scale.X, scale.Y, scale.Z);

        /// <summary>
        /// The transform as a System.Numerics.Matrix4x4, for <c>Vector3.Transform</c>, <c>Matrix4x4.Decompose</c> and
        /// the rest of System.Numerics: the translation is in M41 to M43, rounded to floats.
        /// </summary>
        public readonly Matrix4x4 ToMatrix4x4() => ColumnMajorMatrix.ToMatrix4x4(Basis, Position);

        /// <summary>
        /// Replaces the transform with an affine System.Numerics.Matrix4x4 (the translation in M41 to M43). A matrix
        /// whose fourth column (M14, M24, M34, M44) is more than 1e-5 from 0, 0, 0, 1 is projective and throws
        /// <see cref="System.ArgumentException"/>.
        /// </summary>
        public void FromMatrix4x4(Matrix4x4 m)
        {
            // A matrix read from a file carries rounding. C++ Transform::FromMatrix4x4 uses the same value.
            const float kAffineTolerance = 1e-5f;
            if (MathF.Abs(m.M14) > kAffineTolerance || MathF.Abs(m.M24) > kAffineTolerance ||
                MathF.Abs(m.M34) > kAffineTolerance || MathF.Abs(m.M44 - 1f) > kAffineTolerance)
                throw new System.ArgumentException(
                    $"Transform.FromMatrix4x4 takes an affine matrix, but M14, M24, M34, M44 are ({m.M14}, {m.M24}, {m.M34}, {m.M44}), " +
                    "not (0, 0, 0, 1). Build the transform with FromTRS, or apply the projection after the transform.",
                    nameof(m));
            Basis.Column0 = new Vector3(m.M11, m.M12, m.M13);
            Basis.Column1 = new Vector3(m.M21, m.M22, m.M23);
            Basis.Column2 = new Vector3(m.M31, m.M32, m.M33);
            Position = WorldPosition.FromVector3(m.Translation);
        }

        /// <summary>The transform as a System.Numerics.Matrix4x4.</summary>
        public static explicit operator Matrix4x4(Transform t) => t.ToMatrix4x4();

        /// <inheritdoc/>
        public override readonly string ToString()
        {
            WorldPosition p = Position;
            return $"Transform(pos=({p.X:F3}, {p.Y:F3}, {p.Z:F3}))";
        }
    }
}
