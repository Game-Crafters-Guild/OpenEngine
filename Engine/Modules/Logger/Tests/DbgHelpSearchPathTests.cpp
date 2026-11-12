// DbgHelp reads the PDB of a module only from the folders of the loaded modules.
// Each case loads a copy of DbgHelpSearchPathProbe.dll from a new folder. The PDB
// path in the copy names the PDB in the build tree, which DbgHelp must not open.

#include "Logger/Backtrace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace
{

namespace fs = std::filesystem;

constexpr const wchar_t* kProbeName = L"DbgHelpSearchPathProbe";

class DbgHelpSearchPathTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        wchar_t executablePath[MAX_PATH];
        ASSERT_GT(GetModuleFileNameW(nullptr, executablePath, MAX_PATH), 0u);
        m_BuildFolder = fs::path(executablePath).parent_path() / kProbeName;
        m_Folder = fs::temp_directory_path() /
                   ("ge_dbghelp_search_path_" + std::to_string(GetCurrentProcessId()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(m_Folder, ec);
        ASSERT_TRUE(fs::create_directories(m_Folder, ec)) << ec.message();
    }

    void TearDown() override
    {
        if (m_Module)
        {
            {
                std::lock_guard<std::mutex> dbghelpLock(Logger::DbgHelpMutex());
                SymUnloadModule64(GetCurrentProcess(), reinterpret_cast<DWORD64>(m_Module));
            }
            FreeLibrary(m_Module);
        }
        std::error_code ec;
        fs::remove_all(m_Folder, ec);
    }

    void CopyFromBuild(const wchar_t* extension)
    {
        const std::wstring name = std::wstring(kProbeName) + extension;
        std::error_code ec;
        ASSERT_TRUE(fs::copy_file(m_BuildFolder / name, m_Folder / name, ec)) << ec.message();
    }

    // Loads the copied probe without running its code, symbolizes one export and
    // returns the PDB that DbgHelp opened for it, or an empty path.
    fs::path LoadedPdbOfCopy()
    {
        m_Module = LoadLibraryExW((m_Folder / (std::wstring(kProbeName) + L".dll")).c_str(), nullptr,
                                  DONT_RESOLVE_DLL_REFERENCES);
        EXPECT_NE(m_Module, nullptr);
        if (!m_Module)
            return {};
        const auto address = reinterpret_cast<DWORD64>(GetProcAddress(m_Module, "DbgHelpSearchPathProbe"));
        EXPECT_NE(address, 0u);

        std::lock_guard<std::mutex> dbghelpLock(Logger::DbgHelpMutex());
        Logger::LoadDbgHelpModules();
        alignas(SYMBOL_INFOW) char symbolBuffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)];
        auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(symbolBuffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = MAX_SYM_NAME;
        EXPECT_TRUE(SymFromAddrW(GetCurrentProcess(), address, nullptr, symbol));

        IMAGEHLP_MODULEW64 module{};
        module.SizeOfStruct = sizeof(module);
        EXPECT_TRUE(SymGetModuleInfoW64(GetCurrentProcess(), reinterpret_cast<DWORD64>(m_Module), &module));
        return fs::path(module.LoadedPdbName);
    }

    fs::path m_BuildFolder;
    fs::path m_Folder;
    HMODULE m_Module = nullptr;
};

TEST_F(DbgHelpSearchPathTest, ModuleSymbolizesFromThePdbInItsOwnFolder)
{
    CopyFromBuild(L".dll");
    CopyFromBuild(L".pdb");

    const fs::path loadedPdb = LoadedPdbOfCopy();

    std::error_code ec;
    EXPECT_TRUE(fs::equivalent(loadedPdb, m_Folder / (std::wstring(kProbeName) + L".pdb"), ec))
        << "DbgHelp opened '" << loadedPdb.string() << "'";
}

TEST_F(DbgHelpSearchPathTest, ModuleWithNoPdbInItsFolderOpensNoPdb)
{
    CopyFromBuild(L".dll");

    const fs::path loadedPdb = LoadedPdbOfCopy();

    EXPECT_TRUE(loadedPdb.empty()) << "DbgHelp opened '" << loadedPdb.string() << "'";
}

} // namespace
