namespace GameEngine.Scripting
{
    /// <summary>How a <see cref="Curve"/> runs from one key to the next (the engine's <c>Math::CurveInterp</c>).</summary>
    public enum CurveInterp : byte
    {
        /// <summary>A straight line.</summary>
        Linear = 0,

        /// <summary>A smooth curve through the keys' slopes.</summary>
        Smooth = 1,

        /// <summary>The key's value until the next key.</summary>
        Step = 2,
    }
}
