#include "Platform/Toolbar.h"
#include "Platform/Window.h"
#include "Types/FlatMap.h"

#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>
#include <memory>
#include <string>

namespace GameEngine { class MacToolbar; }

@interface GE_MacToolbarTarget : NSObject
@property(nonatomic, assign) GameEngine::MacToolbar* toolbar;
+ (instancetype)shared;
- (IBAction)onMenuItem:(id)sender;
@end

namespace GameEngine {

namespace {
void ApplyItemTitle(NSMenuItem* item, const std::string& title)
{
    NSString* text = [NSString stringWithUTF8String:title.c_str()];
    NSArray<NSString*>* parts = [text componentsSeparatedByString:@"\t"];
    [item setTitle:[parts firstObject]];
    [item setKeyEquivalent:@""];
    if ([parts count] < 2)
        return;
    NSArray<NSString*>* chord = [parts[1] componentsSeparatedByString:@"+"];
    NSEventModifierFlags modifiers = 0;
    for (NSUInteger index = 0; index + 1 < [chord count]; ++index)
    {
        NSString* modifier = [chord[index] lowercaseString];
        if ([modifier isEqualToString:@"cmd"])
            modifiers |= NSEventModifierFlagCommand;
        else if ([modifier isEqualToString:@"ctrl"])
            modifiers |= NSEventModifierFlagControl;
        else if ([modifier isEqualToString:@"shift"])
            modifiers |= NSEventModifierFlagShift;
        else if ([modifier isEqualToString:@"alt"])
            modifiers |= NSEventModifierFlagOption;
        else
            return;
    }
    NSString* key = [[chord lastObject] lowercaseString];
    if ([key length] == 1)
    {
        [item setKeyEquivalent:key];
        [item setKeyEquivalentModifierMask:modifiers];
    }
}
}

class MacToolbar : public INativeToolbar {
public:
    ~MacToolbar() override { Uninstall(); }
    bool Install(Platform::Window* /*window*/) override {
        if (!m_MenuBar) {
            m_PreviousMenu = [[NSApp mainMenu] retain];
            m_PreviousServicesMenu = [[NSApp servicesMenu] retain];
            m_MenuBar = [[NSMenu alloc] initWithTitle:@"MainMenu"];
            [NSApp setMainMenu:m_MenuBar];
            InsertAppMenuPlaceholder();
        } else {
            Clear();
        }
        // Ensure target routes to this instance
        [GE_MacToolbarTarget shared].toolbar = this;
        return true;
    }

    void Uninstall() override {
        m_Callback = nullptr;
        if ([GE_MacToolbarTarget shared].toolbar == this)
            [GE_MacToolbarTarget shared].toolbar = nullptr;
        if ([NSApp servicesMenu] == m_ServicesMenu)
            [NSApp setServicesMenu:m_PreviousServicesMenu];
        if (m_MenuBar) {
            if ([NSApp mainMenu] == m_MenuBar)
                [NSApp setMainMenu:m_PreviousMenu];
            [m_MenuBar release];
            m_MenuBar = nil;
        }
        [m_PreviousMenu release];
        m_PreviousMenu = nil;
        [m_PreviousServicesMenu release];
        m_PreviousServicesMenu = nil;
        m_ServicesMenu = nil;
        m_QuitItem = nil;
        m_Menus.Clear();
        m_Items.Clear();
    }

    void Clear() override {
        if (!m_MenuBar) return;
        while ([m_MenuBar numberOfItems] > 0) {
            [m_MenuBar removeItemAtIndex:0];
        }
        m_Menus.Clear();
        m_NextMenuId = 1;
        m_Items.Clear();
        InsertAppMenuPlaceholder();
    }

    // macOS treats the first menu item as the application menu (label = app name).
    // Populate it with the standard Cocoa items (About, Services, Hide, Quit) so
    // subsequent AddMenu("Edit") calls land as real "Edit" entries and the user
    // keeps Cmd-Q, Hide, etc. that GLFW's default menu bar would otherwise have.
    void InsertAppMenuPlaceholder() {
        if (!m_MenuBar) return;

        NSString* appName = [[NSProcessInfo processInfo] processName];

        NSMenuItem* appItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@""];

        NSString* aboutTitle = [@"About " stringByAppendingString:appName];
        [appMenu addItemWithTitle:aboutTitle
                           action:@selector(orderFrontStandardAboutPanel:)
                    keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* servicesItem = [[NSMenuItem alloc] initWithTitle:@"Services"
                                                              action:nil
                                                       keyEquivalent:@""];
        NSMenu* servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
        m_ServicesMenu = servicesMenu;
        [servicesItem setSubmenu:servicesMenu];
        [appMenu addItem:servicesItem];
        [NSApp setServicesMenu:servicesMenu];
        [servicesMenu release];
        [servicesItem release];
        [appMenu addItem:[NSMenuItem separatorItem]];

        NSString* hideTitle = [@"Hide " stringByAppendingString:appName];
        [appMenu addItemWithTitle:hideTitle
                           action:@selector(hide:)
                    keyEquivalent:@"h"];

        NSMenuItem* hideOthersItem = [appMenu addItemWithTitle:@"Hide Others"
                                                        action:@selector(hideOtherApplications:)
                                                 keyEquivalent:@"h"];
        [hideOthersItem setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];

        [appMenu addItemWithTitle:@"Show All"
                           action:@selector(unhideAllApplications:)
                    keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];

        NSString* quitTitle = [@"Quit " stringByAppendingString:appName];
        m_QuitItem = [appMenu addItemWithTitle:quitTitle
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];

        [appItem setSubmenu:appMenu];
        [m_MenuBar addItem:appItem];
        [appMenu release];
        [appItem release];
    }

