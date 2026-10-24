using System.Numerics;

namespace GameEngine.Scripting
{
    /// <summary>
    /// An entity's local transform: a 4x4 matrix in column-major order, as the engine stores it. Columns 0 to 2 are
    /// the X, Y and Z axes, column 3 the translation (<c>Matrix[12]</c> to <c>Matrix[14]</c>). Left-handed, +Y up,
    /// +Z forward. System.Numerics.Matrix4x4 multiplies a row vector, so the same 16 floats read in order are its
    /// matrix: <see cref="ToMatrix4x4"/> and <see cref="FromMatrix4x4"/> copy them, and the translation is
    /// <c>Matrix4x4.Translation</c>.
    /// </summary>
    public partial struct Transform
    {
        /// <summary>The identity transform: at the origin, no rotation, scale 1.</summary>
        public static Transform Identity
        {
            get
            {
                Transform t = default;
                t.Matrix[0] = 1f;
                t.Matrix[5] = 1f;
                t.Matrix[10] = 1f;
                t.Matrix[15] = 1f;
                return t;
            }
        }

        /// <summary>
        /// A transform from position, rotation (a quaternion, x y z w) and scale, as the engine's
        /// Transform::FromTRS builds it: <c>matrix * v == rotation.Rotate(v)</c>, so a +Y yaw sends +Z toward +X.
        /// </summary>
        public static Transform FromTRS(float px, float py, float pz,
                                        float qx, float qy, float qz, float qw,
                                        float sx, float sy, float sz)
            => MakeTRS(px, py, pz, qx, qy, qz, qw, sx, sy, sz, leftHanded: true);

        /// <summary>A transform from position, rotation and scale (System.Numerics types), as <see cref="FromTRS(float, float, float, float, float, float, float, float, float, float)"/>.</summary>
        public static Transform FromTRS(Vector3 position, Quaternion rotation, Vector3 scale)
            => FromTRS(position.X, position.Y, position.Z,
                       rotation.X, rotation.Y, rotation.Z, rotation.W,
                       scale.X, scale.Y, scale.Z);

        /// <summary>
        /// The right-handed form, as the engine's Transform::FromTRSRH builds it: the transpose of the rotation of
        /// <see cref="FromTRS(float, float, float, float, float, float, float, float, float, float)"/>, so a +Y yaw
        /// sends +Z toward -X.
        /// </summary>
        public static Transform FromTRSRH(float px, float py, float pz,
                                          float qx, float qy, float qz, float qw,
                                          float sx, float sy, float sz)
            => MakeTRS(px, py, pz, qx, qy, qz, qw, sx, sy, sz, leftHanded: false);

        /// <summary>The right-handed form from System.Numerics types.</summary>
        public static Transform FromTRSRH(Vector3 position, Quaternion rotation, Vector3 scale)
            => FromTRSRH(position.X, position.Y, position.Z,
                         rotation.X, rotation.Y, rotation.Z, rotation.W,
                         scale.X, scale.Y, scale.Z);

        // Starts from `default` and writes all 16 elements, so it needs no engine.
        private static Transform MakeTRS(float px, float py, float pz,
                                         float qx, float qy, float qz, float qw,
                                         float sx, float sy, float sz, bool leftHanded)
        {
            float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
            float xx = qx * x2, xy = qx * y2, xz = qx * z2;
            float yy = qy * y2, yz = qy * z2, zz = qz * z2;
            float wx = qw * x2, wy = qw * y2, wz = qw * z2;

            float r00 = 1f - (yy + zz);
            float r11 = 1f - (xx + zz);
            float r22 = 1f - (xx + yy);
            float r01 = leftHanded ? (xy - wz) : (xy + wz);
            float r02 = leftHanded ? (xz + wy) : (xz - wy);
            float r10 = leftHanded ? (xy + wz) : (xy - wz);
            float r12 = leftHanded ? (yz - wx) : (yz + wx);
            float r20 = leftHanded ? (xz - wy) : (xz + wy);
            float r21 = leftHanded ? (yz + wx) : (yz - wx);

            Transform t = default;
            t.Matrix[0] = r00 * sx; t.Matrix[1] = r10 * sx; t.Matrix[2] = r20 * sx; t.Matrix[3] = 0f;
            t.Matrix[4] = r01 * sy; t.Matrix[5] = r11 * sy; t.Matrix[6] = r21 * sy; t.Matrix[7] = 0f;
            t.Matrix[8] = r02 * sz; t.Matrix[9] = r12 * sz; t.Matrix[10] = r22 * sz; t.Matrix[11] = 0f;
            t.Matrix[12] = px; t.Matrix[13] = py; t.Matrix[14] = pz; t.Matrix[15] = 1f;
            return t;
        }

        /// <summary>The translation (column 3).</summary>
        public readonly Vector3 GetPosition() => ColumnMajorMatrix.Position(Matrix);

        /// <summary>The X of the translation.</summary>
        public readonly float X => Matrix[12];

        /// <summary>The Y of the translation.</summary>
        public readonly float Y => Matrix[13];

        /// <summary>The Z of the translation.</summary>
        public readonly float Z => Matrix[14];

