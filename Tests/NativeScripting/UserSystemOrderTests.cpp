// The order user systems run in: ascending Order (from a SystemBase's `static constexpr int Order`,
// 0 without one), systems of equal order by name, for every hook and across modules.
#include <gtest/gtest.h>

#include "ECS/World.h"
#include "GameSDK/System.h"
#include "NativeScripting/UserSystemRegistry.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace GameEngine;
namespace ns = GameEngine::NativeScripting;

namespace
{
std::vector<std::string>& Calls()
{
    static std::vector<std::string> calls;
    return calls;
}

#ifdef _WIN32
// Read through a volatile so the deliberate null write is neither constant-folded nor flagged.
volatile std::uintptr_t g_FaultAddress = 0;
#endif

struct Recorder : ns::IUserSystem
{
    Recorder(const char* name, int order) : m_Name(name), m_Order(order) {}
    void OnStart(ECS::World&) override { Calls().push_back(std::string("start ") + m_Name); }
    void OnUpdate(ECS::World&, float) override
    {
        Calls().push_back(std::string("update ") + m_Name);
#ifdef _WIN32
        if (Faults)
            *reinterpret_cast<volatile int*>(g_FaultAddress) = 0x0BADC0DE; // access violation
#endif
    }
    void OnDestroy(ECS::World&) override { Calls().push_back(std::string("destroy ") + m_Name); }
    const char* Name() const override { return m_Name; }
    int Order() const override { return m_Order; }

    bool Faults = false;

  private:
    const char* m_Name;
    int m_Order;
};

struct Unordered : ECS::SystemBase
{
    void OnUpdate(ECS::World&, float) {}
};
struct Late : ECS::SystemBase
{
    static constexpr int Order = 20;
    void OnUpdate(ECS::World&, float) {}
};
struct Early : ECS::SystemBase
{
    static constexpr int Order = -5;
};

// Declared forms of Order: the adapter accepts an int constant and refuses any other member named Order.
struct Seven : ECS::SystemBase
{
    static constexpr int Order = 7;
};
struct ConstInt : ECS::SystemBase
{
    static const int Order = 4;
};
struct Inherited : Late
{
};
struct FinalSeven final : ECS::SystemBase
{
    static constexpr int Order = 7;
};
struct Overloaded : ECS::SystemBase
{
    static int Order();
    static int Order(int);
};
struct Templated : ECS::SystemBase
{
    template <class>
    static int Order();
};
struct Enumerator : ECS::SystemBase
{
    enum
    {
        Order = 5
    };
};
struct NestedType : ECS::SystemBase
{
    enum class Order
    {
        Early
    };
};
static_assert(!ECS::Detail::DeclaresOrder<Unordered>);
static_assert(ECS::Detail::DeclaresIntOrder<Seven> && ECS::Detail::DeclaresIntOrder<ConstInt> &&
              ECS::Detail::DeclaresIntOrder<Inherited> && ECS::Detail::DeclaresIntOrder<FinalSeven>);
static_assert(ECS::Detail::DeclaresOrder<Overloaded> && !ECS::Detail::DeclaresIntOrder<Overloaded>);
static_assert(ECS::Detail::DeclaresOrder<Templated> && !ECS::Detail::DeclaresIntOrder<Templated>);
static_assert(ECS::Detail::DeclaresOrder<Enumerator> && !ECS::Detail::DeclaresIntOrder<Enumerator>);
static_assert(ECS::Detail::DeclaresOrder<NestedType> && !ECS::Detail::DeclaresIntOrder<NestedType>);

class UserSystemOrder : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ns::ClearUserSystems();
        ns::SetActiveRegistrationModule({});
        Calls().clear();
    }
    void TearDown() override
    {
        ns::ClearUserSystems();
        ns::SetActiveRegistrationModule({});
    }

    static std::vector<std::string> Names()
    {
        std::vector<std::string> names;
        for (const ns::UserSystemEntry& e : ns::GetUserSystems())
            names.push_back(e.System ? e.System->Name() : "(disabled)");
        return names;
    }
};
} // namespace

