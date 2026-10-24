namespace GameEngine.Scripting
{
    public partial struct ParticlePlayback
    {
        /// <summary>
        /// Queues <paramref name="name"/> for the emitter's game-event rules, delivered to every live particle on the
        /// emitter's next update. False when the queue is full; the event is then dropped. Write the component back
        /// with <c>ComponentType&lt;ParticlePlayback&gt;.Set</c>, or call it on the <c>ref</c> of a system.
        /// </summary>
        public bool SendEvent(StringId name)
        {
            Span<StringId> pending = PendingEvents;
            if (PendingEventCount >= (uint)pending.Length)
                return false;
            pending[(int)PendingEventCount++] = name;
            return true;
        }
    }
}
