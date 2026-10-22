using System;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    public static class Application
    {
        private const int kOk = 0;
        private const int kNotInitialized = -4;

        [DllImport("GameEngine.Native", CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Application_RequestExit(int exitCode);

        public static void RequestExit(int exitCode = 0)
        {
            int result = GE_Application_RequestExit(exitCode);
            if (result != kOk && result != kNotInitialized)
                throw new InvalidOperationException(
                    "Application.RequestExit was refused. Call it on the main thread outside a job. " +
                    "Use MainThread.Post(() => Application.RequestExit(exitCode)) from another thread or a system job.");
        }
    }
}
