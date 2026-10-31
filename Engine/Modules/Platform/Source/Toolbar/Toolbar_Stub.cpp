#include "ClientToolbar.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <memory>

namespace GameEngine {

std::unique_ptr<INativeToolbar> CreateNativeToolbar() {
    return std::make_unique<ClientToolbar>();
}

} // namespace GameEngine

#endif // !_WIN32 && !__APPLE__
