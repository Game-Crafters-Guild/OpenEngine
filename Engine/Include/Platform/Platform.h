#pragma once

#include "Types/Types.h"

namespace GameEngine {

/**
 * @brief Platform-specific functionality (placeholder)
 */
class Platform {
public:
    /**
     * @brief Initialize platform-specific systems
     */
    static bool Initialize();

    /**
     * @brief Shutdown platform-specific systems
     */
    static void Shutdown();

    /**
     * @brief Get platform name
     */
    static String GetPlatformName();

    /**
     * @brief Get total system memory in bytes
     */
    static uint64 GetTotalSystemMemory();

private:
    static bool s_Initialized;
};

} // namespace GameEngine
