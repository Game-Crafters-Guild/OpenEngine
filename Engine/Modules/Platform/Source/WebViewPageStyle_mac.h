#pragma once

#import <Foundation/Foundation.h>

namespace GameEngine::Platform::Detail
{
// Dark background injected into every loaded page so a light page never flashes white
// before it paints. At document-start the parser may not have created a root yet;
// observe its creation and attach before the next paint. Reapply after navigation
// for pages that rebuild <head> during load.
//
// The page scrollbar is deliberately left alone: macOS WebKit draws it as a native
// scroller, which ignores ::-webkit-scrollbar and scrollbar-color (scrollbar-width is
// the only property that takes effect), so it cannot be themed to match the editor.
inline NSString* const kPageStyleScript = @""
    "(function() {"
    "  var root = document.documentElement;"
    "  if (root) {"
    "    root.style.backgroundColor = '#272727';"
    "  }"
    "  if (document.body) {"
    "    document.body.style.backgroundColor = '#272727';"
    "  }"
    "  var style = document.getElementById('ge-injected-style');"
    "  if (style) {"
    "    if (document.head) {"
    "      document.head.appendChild(style);"  // move last so it outranks page styles
    "    }"
    "    return;"
    "  }"
    "  style = document.createElement('style');"
    "  style.id = 'ge-injected-style';"
    "  style.textContent = 'html, body { background-color: #272727 !important; }';"
    "  var attach = function() {"
    "    var parent = document.head || document.documentElement;"
    "    if (!parent) {"
    "      return false;"
    "    }"
    "    parent.appendChild(style);"
    "    return true;"
    "  };"
    "  if (!attach()) {"
    "    var observer = new MutationObserver(function() {"
    "      if (attach()) {"
    "        observer.disconnect();"
    "      }"
    "    });"
    "    observer.observe(document, { childList: true, subtree: true });"
    "  }"
    "})();";

}
