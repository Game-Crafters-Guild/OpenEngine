#include "SecretStore/SystemSecretStore.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>


namespace GameEngine::Platform
{
namespace
{
// Releases a Core Foundation object at the end of its scope.
class CFOwner
{
public:
    explicit CFOwner(CFTypeRef object) : m_Object(object) {}
    ~CFOwner()
    {
        if (m_Object)
            CFRelease(m_Object);
    }
    CFOwner(const CFOwner&) = delete;
    CFOwner& operator=(const CFOwner&) = delete;

private:
    CFTypeRef m_Object;
};

CFStringRef MakeString(std::string_view text)
{
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                   static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
}

// The query of one generic password: the service and the account are both the secret's name.
CFMutableDictionaryRef MakeQuery(CFStringRef name)
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, name);
    CFDictionarySetValue(query, kSecAttrAccount, name);
    return query;
}

SecretStoreStatus StatusOf(OSStatus status)
{
    switch (status)
    {
    case errSecSuccess:
        return SecretStoreStatus::Done;
    case errSecItemNotFound:
        return SecretStoreStatus::NotFound;
    case errSecNotAvailable:
    case errSecNoSuchKeychain:
    case errSecInteractionNotAllowed:
        return SecretStoreStatus::NoStore;
    default:
        return SecretStoreStatus::Failed;
    }
}
} // namespace

// A generic password in the login Keychain. Its access list holds the binary that saved it, so
// a binary signed differently can make the Keychain ask the user once.
SecretStoreStatus SystemStoreSecret(std::string_view name, std::string_view value)
{
    CFStringRef nameString = MakeString(name);
    if (!nameString)
        return SecretStoreStatus::Failed;
    CFOwner nameOwner(nameString);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(value.data()),
                                  static_cast<CFIndex>(value.size()));
    CFOwner dataOwner(data);
    CFMutableDictionaryRef query = MakeQuery(nameString);
    CFOwner queryOwner(query);

    CFMutableDictionaryRef update = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks);
    CFOwner updateOwner(update);
    CFDictionarySetValue(update, kSecValueData, data);
    const OSStatus updated = SecItemUpdate(query, update);
    if (updated != errSecItemNotFound)
        return StatusOf(updated);

    CFDictionarySetValue(query, kSecValueData, data);
    CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleWhenUnlocked);
    const SecretStoreStatus added = StatusOf(SecItemAdd(query, nullptr));
    return added == SecretStoreStatus::NotFound ? SecretStoreStatus::Failed : added;
}

SecretReadResult SystemReadSecret(std::string_view name)
{
    CFStringRef nameString = MakeString(name);
    if (!nameString)
        return {.Status = SecretStoreStatus::Failed};
    CFOwner nameOwner(nameString);
    CFMutableDictionaryRef query = MakeQuery(nameString);
    CFOwner queryOwner(query);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef found = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &found);
    CFOwner foundOwner(found);
    if (status != errSecSuccess)
        return {.Status = StatusOf(status)};
    if (!found || CFGetTypeID(found) != CFDataGetTypeID())
        return {.Status = SecretStoreStatus::Failed};
    const auto data = static_cast<CFDataRef>(found);
    SecretReadResult result{.Status = SecretStoreStatus::Done};
    result.Value.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                        static_cast<size_t>(CFDataGetLength(data)));
    return result;
}

SecretStoreStatus SystemDeleteSecret(std::string_view name)
{
    CFStringRef nameString = MakeString(name);
    if (!nameString)
        return SecretStoreStatus::Failed;
    CFOwner nameOwner(nameString);
    CFMutableDictionaryRef query = MakeQuery(nameString);
    CFOwner queryOwner(query);
    return StatusOf(SecItemDelete(query));
}

std::string_view SystemSecretStoreName()
{
    return "Keychain";
}

} // namespace GameEngine::Platform
