#include "DisplayChangeListener.h"
#include "Logger/Logger.h"

#import <AppKit/AppKit.h>

namespace GameEngine::Platform {

DisplayChangeListener::~DisplayChangeListener()
{
    Destroy();
}

bool DisplayChangeListener::Create(Handler handler)
{
    if (m_Observer)
        return true;

    // A nil queue runs the block on the posting thread. AppKit posts on the main thread.
    id observer = [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                                    object:nil
                                                                     queue:nil
                                                                usingBlock:^(NSNotification*) { handler(); }];
    if (!observer)
    {
        Logger::Log::Error("Platform: the display-change observer was not added; monitor changes are seen only on a connect or disconnect");
        return false;
    }
    m_Observer = [observer retain];
    return true;
}

void DisplayChangeListener::Destroy()
{
    if (!m_Observer)
        return;
    id observer = static_cast<id>(m_Observer);
    [[NSNotificationCenter defaultCenter] removeObserver:observer];
    [observer release];
    m_Observer = nullptr;
}

} // namespace GameEngine::Platform
