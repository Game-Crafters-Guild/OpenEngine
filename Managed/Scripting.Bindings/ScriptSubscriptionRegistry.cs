using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Every live event subscription the generated bindings made, by the token the engine hands back with each event.
    /// A subscription is one listener: one handler on one event of one entity. The generated event accessors add and
    /// find listeners here; the engine's per-event call reads the handler of its token; the engine's Ended call is the
    /// one place a listener leaves (after an unsubscribe, or when the engine ends the subscription itself, as the
    /// physics world does when it shuts down).
    /// <para>
    /// Hot reload: a handler whose code lives in a collectible scripts context is recorded against that context. When
    /// the context starts to unload, its handlers are dropped at once (never called again, and no longer keeping the
    /// context alive) and their subscriptions are ended. This assembly is loaded once into the default context, so it
    /// outlives every scripts context.
    /// </para>
    /// <para>
    /// Threads: the accessors run on the main thread (the generated thunks refuse other threads), the engine calls on
    /// the event's delivery thread, and an unload on the thread that unloads; one lock covers the table. The registry
    /// calls no user code under its lock and never calls the script runner.
    /// </para>
    /// </summary>
    internal static unsafe class ScriptSubscriptionRegistry
    {
        private sealed class Listener
        {
            public Listener(string eventLabel, uint scope, Delegate handler, AssemblyLoadContext? owner)
            {
                EventLabel = eventLabel;
                Scope = scope;
                Handler = handler;
                Owner = owner;
            }

            public string EventLabel { get; }
            public uint Scope { get; }
            // Null once the owner's context unloads: the listener waits for the engine to end it, and is not called.
            public Delegate? Handler { get; set; }
            public AssemblyLoadContext? Owner { get; }
            public ulong Subscription { get; set; }
            public delegate* unmanaged[Cdecl]<ulong, int> Unsubscribe { get; set; }
        }

        private static readonly object s_Lock = new object();
        private static readonly Dictionary<nint, Listener> s_Listeners = new Dictionary<nint, Listener>();
        // Weak keys: being hooked must not keep a context alive.
        private static readonly ConditionalWeakTable<AssemblyLoadContext, object> s_HookedOwners =
            new ConditionalWeakTable<AssemblyLoadContext, object>();
        private static readonly object s_HookedMarker = new object();
        private static nint s_LastToken;

        /// <summary>The function the engine calls once when a subscription ends, with its token.</summary>
        public static void* EndedFunction => (delegate* unmanaged[Cdecl]<void*, void>)&OnEnded;

        /// <summary>Every listener the registry holds, also one whose context is unloading; for the tests.</summary>
        internal static int Count
        {
            get
            {
                lock (s_Lock)
                    return s_Listeners.Count;
            }
        }

        /// <summary>Records a listener before its subscribe call and returns its token.</summary>
        public static nint Add(string eventLabel, uint scope, Delegate handler)
        {
            AssemblyLoadContext? owner = ResolveCollectibleOwner(handler);
            nint token;
            bool hook;
            lock (s_Lock)
            {
                token = ++s_LastToken;
                s_Listeners.Add(token, new Listener(eventLabel, scope, handler, owner));
                hook = owner != null && !s_HookedOwners.TryGetValue(owner, out _);
                if (hook)
                    s_HookedOwners.Add(owner!, s_HookedMarker);
            }
            if (hook)
                owner!.Unloading += static context => RevokeOwned(context);
            return token;
        }

        /// <summary>Stores the engine's subscription of a listener once its subscribe call succeeded.</summary>
        public static void Bind(nint token, ulong subscription, delegate* unmanaged[Cdecl]<ulong, int> unsubscribe)
        {
            lock (s_Lock)
            {
                if (!s_Listeners.TryGetValue(token, out Listener? listener))
                    return;
                listener.Subscription = subscription;
                listener.Unsubscribe = unsubscribe;
            }
        }

        /// <summary>Drops a listener the engine never took, or can no longer end (the engine is gone).</summary>
        public static void Discard(nint token)
        {
            lock (s_Lock)
                s_Listeners.Remove(token);
        }

        /// <summary>
        /// The engine's subscription of the listener a `-=` removes: the newest live one with the same event, entity and
        /// handler, as a C# event removes the last matching handler.
        /// </summary>
        public static bool TryFind(string eventLabel, uint scope, Delegate handler, out nint token, out ulong subscription)
        {
            token = 0;
            subscription = 0;
            lock (s_Lock)
            {
                foreach (KeyValuePair<nint, Listener> entry in s_Listeners)
                {
                    Listener listener = entry.Value;
                    if (entry.Key > token && listener.Subscription != 0 && listener.Scope == scope &&
                        string.Equals(listener.EventLabel, eventLabel, StringComparison.Ordinal) &&
                        listener.Handler != null && listener.Handler.Equals(handler))
                    {
                        token = entry.Key;
                        subscription = listener.Subscription;
                    }
                }
            }
            return token != 0;
        }

        /// <summary>The handler of a token, or null when it ended or its context is unloading.</summary>
        public static Delegate? Handler(nint token)
        {
            lock (s_Lock)
                return s_Listeners.TryGetValue(token, out Listener? listener) ? listener.Handler : null;
        }

        /// <summary>Reports a handler that threw; the exception never crosses into the engine.</summary>
        public static void Report(string eventLabel, Exception error)
        {
            Console.Error.WriteLine($"[Scripting] A {eventLabel} handler threw: {error}");
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void OnEnded(void* user)
        {
            lock (s_Lock)
                s_Listeners.Remove((nint)user);
        }

        /// <summary>
        /// Drops the handlers an unloading context owns and ends their subscriptions. Runs on the thread that unloads
        /// the context; an engine that refuses the call there (the main-thread rule) leaves the listener without a
        /// handler until the engine ends it. Nothing propagates: an exception would take the unload down with it.
        /// </summary>
        private static void RevokeOwned(AssemblyLoadContext owner)
        {
            List<(ulong Subscription, nint Unsubscribe)>? revoked = null;
            lock (s_Lock)
            {
                foreach (Listener listener in s_Listeners.Values)
                {
                    if (!ReferenceEquals(listener.Owner, owner) || listener.Handler == null)
                        continue;
                    listener.Handler = null;
                    if (listener.Subscription != 0)
                        (revoked ??= new List<(ulong, nint)>()).Add((listener.Subscription, (nint)listener.Unsubscribe));
                }
            }
            if (revoked == null)
                return;
            foreach ((ulong subscription, nint unsubscribe) in revoked)
            {
                try
                {
                    ((delegate* unmanaged[Cdecl]<ulong, int>)unsubscribe)(subscription);
                }
                catch
                {
                }
            }
        }

        /// <summary>
        /// The collectible context the handler would keep alive, or null when it belongs to the default one. A delegate
        /// can be multicast, and any collectible target pins its context, so the first one found decides.
        /// </summary>
        private static AssemblyLoadContext? ResolveCollectibleOwner(Delegate handler)
        {
            try
            {
                foreach (Delegate d in handler.GetInvocationList())
                {
                    AssemblyLoadContext? owner = CollectibleContextOf(d.Target?.GetType().Assembly) ??
                                                 CollectibleContextOf(d.Method.Module.Assembly);
                    if (owner != null)
                        return owner;
                }
            }
            catch
            {
            }
            return null;
        }

        private static AssemblyLoadContext? CollectibleContextOf(Assembly? assembly)
        {
            if (assembly == null)
                return null;
            AssemblyLoadContext? context = AssemblyLoadContext.GetLoadContext(assembly);
            return context != null && context.IsCollectible ? context : null;
        }
    }
}
