using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace GameEngine.Scripting
{
    /// <summary>
    /// The engine's main thread, the thread that runs its frames, reached from a thread of your own
    /// (<c>Task.Run</c>, a timer, a socket callback).
    /// </summary>
    public static class MainThread
    {
        private const string kLib = "GameEngine.Native";

        // What GE_MainThread_RequestPump answers (GE_MainThreadPost in ScriptingABI.h).
        private const int kQueued = 0;
        private const int kRunInline = 1;
        // What GE_MainThread_SetDrain answers when it installed the drain (GE_Result_Ok).
        private const int kDrainInstalled = 0;

        private enum DrainState
        {
            NotInstalled,
            Installed,
            Refused,
        }

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern unsafe int GE_MainThread_SetDrain(delegate* unmanaged[Cdecl]<int, void> drain);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_MainThread_RequestPump();

        // Lock order: this lock, then the engine's post gate, then its main-thread queue. Never held
        // while an action runs.
        private static readonly object s_Lock = new();
        private static List<Action> s_Queue = new();
        private static DrainState s_DrainState;
        // The collectible contexts whose Unloading this class handles, weakly, so a watched context
        // still collects.
        private static readonly ConditionalWeakTable<AssemblyLoadContext, object> s_Watched = new();

        /// <summary>
        /// Runs <paramref name="action"/> on the main thread at the end of the current frame, after this
        /// frame's systems and before the next frame's first system, in the order the posts were made.
        /// <para>
        /// Fire and forget: nothing is returned and nothing waits for the action. A value-returning or
        /// waiting form would deadlock: the main thread inside a frame waits for the frame's systems, and a
        /// system waiting for the main thread would wait for itself. To get a result back, have the action
        /// write it somewhere your thread reads.
        /// </para>
        /// <para>
        /// The action waits for the main thread however long the main thread is busy, and never runs on
        /// any other thread. An exception it throws is reported on the console and does not reach the frame. An action posted
        /// by a script whose assembly unloads (a hot reload) before it runs is discarded, as are the actions
        /// still queued when the engine shuts down; a post after the engine shut down is dropped and logged.
        /// In a process with no frame loop (no engine initialized, or a <c>dotnet test</c> host) the action
        /// runs at once, on the posting thread.
        /// </para>
        /// </summary>
        /// <param name="action">What to run on the main thread.</param>
        /// <exception cref="ArgumentNullException"><paramref name="action"/> is null.</exception>
        /// <exception cref="InvalidOperationException">The engine refused this assembly's drain because another
        /// one is installed (a second copy of GameEngine.Scripting.ABI, or a native host's own), so nothing would
        /// run a queued action.</exception>
        public static void Post(Action action)
        {
            ArgumentNullException.ThrowIfNull(action);
            int answer;
            lock (s_Lock)
            {
                EnsureDrainInstalled();
                // The request and the append under one lock: a drain swaps its batch out under this lock,
                // so it either takes the action or finds the request this call set and runs at the next
                // pump. Nothing strands.
                answer = GE_MainThread_RequestPump();
                if (answer == kQueued)
                {
                    Enqueue(action);
                    return;
                }
            }
            // Outside the lock, so the action may post again. A dropped post was logged by the engine.
            if (answer == kRunInline)
                Run(action);
        }

        /// <summary>
        /// Appends an action to the queue the next drain runs, and has the unloading of the collectible
        /// context its code lives in drop it.
        /// </summary>
        internal static void Enqueue(Action action)
        {
            lock (s_Lock)
            {
                s_Queue.Add(action);
                foreach (Delegate target in action.GetInvocationList())
                {
                    AssemblyLoadContext? context = ContextOf(target);
                    if (context != null && context.IsCollectible && s_Watched.TryAdd(context, s_Lock))
                        context.Unloading += OnContextUnloading;
                }
            }
        }

        /// <summary>
        /// Takes the whole queue and runs it in order when <paramref name="run"/> is true, or discards it.
        /// An action the batch posts lands in the next batch.
        /// </summary>
        internal static void TakeAndRun(bool run)
        {
            List<Action> batch;
            lock (s_Lock)
            {
                if (s_Queue.Count == 0)
                    return;
                batch = s_Queue;
                s_Queue = new List<Action>();
            }
            if (!run)
                return;
            foreach (Action action in batch)
                Run(action);
        }

        // Under s_Lock. First-wins in the engine; this assembly lives in the default context and never
        // unloads, so the one installation is this one unless something else installed first.
        private static unsafe void EnsureDrainInstalled()
        {
            if (s_DrainState == DrainState.NotInstalled)
                RecordDrainInstall(GE_MainThread_SetDrain(&Drain));
            if (s_DrainState == DrainState.Refused)
                throw new InvalidOperationException(
                    "MainThread.Post: the engine refused this assembly's main-thread drain because another one is " +
                    "installed, so a queued action would never run. Load GameEngine.Scripting.ABI once, in the " +
                    "default load context, and do not install a drain from the native host.");
        }

        /// <summary>
        /// Records what GE_MainThread_SetDrain answered, once per process; a refusal is reported once here and
        /// makes every later Post throw.
        /// </summary>
        internal static void RecordDrainInstall(int result)
        {
            lock (s_Lock)
            {
                if (s_DrainState != DrainState.NotInstalled)
                    return;
                s_DrainState = result == kDrainInstalled ? DrainState.Installed : DrainState.Refused;
            }
            if (result != kDrainInstalled)
                Report("the engine refused the main-thread drain (GE_MainThread_SetDrain answered " + result +
                       "): another drain is installed, and every MainThread.Post will throw.");
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void Drain(int run)
        {
            try { TakeAndRun(run != 0); }
            catch (Exception e) { Report("the drain failed: " + e); }
        }

        private static void Run(Action action)
        {
            try { action(); }
            catch (Exception e) { Report("a posted action threw: " + e); }
        }

        // The unloading context's actions would run code that is going away: they are dropped.
        private static void OnContextUnloading(AssemblyLoadContext context)
        {
            lock (s_Lock)
            {
                s_Queue.RemoveAll(action => BelongsTo(action, context));
            }
        }

        private static bool BelongsTo(Action action, AssemblyLoadContext context)
        {
            foreach (Delegate target in action.GetInvocationList())
            {
                if (ReferenceEquals(ContextOf(target), context))
                    return true;
            }
            return false;
        }

        // The context of the code a delegate runs: the assembly that declares its method, which for a
        // lambda is the script's own closure class. Null when the runtime cannot say, which a NativeAOT
        // image, with no collectible context to unload, can answer that way.
        private static AssemblyLoadContext? ContextOf(Delegate target)
        {
            try
            {
                MethodInfo method = target.Method;
                Assembly assembly = method.DeclaringType?.Assembly ?? method.Module.Assembly;
                return AssemblyLoadContext.GetLoadContext(assembly);
            }
            catch (Exception)
            {
                return null;
            }
        }

        // The console channel: this runs from the drain, before or after the engine's managed logging is bound.
        private static void Report(string message) => ConsoleDiagnostics.Report("MainThread.Post", message);
    }
}
