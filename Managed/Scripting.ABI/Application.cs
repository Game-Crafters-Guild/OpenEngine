using System;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>Controls the running game through its application host.</summary>
    public static class Application
    {
        private const int kOk = 0;
        private const int kNotInitialized = -4;

        [DllImport("GameEngine.Native", CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Application_RequestExit(int exitCode);

        /// <summary>
        /// Requests game exit without interrupting the current frame.
        /// </summary>
        /// <remarks>
        /// Call on the main thread, outside a system job. From another thread or a job,
        /// use <see cref="MainThread.Post(Action)"/> to post the request.
        /// <para>
        /// The Player runs its normal shutdown, including script OnDestroy, then returns
        /// the requested exit code. Repeated requests keep the last code.
        /// </para>
        /// <para>
        /// The Editor stops play through its normal change-review flow at the next safe
        /// editor update. It stays open, retains unsaved documents, and ignores the exit code.
        /// Outside play mode, or with no application, this call does nothing.
        /// </para>
        /// </remarks>
        /// <param name="exitCode">Process exit code for a standalone Player; defaults to zero.</param>
        /// <exception cref="InvalidOperationException">
        /// The calling thread was refused or the native host failed to handle the request.
        /// </exception>
        public static void RequestExit(int exitCode = 0)
        {
            int result = GE_Application_RequestExit(exitCode);
            if (result != kOk && result != kNotInitialized)
                throw new InvalidOperationException(
                    $"Application.RequestExit failed with native result {result}. Calls must run on the main thread outside a job. " +
                    "Use MainThread.Post(() => Application.RequestExit(exitCode)) from another thread or a system job.");
        }
    }
}
