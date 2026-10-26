using System.Numerics;

namespace GameEngine.Scripting
{
    /// <summary>
    /// An axis-aligned box: its top-left corner (<see cref="X"/>, <see cref="Y"/>) and its size. An element's
    /// rect is in window space, in UI pixels. A pointer event's X and Y are in the element's layout space,
    /// which is the same space only when no transform applies to the element or its ancestors.
    /// </summary>
    /// <remarks>
    /// For a transformed element it is the axis-aligned bounds of the transformed box, so it can hold points
    /// the element does not cover. A tooltip, a menu or a drag anchors to it. The box is closed: its right and
    /// bottom edges are part of it, as in the UI's own hit test. <c>Input.GetMousePosition()</c> is in
    /// play-surface pixels, which differ from UI pixels when the UI scale is not 1.
    /// </remarks>
    public readonly record struct Rect(float X, float Y, float Width, float Height)
    {
        /// <summary>The top-left corner.</summary>
        public Vector2 Position => new(X, Y);

        /// <summary>The width and the height.</summary>
        public Vector2 Size => new(Width, Height);

        /// <summary>The right edge, <c>X + Width</c>; a point on it is inside the box.</summary>
        public float Right => X + Width;

        /// <summary>The bottom edge, <c>Y + Height</c>; a point on it is inside the box.</summary>
        public float Bottom => Y + Height;

        /// <summary>Whether a point falls inside the box, its four edges included.</summary>
        public bool Contains(Vector2 point) => point.X >= X && point.X <= Right && point.Y >= Y && point.Y <= Bottom;
    }
}