    void SetCommandHandler(CommandCallback cb) override {
        m_Callback = std::move(cb);
        [GE_MacToolbarTarget shared].toolbar = this;
    }

    bool SetApplicationQuitCommand(uint32_t commandId) override {
        if (!m_QuitItem) return false;
        [m_QuitItem setTarget:[GE_MacToolbarTarget shared]];
        [m_QuitItem setAction:@selector(onMenuItem:)];
        [m_QuitItem setTag:static_cast<NSInteger>(commandId)];
        m_Items.InsertOrAssign(commandId, m_QuitItem);
        return true;
    }

    uint32_t AddMenu(const std::string& title) override {
        if (!m_MenuBar) return 0;
        NSString* t = [NSString stringWithUTF8String:title.c_str()];
        NSMenuItem* topItem = [[NSMenuItem alloc] initWithTitle:t action:nil keyEquivalent:@""];
        NSMenu* sub = [[NSMenu alloc] initWithTitle:t];
        [topItem setSubmenu:sub];
        [m_MenuBar addItem:topItem];
        uint32_t id = m_NextMenuId++;
        m_Menus.InsertOrAssign(id, sub);
        [sub release];
        [topItem release];
        return id;
    }

    uint32_t AddSubMenu(uint32_t parentMenuId, const std::string& title) override {
        NSMenu* parent = GetMenuById(parentMenuId);
        if (!parent) return 0;
        NSString* t = [NSString stringWithUTF8String:title.c_str()];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:t action:nil keyEquivalent:@""];
        NSMenu* sub = [[NSMenu alloc] initWithTitle:t];
        [item setSubmenu:sub];
        [parent addItem:item];
        uint32_t id = m_NextMenuId++;
        m_Menus.InsertOrAssign(id, sub);
        [sub release];
        [item release];
        return id;
    }

    void AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId) override {
        NSMenu* parent = GetMenuById(parentMenuId);
        if (!parent) return;
        NSString* t = [NSString stringWithUTF8String:title.c_str()];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:t action:@selector(onMenuItem:) keyEquivalent:@""];
        ApplyItemTitle(item, title);
        [item setTarget:[GE_MacToolbarTarget shared]];
        [item setTag:(NSInteger)commandId];
        [parent addItem:item];
        m_Items.InsertOrAssign(commandId, item);
        [item release];
    }

    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override {
        auto* item = m_Items.Find(commandId);
        if (!item || !*item || imagePath.empty()) return;

        NSString* path = ResolveIconPath(imagePath);
        if (!path) return;
        NSImage* image = [[NSImage alloc] initWithContentsOfFile:path];
        if (!image) return;
        [image setSize:NSMakeSize(16.0, 16.0)];
        [*item setImage:image];
        [image release];
    }

    void UpdateItemTitle(uint32_t commandId, const std::string& title) override {
        auto* found = m_Items.Find(commandId);
        if (!found) return;
        NSMenuItem* item = *found;
        if (!item) return;
        ApplyItemTitle(item, title);
    }

    void Dispatch(uint32_t commandId) {
        auto callback = m_Callback;
        if (callback) callback(commandId);
    }

private:
    static NSString* ResolveIconPath(const std::string& imagePath) {
        std::string path = imagePath;
        bool editorAlias = false;
        constexpr const char* kEditorColon = "editor:";
        constexpr const char* kEditorSlash = "@editor/";
        if (path.rfind(kEditorColon, 0) == 0) {
            path = path.substr(std::char_traits<char>::length(kEditorColon));
            editorAlias = true;
        } else if (path.rfind(kEditorSlash, 0) == 0) {
            path = path.substr(std::char_traits<char>::length(kEditorSlash));
            editorAlias = true;
        }

        NSString* nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsPath) return nil;
        if ([nsPath isAbsolutePath]) return [nsPath stringByStandardizingPath];

        NSString* base = [[NSBundle mainBundle] resourcePath];
        if (!base) base = [[NSBundle mainBundle] bundlePath];
        if (!base) return nil;
        if (editorAlias)
            base = [base stringByAppendingPathComponent:@"Assets"];
        return [[base stringByAppendingPathComponent:nsPath] stringByStandardizingPath];
    }

    NSMenu* GetMenuById(uint32_t id) const {
        auto* menu = m_Menus.Find(id);
        return menu ? *menu : nil;
    }

    NSMenu* m_MenuBar = nil;
    NSMenu* m_PreviousMenu = nil;
    NSMenu* m_PreviousServicesMenu = nil;
    NSMenu* m_ServicesMenu = nil;
    NSMenuItem* m_QuitItem = nil;
    FlatMap<uint32_t, NSMenu*> m_Menus;
    FlatMap<uint32_t, NSMenuItem*> m_Items;
    uint32_t m_NextMenuId = 1;
    CommandCallback m_Callback;
};

std::unique_ptr<INativeToolbar> CreateNativeToolbar() {
    return std::make_unique<MacToolbar>();
}

} // namespace GameEngine

@implementation GE_MacToolbarTarget

+ (instancetype)shared {
    static GE_MacToolbarTarget* sInst = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{ sInst = [GE_MacToolbarTarget new]; });
    return sInst;
}

- (IBAction)onMenuItem:(id)sender {
    NSInteger tag = [sender tag];
    if (self.toolbar) { self.toolbar->Dispatch((uint32_t)tag); }
}

@end

#endif // __APPLE__
