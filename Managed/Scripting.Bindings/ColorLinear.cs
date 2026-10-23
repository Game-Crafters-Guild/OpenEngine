using System;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// A color in linear space: red, green, blue and alpha, each 0 to 1, in the engine's
    /// <c>ColorLinear</c> layout of four floats. Lighting and blending work on linear values; a color
    /// picked in an image editor is sRGB, so convert it with <see cref="FromSrgb"/>.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public readonly record struct ColorLinear(float R, float G, float B, float A)
    {
        /// <summary>The linear color of an sRGB color: each channel through the sRGB curve, alpha as it is.
        /// Every channel is clamped to 0 to 1 first.</summary>
        public static ColorLinear FromSrgb(float r, float g, float b, float a = 1f) =>
            new(SrgbToLinear(r), SrgbToLinear(g), SrgbToLinear(b), Math.Clamp(a, 0f, 1f));

        private static float SrgbToLinear(float channel)
        {
            float clamped = Math.Clamp(channel, 0f, 1f);
            return clamped <= 0.04045f ? clamped / 12.92f : MathF.Pow((clamped + 0.055f) / 1.055f, 2.4f);
        }
    }
}