        /// <summary>The rotation as a quaternion, with the scale divided out of the columns first, as the engine's Transform::GetRotation.</summary>
        public readonly Quaternion GetRotation()
        {
            Vector3 scale = GetScale();
            float sx = scale.X, sy = scale.Y, sz = scale.Z;

            if (sx <= 0f || sy <= 0f || sz <= 0f)
                return Quaternion.Identity;

            float invSx = 1f / sx, invSy = 1f / sy, invSz = 1f / sz;

            float r00 = Matrix[0] * invSx, r10 = Matrix[1] * invSx, r20 = Matrix[2] * invSx;
            float r01 = Matrix[4] * invSy, r11 = Matrix[5] * invSy, r21 = Matrix[6] * invSy;
            float r02 = Matrix[8] * invSz, r12 = Matrix[9] * invSz, r22 = Matrix[10] * invSz;

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
            ColumnMajorMatrix.AxisLength(Matrix, 0), ColumnMajorMatrix.AxisLength(Matrix, 1), ColumnMajorMatrix.AxisLength(Matrix, 2));

        /// <summary>The local +X axis, normalized.</summary>
        public readonly Vector3 Right => ColumnMajorMatrix.Axis(Matrix, 0);

        /// <summary>The local -X axis, normalized.</summary>
        public readonly Vector3 Left => -Right;

        /// <summary>The local +Y axis, normalized.</summary>
        public readonly Vector3 Up => ColumnMajorMatrix.Axis(Matrix, 1);

        /// <summary>The local -Y axis, normalized.</summary>
        public readonly Vector3 Down => -Up;

        /// <summary>The local +Z axis, normalized.</summary>
        public readonly Vector3 Forward => ColumnMajorMatrix.Axis(Matrix, 2);

        /// <summary>The local -Z axis, normalized.</summary>
        public readonly Vector3 Back => -Forward;

        /// <summary>Resets the transform to the identity.</summary>
        public void SetIdentity() => this = Identity;

        /// <summary>Adds an offset to the translation.</summary>
        public void Translate(float x, float y, float z)
        {
            Matrix[12] += x;
            Matrix[13] += y;
            Matrix[14] += z;
        }

        /// <summary>Adds an offset to the translation.</summary>
        public void Translate(Vector3 offset) => Translate(offset.X, offset.Y, offset.Z);

        /// <summary>Replaces the rotation (a quaternion, x y z w), keeping the position and the scale, as the engine's Transform::Rotate.</summary>
        public void Rotate(float x, float y, float z, float w)
        {
            Vector3 p = GetPosition();
            Vector3 s = GetScale();
            this = FromTRS(p.X, p.Y, p.Z, x, y, z, w, s.X, s.Y, s.Z);
        }

        /// <summary>Replaces the rotation, keeping the position and the scale.</summary>
        public void Rotate(Quaternion q) => Rotate(q.X, q.Y, q.Z, q.W);

        /// <summary>Writes the diagonal scale values (replaces them, does not multiply), as the engine's Transform::SetScale.</summary>
        public void SetScale(float x, float y, float z)
        {
            Matrix[0] = x;
            Matrix[5] = y;
            Matrix[10] = z;
        }

        /// <summary>Writes the diagonal scale values.</summary>
        public void SetScale(Vector3 scale) => SetScale(scale.X, scale.Y, scale.Z);

        /// <summary>Replaces the translation.</summary>
        public void SetPosition(float x, float y, float z)
        {
            Matrix[12] = x;
            Matrix[13] = y;
            Matrix[14] = z;
        }

        /// <summary>Replaces the translation.</summary>
        public void SetPosition(Vector3 position) => SetPosition(position.X, position.Y, position.Z);

        /// <summary>
        /// The transform as a System.Numerics.Matrix4x4, for <c>Vector3.Transform</c>, <c>Matrix4x4.Decompose</c> and
        /// the rest of System.Numerics: the translation is in M41 to M43.
        /// </summary>
        public readonly Matrix4x4 ToMatrix4x4() => ColumnMajorMatrix.ToMatrix4x4(Matrix);

        /// <summary>Replaces the transform with a System.Numerics.Matrix4x4 (the translation in M41 to M43).</summary>
        public void FromMatrix4x4(Matrix4x4 m)
        {
            Matrix[0] = m.M11; Matrix[1] = m.M12; Matrix[2] = m.M13; Matrix[3] = m.M14;
            Matrix[4] = m.M21; Matrix[5] = m.M22; Matrix[6] = m.M23; Matrix[7] = m.M24;
            Matrix[8] = m.M31; Matrix[9] = m.M32; Matrix[10] = m.M33; Matrix[11] = m.M34;
            Matrix[12] = m.M41; Matrix[13] = m.M42; Matrix[14] = m.M43; Matrix[15] = m.M44;
        }

        /// <summary>The transform as a System.Numerics.Matrix4x4.</summary>
        public static explicit operator Matrix4x4(Transform t) => t.ToMatrix4x4();

        /// <inheritdoc/>
        public override readonly string ToString()
        {
            Vector3 p = GetPosition();
            return $"Transform(pos=({p.X:F3}, {p.Y:F3}, {p.Z:F3}))";
        }
    }
}
