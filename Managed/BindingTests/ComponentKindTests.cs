using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The hand-written kinds a component mirror holds: an asset GUID keeps the engine's text order, a linear color
    /// converts from sRGB, and each kind reads its fields at the C++ offsets ScriptBindingTests pins.
    /// </summary>
    public class ComponentKindTests
    {
        private const string kGuidText = "00112233-4455-6677-8899-aabbccddeeff";

        private static byte[] BytesOf(AssetGuid guid) =>
            MemoryMarshal.AsBytes(MemoryMarshal.CreateReadOnlySpan(ref Unsafe.AsRef(in guid), 1)).ToArray();

        /// <summary>An asset GUID holds its bytes in the order its text writes them, and its text round-trips.</summary>
        [Test]
        public void AssetGuidKeepsTheTextOrder()
        {
            AssetGuid guid = AssetGuid.Parse(kGuidText);

            Assert.That(BytesOf(guid), Is.EqualTo(Convert.FromHexString(kGuidText.Replace("-", ""))));
            Assert.That(guid.ToString(), Is.EqualTo(kGuidText));
            Assert.That(guid.ToGuid(), Is.EqualTo(Guid.Parse(kGuidText)));
            Assert.That(AssetGuid.FromGuid(guid.ToGuid()), Is.EqualTo(guid));
            Assert.That(guid.IsNone, Is.False);
            Assert.That(AssetGuid.None.IsNone, Is.True);
            Assert.That(default(AssetGuid), Is.EqualTo(AssetGuid.None));
        }

        /// <summary>An sRGB color converts to linear through the sRGB curve; alpha stays as it is.</summary>
        [Test]
        public void ColorLinearFromSrgbTakesTheSrgbCurve()
        {
            ColorLinear color = ColorLinear.FromSrgb(1f, 0.5f, 0f, 0.25f);

            Assert.That(color.R, Is.EqualTo(1f));
            Assert.That(color.G, Is.EqualTo(0.21404f).Within(1e-4f));
            Assert.That(color.B, Is.EqualTo(0f));
            Assert.That(color.A, Is.EqualTo(0.25f));
            Assert.That(ColorLinear.FromSrgb(2f, -1f, 0.04f).R, Is.EqualTo(1f));
        }

        /// <summary>The hand-written kinds read their fields at the C++ offsets.</summary>
        [Test]
        public void KindsReadTheirFieldsAtTheCppOffsets()
        {
            byte[] curveBytes = new byte[Unsafe.SizeOf<Curve>()];
            curveBytes[0] = 2;
            BitConverter.TryWriteBytes(curveBytes.AsSpan(4 + 20 + 8, 4), 3f);
            curveBytes[4 + 20 + 16] = (byte)CurveInterp.Step;
            curveBytes[4 + 20 + 17] = (byte)CurveTangentMode.Mirrored;
            byte[] boxBytes = new byte[Unsafe.SizeOf<BoundingBox>()];
            BitConverter.TryWriteBytes(boxBytes.AsSpan(16, 4), 5f);
            byte[] colorBytes = new byte[Unsafe.SizeOf<ColorLinear>()];
            BitConverter.TryWriteBytes(colorBytes.AsSpan(8, 4), 0.5f);

            Curve curve = MemoryMarshal.Read<Curve>(curveBytes);
            BoundingBox box = MemoryMarshal.Read<BoundingBox>(boxBytes);
            ColorLinear color = MemoryMarshal.Read<ColorLinear>(colorBytes);

            Assert.That(curve.KeyCount, Is.EqualTo(2));
            Assert.That(curve.Keys[1].InTangent, Is.EqualTo(3f));
            Assert.That(curve.Keys[1].Interp, Is.EqualTo(CurveInterp.Step));
            Assert.That(curve.Keys[1].TangentMode, Is.EqualTo(CurveTangentMode.Mirrored));
            Assert.That(box.HalfExtents, Is.EqualTo(new Vector3(0f, 5f, 0f)));
            Assert.That(color.B, Is.EqualTo(0.5f));
        }
    }
}