TEST_F(UserSystemOrder, AdapterReadsTheStructsOrderAndDefaultsToZero)
{
    EXPECT_EQ(ECS::UserSystemAdapter<Unordered>("Unordered").Order(), 0);
    EXPECT_EQ(ECS::UserSystemAdapter<Late>("Late").Order(), 20);
    EXPECT_EQ(ECS::UserSystemAdapter<Early>("Early").Order(), -5);
    EXPECT_EQ(ECS::UserSystemAdapter<ConstInt>("ConstInt").Order(), 4);
    EXPECT_EQ(ECS::UserSystemAdapter<Inherited>("Inherited").Order(), 20);
    EXPECT_EQ(ECS::UserSystemAdapter<FinalSeven>("FinalSeven").Order(), 7);
}

TEST_F(UserSystemOrder, ASystemWithoutANameIsRefused)
{
    Recorder unnamed(nullptr, 0);
    EXPECT_FALSE(ns::RegisterUserSystem(&unnamed));
    EXPECT_TRUE(ns::GetUserSystems().empty());
}

TEST_F(UserSystemOrder, RegistryRunsByOrderThenName)
{
    // Names compare in character order: capital letters before lowercase.
    Recorder rules("Rules", 30), flight("Flight", 0), turrets("Turrets", 10), sound("Sound", 0), input("Input", -10),
        audio("audio", 0);
    for (Recorder* r : {&rules, &audio, &sound, &turrets, &flight, &input})
        ns::RegisterUserSystem(r);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Input", "Flight", "Sound", "audio", "Turrets", "Rules"}));

    ECS::World world;
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, 1.0f / 60.0f);
    ns::StopUserSystems(world);
    EXPECT_EQ(Calls(), (std::vector<std::string>{
                           "start Input", "start Flight", "start Sound", "start audio", "start Turrets", "start Rules",
                           "update Input", "update Flight", "update Sound", "update audio", "update Turrets",
                           "update Rules", "destroy Input", "destroy Flight", "destroy Sound", "destroy audio",
                           "destroy Turrets", "destroy Rules"}));
}

TEST_F(UserSystemOrder, ReloadingOneModuleKeepsTheOrderAcrossModules)
{
    Recorder flight("Flight", 0), rules("Rules", 30), packageEarly("PackageEarly", -10), packageMid("PackageMid", 15);
    ns::SetActiveRegistrationModule("UserScripts");
    ns::RegisterUserSystem(&flight);
    ns::RegisterUserSystem(&rules);
    ns::SetActiveRegistrationModule("Package");
    ns::RegisterUserSystem(&packageEarly);
    ns::RegisterUserSystem(&packageMid);
    EXPECT_EQ(Names(), (std::vector<std::string>{"PackageEarly", "Flight", "PackageMid", "Rules"}));

    // The project's module reloads: its systems leave and register again, in between the package's.
    ns::ClearUserSystems("UserScripts");
    EXPECT_EQ(Names(), (std::vector<std::string>{"PackageEarly", "PackageMid"}));
    ns::SetActiveRegistrationModule("UserScripts");
    ns::RegisterUserSystem(&flight);
    ns::RegisterUserSystem(&rules);
    EXPECT_EQ(Names(), (std::vector<std::string>{"PackageEarly", "Flight", "PackageMid", "Rules"}));
}

TEST_F(UserSystemOrder, EqualOrdersKeepTheirPlacesWhicheverModuleReloads)
{
    Recorder flight("Flight", 0), packageMover("PackageMover", 0);
    ns::SetActiveRegistrationModule("UserScripts");
    ns::RegisterUserSystem(&flight);
    ns::SetActiveRegistrationModule("Package");
    ns::RegisterUserSystem(&packageMover);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Flight", "PackageMover"}));

    // The project's module reloads after the package's: its system registers last and still runs first.
    ns::ClearUserSystems("UserScripts");
    ns::SetActiveRegistrationModule("UserScripts");
    ns::RegisterUserSystem(&flight);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Flight", "PackageMover"}));
}

#ifdef _WIN32
TEST_F(UserSystemOrder, ADisabledSystemKeepsItsPlace)
{
    Recorder turrets("Turrets", 0), rules("Rules", 10);
    turrets.Faults = true;
    ns::RegisterUserSystem(&turrets);
    ns::RegisterUserSystem(&rules);

    ECS::World world;
    ns::TickUserSystems(world, 1.0f / 60.0f); // Turrets hard-faults and is disabled
    ASSERT_EQ(Names(), (std::vector<std::string>{"(disabled)", "Rules"}));

    Recorder sound("Sound", 0), weather("Weather", 0);
    ns::RegisterUserSystem(&weather);
    ns::RegisterUserSystem(&sound);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Sound", "(disabled)", "Weather", "Rules"}));
}
#endif
