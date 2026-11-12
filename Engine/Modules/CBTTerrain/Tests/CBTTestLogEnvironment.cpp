// The Logger for the whole CBTTerrainTests process: initialized once before the first test, so a
// Logger line prints in a filtered run too. A test that captures the log installs its own sinks
// (ScopedLogCapture) and puts the default back when it ends.
#include "Logger/Logger.h"

#include <gtest/gtest.h>

namespace
{
class CBTTestLogEnvironment : public ::testing::Environment
{
  public:
    void SetUp() override { Logger::Log::Initialize({}); }
};

// GTest takes ownership and runs SetUp() before the first test.
[[maybe_unused]] const ::testing::Environment* const g_CBTTestLogEnvironment =
    ::testing::AddGlobalTestEnvironment(new CBTTestLogEnvironment());
} // namespace
