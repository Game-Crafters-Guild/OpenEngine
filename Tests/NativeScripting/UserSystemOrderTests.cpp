// The order user systems run in: ascending Order (from a SystemBase's `static constexpr int Order`,
// 0 without one), systems of equal order in registration order, for every hook and across modules.
#include <gtest/gtest.h>

#include "ECS/World.h"
#include "GameSDK/System.h"
#include "NativeScripting/UserSystemRegistry.h"

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
}

TEST_F(UserSystemOrder, RegistryRunsByOrderThenRegistration)
{
    Recorder rules("Rules", 30), flight("Flight", 0), turrets("Turrets", 10), sound("Sound", 0), input("Input", -10);
    for (Recorder* r : {&rules, &flight, &turrets, &sound, &input})
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
