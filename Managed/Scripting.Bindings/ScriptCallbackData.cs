using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// The C# side of the engine's ScriptCallback&lt;Payload&gt; (Types/ScriptCallback.h): the function the engine
    /// calls per event, the function it calls once when the subscription ends, and the registry token both receive.
    /// A generated subscribe call builds one on the stack and hands its address to the engine, which copies it.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal readonly unsafe struct ScriptCallbackData
    {
        public readonly void* Invoke;
        public readonly void* Ended;
        public readonly void* User;

        public ScriptCallbackData(void* invoke, nint token)
        {
            Invoke = invoke;
            Ended = ScriptSubscriptionRegistry.EndedFunction;
            User = (void*)token;
        }
    }
}
