#ifndef GE_EVENTS_CLOCK_SCRIPT_API_H
#define GE_EVENTS_CLOCK_SCRIPT_API_H

// The Clock script module: a second script API header in the Events folder, behind an include
// guard, with a static member function. Never compiled: the scanner reads it.

namespace GameEngine::Events
{

/*
// @ge-script
void Retired();
*/

struct Clock
{
    /// Seconds since the start.
    // @ge-script
    static double Now();
};

} // namespace GameEngine::Events

#endif
