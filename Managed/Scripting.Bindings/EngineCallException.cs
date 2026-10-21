namespace GameEngine.Scripting
{
    /// <summary>
    /// A script API call that the engine refused or that failed: called from a thread its rule does not allow, or
    /// failed inside the engine. The message says what happened and what to do.
    /// </summary>
    public sealed class EngineCallException : Exception
    {
        internal EngineCallException(int code, string function, string reason)
            : base(reason.Length != 0 ? reason : $"{function} failed with the engine result {code}")
        {
            Code = code;
            Function = function;
        }

        /// <summary>The engine's result code (GE_Result).</summary>
        public int Code { get; }

        /// <summary>The call, as Module.Function ("Input.IsKeyDown").</summary>
        public string Function { get; }
    }
}
