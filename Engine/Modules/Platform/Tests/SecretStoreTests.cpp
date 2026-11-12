// Platform::StoreSecret, ReadSecret and DeleteSecret against the real store of this system,
// under a name unique to the run, deleted at the end. Every value is a throwaway made here.
// Where the system has no store, the store tests skip with that reason.

#include "Platform/SecretStore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using namespace GameEngine::Platform;

namespace
{
// A name no other run uses, so a run never reads or deletes a secret it did not store.
std::string UniqueName(std::string_view what)
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return "GameEngine/PlatformSecretStoreTests/" + std::string(what) + "-" + std::to_string(ticks);
}

// Deletes the secret at the end of the test, whatever the test's assertions did.
class ScopedSecretName
{
public:
    explicit ScopedSecretName(std::string name) : Name(std::move(name)) {}
    ~ScopedSecretName() { DeleteSecret(Name); }
    ScopedSecretName(const ScopedSecretName&) = delete;
    ScopedSecretName& operator=(const ScopedSecretName&) = delete;

    const std::string Name;
};

bool HasStore()
{
    return ReadSecret(UniqueName("probe")).Status != SecretStoreStatus::NoStore;
}
} // namespace

// The rules run before the store is called, so they give Failed on every system, with or
// without a store. A pair that the rules let through by mistake is deleted at the end.
TEST(PlatformSecretStoreTests, TheRulesRefuseAValueOverTheCapAnEmptyOrLongNameAndANul)
{
    const ScopedSecretName secret(UniqueName("rules"));
    const ScopedSecretName longName(secret.Name + std::string(256, 'n'));

    EXPECT_EQ(StoreSecret(secret.Name, std::string(kMaxSecretBytes + 1, 'k')), SecretStoreStatus::Failed);
    EXPECT_EQ(StoreSecret(longName.Name, "throwaway"), SecretStoreStatus::Failed);
    EXPECT_EQ(StoreSecret("", "throwaway"), SecretStoreStatus::Failed);
    EXPECT_EQ(StoreSecret(secret.Name + std::string("\0x", 2), "throwaway"), SecretStoreStatus::Failed);
    EXPECT_EQ(StoreSecret(secret.Name, std::string_view("va\0lue", 6)), SecretStoreStatus::Failed);
}

TEST(PlatformSecretStoreTests, AValueOfTheCapsSizeIsStoredWhole)
{
    if (!HasStore())
        GTEST_SKIP() << "This system has no secret store (SecretStoreStatus::NoStore)";
    const ScopedSecretName secret(UniqueName("cap"));

    ASSERT_EQ(StoreSecret(secret.Name, std::string(kMaxSecretBytes, 'k')), SecretStoreStatus::Done);
    EXPECT_EQ(ReadSecret(secret.Name).Value.size(), kMaxSecretBytes);
}

TEST(PlatformSecretStoreTests, AStoredSecretReadsBackIsReplacedAndIsGoneAfterDelete)
{
    if (!HasStore())
        GTEST_SKIP() << "This system has no secret store (SecretStoreStatus::NoStore)";
    const ScopedSecretName secret(UniqueName("round-trip"));

    ASSERT_EQ(ReadSecret(secret.Name).Status, SecretStoreStatus::NotFound);
    ASSERT_EQ(StoreSecret(secret.Name, "throwaway-first"), SecretStoreStatus::Done);
    SecretReadResult read = ReadSecret(secret.Name);
    ASSERT_EQ(read.Status, SecretStoreStatus::Done);
    EXPECT_EQ(read.Value, "throwaway-first");

    ASSERT_EQ(StoreSecret(secret.Name, "throwaway-second"), SecretStoreStatus::Done);
    read = ReadSecret(secret.Name);
    ASSERT_EQ(read.Status, SecretStoreStatus::Done);
    EXPECT_EQ(read.Value, "throwaway-second") << "a second store replaces the value";

    EXPECT_EQ(DeleteSecret(secret.Name), SecretStoreStatus::Done);
    read = ReadSecret(secret.Name);
    EXPECT_EQ(read.Status, SecretStoreStatus::NotFound);
    EXPECT_TRUE(read.Value.empty());
    EXPECT_EQ(DeleteSecret(secret.Name), SecretStoreStatus::NotFound);
}

TEST(PlatformSecretStoreTests, AValueOfTheCapsSizeIsKeptWholeAndALongerOneIsRefusedWithoutAWrite)
{
    if (!HasStore())
        GTEST_SKIP() << "This system has no secret store (SecretStoreStatus::NoStore)";
    const ScopedSecretName secret(UniqueName("cap"));
    std::string value;
    for (size_t index = 0; index < kMaxSecretBytes; ++index)
        value += static_cast<char>('a' + index % 26);

    ASSERT_EQ(StoreSecret(secret.Name, value), SecretStoreStatus::Done);
    const SecretReadResult read = ReadSecret(secret.Name);
    ASSERT_EQ(read.Status, SecretStoreStatus::Done);
    EXPECT_EQ(read.Value, value);

    EXPECT_EQ(StoreSecret(secret.Name, value + "z"), SecretStoreStatus::Failed);
    EXPECT_EQ(ReadSecret(secret.Name).Value, value) << "a refused value leaves the stored one";
}

TEST(PlatformSecretStoreTests, NonAsciiTextIsKeptByteForByte)
{
    if (!HasStore())
        GTEST_SKIP() << "This system has no secret store (SecretStoreStatus::NoStore)";
    const ScopedSecretName secret(UniqueName("utf8-\xC3\xA9"));
    const std::string value = "throwaway-\xE2\x80\xA2-\xF0\x9F\x94\x91";

    ASSERT_EQ(StoreSecret(secret.Name, value), SecretStoreStatus::Done);
    EXPECT_EQ(ReadSecret(secret.Name).Value, value);
}

TEST(PlatformSecretStoreTests, TheStoreHasANameWhereAStoreExists)
{
    if (!HasStore())
    {
        EXPECT_TRUE(SecretStoreName().empty());
        return;
    }
    EXPECT_FALSE(SecretStoreName().empty());
#if defined(_WIN32)
    EXPECT_EQ(SecretStoreName(), "Windows Credential Manager");
#endif
}

TEST(PlatformSecretStoreTests, AnInvalidNameFailsEveryCallWithoutTouchingTheStore)
{
    if (!HasStore())
        GTEST_SKIP() << "This system has no secret store (SecretStoreStatus::NoStore)";
    EXPECT_EQ(StoreSecret("", "throwaway"), SecretStoreStatus::Failed);
    EXPECT_EQ(ReadSecret("").Status, SecretStoreStatus::Failed);
    EXPECT_EQ(DeleteSecret(UniqueName("long") + std::string(256, 'n')), SecretStoreStatus::Failed);
}
