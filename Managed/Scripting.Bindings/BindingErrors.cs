using System.Runtime.CompilerServices;
using GameEngine.ECS.Internal;

namespace GameEngine.Scripting
{
    /// <summary>
    /// What a generated call does with a result other than GE_Result_Ok. NotInitialized (no engine, or no system for
    /// the call, such as input before play) is an answer, not an error: the out values hold their defaults and a
    /// setter does nothing, so a generated call returns without calling here. InvalidArg throws ArgumentException;
    /// every other code throws EngineCallException. Both carry the engine's last error for the calling thread
    /// (GE_GetLastScriptError).
    /// </summary>
    internal static class BindingErrors
    {
        /// <summary>GE_Result_NotInitialized.</summary>
        public const int NotInitialized = -4;

        private const int kInvalidArg = -2;

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Throw(int code, string function)
        {
            string reason = LastError();
            if (code == kInvalidArg)
                throw new ArgumentException(reason.Length != 0 ? reason : $"{function}: the engine refused an argument");
            throw new EngineCallException(code, function, reason);
        }

        /// <summary>The engine's last error text for the calling thread, or empty when the engine has none.</summary>
        public static string LastError() => ScriptLastError.Read();
    }
}
