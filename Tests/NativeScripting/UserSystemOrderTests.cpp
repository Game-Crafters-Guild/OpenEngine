// The order user systems run in: ascending Order (from a SystemBase's `static constexpr int Order`,
// 0 without one), systems of equal order by name, for every hook and across modules, whatever the
// order the modules load and reload in.
#include <gtest/gtest.h>

#include "ECS/World.h"
#include "GameSDK/System.h"
#include "NativeScripting/UserSystemRegistry.h"

#include <cstddef>
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

struct Recorder : ns::IUserSystem
{
    Recorder(const char* name, int order) : m_Name(name), m_Order(order) {}
    void OnStart(ECS::World&) override { Calls().push_back(std::string("start ") + m_Name); }
    void OnUpdate(ECS::World&, float) override { Calls().push_back(std::string("update ") + m_Name); }
    void OnDestroy(ECS::World&) override { Calls().push_back(std::string("destroy ") + m_Name); }
    const char* Name() const override { return m_Name; }
    int Order() const override { return m_Order; }

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

// Misdeclared orders, each a compile error in UserSystemAdapter<T>::Order().
struct FloatOrder : ECS::SystemBase
{
    static constexpr float Order = 0.75f;
};
struct FunctionOrder : ECS::SystemBase
{
    static int Order() { return 10; }
};
struct UnsignedOrder : ECS::SystemBase
{
    static constexpr unsigned Order = 3000000000u;
};
struct MutableOrder : ECS::SystemBase
{
    static inline int Order = 10;
};
struct MemberOrder : ECS::SystemBase
{
    int Order = 10;
};
static_assert(ECS::Detail::DeclaresIntOrder<Late>);
static_assert(!ECS::Detail::DeclaresOrder<Unordered>);
static_assert(ECS::Detail::DeclaresOrder<FloatOrder> && !ECS::Detail::DeclaresIntOrder<FloatOrder>);
static_assert(ECS::Detail::DeclaresOrder<FunctionOrder> && !ECS::Detail::DeclaresIntOrder<FunctionOrder>);
static_assert(ECS::Detail::DeclaresOrder<UnsignedOrder> && !ECS::Detail::DeclaresIntOrder<UnsignedOrder>);
static_assert(ECS::Detail::DeclaresOrder<MutableOrder> && !ECS::Detail::DeclaresIntOrder<MutableOrder>);
static_assert(ECS::Detail::DeclaresOrder<MemberOrder> && !ECS::Detail::DeclaresIntOrder<MemberOrder>);

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
            names.push_back(e.System ? e.Name : "(disabled " + e.Name + ")");
        return names;
    }

    // What GuardedUserInvoke does to a system that hard-faults (Windows only, so not reachable
    // from a portable test): null its slot and leave the entry in place.
    static void Disable(std::size_t index)
    {
        const_cast<ns::UserSystemEntry&>(ns::GetUserSystems()[index]).System = nullptr;
    }
};
} // namespace

TEST_F(UserSystemOrder, AdapterReadsTheStructsOrderAndDefaultsToZero)
{
    EXPECT_EQ(ECS::UserSystemAdapter<Unordered>("Unordered").Order(), 0);
    EXPECT_EQ(ECS::UserSystemAdapter<Late>("Late").Order(), 20);
    EXPECT_EQ(ECS::UserSystemAdapter<Early>("Early").Order(), -5);
}

TEST_F(UserSystemOrder, RegistryRunsByOrderThenName)
{
    Recorder rules("Rules", 30), flight("Flight", 0), turrets("Turrets", 10), sound("Sound", 0), input("Input", -10);
    for (Recorder* r : {&rules, &sound, &turrets, &flight, &input})
        ns::RegisterUserSystem(r);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Input", "Flight", "Sound", "Turrets", "Rules"}));

    ECS::World world;
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, 1.0f / 60.0f);
    ns::StopUserSystems(world);
    EXPECT_EQ(Calls(), (std::vector<std::string>{
                           "start Input", "start Flight", "start Sound", "start Turrets", "start Rules",
                           "update Input", "update Flight", "update Sound", "update Turrets", "update Rules",
                           "destroy Input", "destroy Flight", "destroy Sound", "destroy Turrets", "destroy Rules"}));
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

TEST_F(UserSystemOrder, EqualOrdersAcrossModulesDoNotDependOnLoadOrReloadOrder)
{
    Recorder flight("Game::Flight", 0), mover("Ocean::PackageMover", 0);
    // The package loads first, the project second: the order is still by name.
    ns::SetActiveRegistrationModule("Package");
    ns::RegisterUserSystem(&mover);
    ns::SetActiveRegistrationModule("UserScripts");
    ns::RegisterUserSystem(&flight);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Game::Flight", "Ocean::PackageMover"}));

    // Rebuilding either module leaves it where it was.
    ns::ClearUserSystems("UserScripts");
    ns::RegisterUserSystem(&flight);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Game::Flight", "Ocean::PackageMover"}));
    ns::ClearUserSystems("Package");
    ns::SetActiveRegistrationModule("Package");
    ns::RegisterUserSystem(&mover);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Game::Flight", "Ocean::PackageMover"}));
}

TEST_F(UserSystemOrder, EqualOrderAndNameRunByModuleId)
{
    Recorder project("Shared::Mover", 0), package("Shared::Mover", 0);
    ns::SetActiveRegistrationModule("UserScripts");
    ns::RegisterUserSystem(&project);
    ns::SetActiveRegistrationModule("OceanPack");
    ns::RegisterUserSystem(&package);

    const std::vector<ns::UserSystemEntry>& systems = ns::GetUserSystems();
    ASSERT_EQ(systems.size(), 2u);
    EXPECT_EQ(systems[0].ModuleId, "OceanPack");
    EXPECT_EQ(systems[1].ModuleId, "UserScripts");
}

TEST_F(UserSystemOrder, ADisabledSystemKeepsItsPlace)
{
    Recorder alpha("Alpha", 0), charlie("Charlie", 0), late("Late", 10);
    for (Recorder* r : {&alpha, &charlie, &late})
        ns::RegisterUserSystem(r);
    Disable(0);

    // A system registered after the fault still lands by Order and name around the disabled one.
    Recorder bravo("Bravo", 0), early("Early", -10);
    ns::RegisterUserSystem(&bravo);
    ns::RegisterUserSystem(&early);
    EXPECT_EQ(Names(),
              (std::vector<std::string>{"Early", "(disabled Alpha)", "Bravo", "Charlie", "Late"}));

    ECS::World world;
    ns::TickUserSystems(world, 1.0f / 60.0f);
    EXPECT_EQ(Calls(), (std::vector<std::string>{"update Early", "update Bravo", "update Charlie", "update Late"}));
}
