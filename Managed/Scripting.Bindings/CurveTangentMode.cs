namespace GameEngine.Scripting
{
    /// <summary>Where the slopes of a <see cref="CurveKey"/> come from (the engine's <c>Math::CurveTangentMode</c>).</summary>
    public enum CurveTangentMode : byte
    {
        /// <summary>From the keys on each side.</summary>
        Auto = 0,

        /// <summary>Zero: the curve is level at the key.</summary>
        Flat = 1,

        /// <summary>The key's own in and out slopes, set apart.</summary>
        Broken = 2,

        /// <summary>The key's own slopes, set together.</summary>
        Manual = 3,

        /// <summary>The in slope, used out as well.</summary>
        Mirrored = 4,
    }
}
