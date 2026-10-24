namespace GameEngine.Scripting
{
    /// <summary>
    /// Marks a struct as an ECS component: a component a game declares, or the mirror of an engine
    /// component (<see cref="BuiltInComponentAttribute"/>). A game's component is an unmanaged struct with
    /// [StructLayout(LayoutKind.Sequential)]; a mirror has the C++ layout.
    /// </summary>
    public interface IComponent { }
}
