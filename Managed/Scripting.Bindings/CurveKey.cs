using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// One key of a <see cref="Curve"/>: the value at a time, the slopes into and out of it, and how the curve
    /// runs to the next key. The engine's <c>Math::CurveKey</c> layout, 20 bytes.
    /// </summary>
    [StructLayout(LayoutKind.Explicit, Size = 20)]
    public struct CurveKey
    {
        /// <summary>Where the key sits on the curve's time axis.</summary>
        [FieldOffset(0)] public float Time;

        /// <summary>The curve's value at <see cref="Time"/>.</summary>
        [FieldOffset(4)] public float Value;

        /// <summary>The slope into the key, in value per time unit. Read when <see cref="TangentMode"/> is Broken or Manual.</summary>
        [FieldOffset(8)] public float InTangent;

        /// <summary>The slope out of the key, in value per time unit. Read when <see cref="TangentMode"/> is Broken or Manual.</summary>
        [FieldOffset(12)] public float OutTangent;

        /// <summary>How the curve runs from this key to the next.</summary>
        [FieldOffset(16)] public CurveInterp Interp;

        /// <summary>Where the key's slopes come from.</summary>
        [FieldOffset(17)] public CurveTangentMode TangentMode;
    }
}
