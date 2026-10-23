using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The scanner's fixture component mirrors (Tools/ComponentScanner/Tests/Fixtures/Components/Expected/CSharp),
    /// compiled into this assembly: an [InlineArray] field inside an explicit layout loads and reads its elements at
    /// the C++ offsets, a bool is read at its byte through <c>ref T</c>, and the enum and text setters refuse what
    /// the C++ field cannot hold. The sizes and offsets are the ones ScriptBindingTests pins to the C++ compiler.
    /// </summary>
    public class ComponentMirrorFixtureTests
    {
        private static bool ReadEnabled(ref FixtureKinds kinds) => kinds.Enabled;

        private static void SetLabel(byte[] bytes, string text) => MemoryMarshal.AsRef<FixtureKinds>(bytes.AsSpan()).Label = text;

        /// <summary>The mirrors have the C++ sizes, and the hand-written kinds theirs.</summary>
        [Test]
        public void MirrorsHaveTheCppSizes()
        {
            Assert.That(Unsafe.SizeOf<FixtureKinds>(), Is.EqualTo(928));
            Assert.That(Unsafe.SizeOf<FixtureArrays>(), Is.EqualTo(168));
            Assert.That(Unsafe.SizeOf<FixtureClock>(), Is.EqualTo(32));
            Assert.That(Unsafe.SizeOf<FixtureRule>(), Is.EqualTo(8));
            Assert.That(Unsafe.SizeOf<AssetGuid>(), Is.EqualTo(16));
            Assert.That(Unsafe.SizeOf<ColorLinear>(), Is.EqualTo(16));
            Assert.That(Unsafe.SizeOf<CurveKey>(), Is.EqualTo(20));
            Assert.That(Unsafe.SizeOf<Curve>(), Is.EqualTo(644));
            Assert.That(Unsafe.SizeOf<BoundingBox>(), Is.EqualTo(24));
        }

        /// <summary>An element of an inline array inside an explicit layout reads the bytes at its C++ offset.</summary>
        [Test]
        public void InlineArrayElementsReadTheirBytes()
        {
            byte[] bytes = new byte[Unsafe.SizeOf<FixtureArrays>()];
            BitConverter.TryWriteBytes(bytes.AsSpan(20, 4), 7);
            BitConverter.TryWriteBytes(bytes.AsSpan(128, 4), 2.5f);
            ref FixtureArrays arrays = ref MemoryMarshal.AsRef<FixtureArrays>(bytes.AsSpan());

            Assert.That(arrays.Lanes[1], Is.EqualTo(7));
            Assert.That(arrays.Rules[0].Height, Is.EqualTo(2.5f));
            arrays.Weights[3] = 1.5f;
            Assert.That(BitConverter.ToSingle(bytes, 12), Is.EqualTo(1.5f));
        }

        /// <summary>A bool is the one byte at its offset, read through a ref.</summary>
        [Test]
        public void BoolIsReadAtItsByteThroughRef()
        {
            byte[] bytes = new byte[Unsafe.SizeOf<FixtureKinds>()];
            ref FixtureKinds kinds = ref MemoryMarshal.AsRef<FixtureKinds>(bytes.AsSpan());
            Assert.That(ReadEnabled(ref kinds), Is.False);

            bytes[0] = 1;

            Assert.That(ReadEnabled(ref kinds), Is.True);
        }

        /// <summary>An enum setter refuses a value the enum does not declare, with gaps and without.</summary>
        [Test]
        public void EnumSetterRefusesAValueOutsideTheEnum()
        {
            FixtureKinds kinds = default;

            kinds.Glow = FixtureGlow.Bright;
            kinds.Pulse = FixturePulse.Slow;

            Assert.That(kinds.Glow, Is.EqualTo(FixtureGlow.Bright));
            Assert.That(kinds.Pulse, Is.EqualTo(FixturePulse.Slow));
            var glow = Assert.Throws<ArgumentOutOfRangeException>(() => kinds.Glow = (FixtureGlow)3);
            Assert.That(glow!.Message, Does.Contain("FixtureKinds.Glow is Off, Soft or Bright."));
            Assert.Throws<ArgumentOutOfRangeException>(() => kinds.Pulse = (FixturePulse)1);
            Assert.That(kinds.Pulse, Is.EqualTo(FixturePulse.Slow));
        }

        /// <summary>A text setter writes UTF-8 and a zero, and refuses text past the field's capacity.</summary>
        [Test]
        public void TextSetterRefusesTextPastTheCapacity()
        {
            byte[] bytes = new byte[Unsafe.SizeOf<FixtureKinds>()];
            ref FixtureKinds kinds = ref MemoryMarshal.AsRef<FixtureKinds>(bytes.AsSpan());
            int label = 885;
            bytes.AsSpan(label, 16).Fill((byte)'x');

            kinds.Label = "Tür";

            Assert.That(kinds.Label, Is.EqualTo("Tür"));
            Assert.That(bytes.AsSpan(label + 4, 12).IndexOfAnyExcept((byte)0), Is.EqualTo(-1));
            var refused = Assert.Throws<ArgumentException>(() => SetLabel(bytes, new string('a', 16)));
            Assert.That(refused!.Message, Does.Contain("FixtureKinds.Label holds at most 15 UTF-8 bytes; the text has 16."));
            Assert.That(kinds.Label, Is.EqualTo("Tür"));
        }

        /// <summary>A mirror takes its defaults from the engine: with no default table, the constructor throws the fix.</summary>
        [Test]
        public void ConstructorWithoutEngineDefaultsThrowsTheFix()
        {
            var refused = Assert.Throws<InvalidOperationException>(() => _ = new FixtureKinds());
            Assert.That(refused!.Message, Does.StartWith("FixtureKinds takes its defaults from the engine"));
        }

        /// <summary>An element struct takes its defaults from the engine too: `new` never gives silent zeros.</summary>
        [Test]
        public void ElementConstructorWithoutEngineDefaultsThrowsTheFix()
        {
            var refused = Assert.Throws<InvalidOperationException>(() => _ = new FixtureRule());
            Assert.That(refused!.Message, Does.StartWith("FixtureRule takes its defaults from the engine"));
        }
    }
}
