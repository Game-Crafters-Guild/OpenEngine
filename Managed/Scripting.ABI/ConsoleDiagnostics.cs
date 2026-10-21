using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// The console channel for Scripting.ABI diagnostics that run where the engine's managed logging may not
    /// be bound (load and reload threads, the main-thread drain) and that must never be the thing that throws.
    /// Under CoreCLR, CoreBridge redirects the console to the engine log.
    /// </summary>
    internal static class ConsoleDiagnostics
    {
        /// <summary>Writes <paramref name="message"/> under the <paramref name="channel"/> prefix.</summary>
        internal static void Report(string channel, string message)
        {
            try { Console.WriteLine("[" + channel + "] " + message); }
            catch { }
        }
    }
}
