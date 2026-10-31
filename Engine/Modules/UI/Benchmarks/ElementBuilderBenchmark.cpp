#include "UI/ElementBuilder.h"

#include "UI/Controls/Label.h"
#include "UI/Parsers/XMLParser.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

using namespace GameEngine;

namespace
{
constexpr std::uint64_t kPopulationOwner = 0x4c41594f555450;
constexpr int kFactoryValue = 3;
constexpr int kDefaultValue = 23;
constexpr int kRowCounts[] = {8, 64, 256};
constexpr int kPopulations[] = {0, 256};
constexpr std::string_view kProbeTag = "layoutprofileprobe";
bool s_WorkloadFailed = false;

class ProfileElement : public UIElement
{
public:
    void SetValue(int value) { m_Value = value; }
    int GetValue() const { return m_Value; }
private:
    int m_Value = kFactoryValue;
};

class LeadingProfileBase
{
public:
    virtual ~LeadingProfileBase() = default;
    int Value = kFactoryValue;
};

class OffsetProfileElement : public LeadingProfileBase, public ProfileElement {};
class VirtualProfileElement : public virtual UIElement {};

class UnrelatedElement : public UIElement
{
public:
    void SetValue(int value) { m_Value = value; }
private:
    int m_Value = 0;
};

class RegistryPopulation
{
public:
    explicit RegistryPopulation(int count)
    {
        for (int index = 0; index < count; ++index)
        {
            UIRegistration::RegisterOwned<UnrelatedElement>("layoutprofilenoise" + std::to_string(index),
                                                            kPopulationOwner)
                .Attr("profile-noise", &UnrelatedElement::SetValue);
        }
    }

    ~RegistryPopulation()
    {
        UIRegistration::ElementFactoryRegistry::Instance().UnregisterFactoriesOwnedBy(kPopulationOwner);
    }
    RegistryPopulation(const RegistryPopulation&) = delete;
    RegistryPopulation& operator=(const RegistryPopulation&) = delete;
};

void RegisterProfileControls()
{
    UIRegistration::RegisterBuiltInControls();
    UIRegistration::Register<ProfileElement>(kProbeTag)
        .Attr("profile-value", &ProfileElement::SetValue, kDefaultValue)
        .TagAlias("layoutprofilealias");
    UIRegistration::Register<OffsetProfileElement>("layoutprofileoffset");
    UIRegistration::Register<VirtualProfileElement>("layoutprofilevirtual");
}

enum class FactoryCase { Inferred, Explicit, Alias, Offset, Virtual, Erased };

template<FactoryCase Case>
auto CreateProfileElement()
{
    auto& registry = UIRegistration::ElementFactoryRegistry::Instance();
    if constexpr (Case == FactoryCase::Inferred)
        return registry.Create<ProfileElement>();
    else if constexpr (Case == FactoryCase::Explicit)
        return registry.Create<ProfileElement>(kProbeTag);
    else if constexpr (Case == FactoryCase::Alias)
        return registry.Create<ProfileElement>("layoutprofilealias");
    else if constexpr (Case == FactoryCase::Offset)
        return registry.Create<OffsetProfileElement>("layoutprofileoffset");
    else if constexpr (Case == FactoryCase::Virtual)
        return registry.Create<VirtualProfileElement>("layoutprofilevirtual");
    else
        return registry.Create(kProbeTag);
}

template<FactoryCase Case>
bool VerifyFactory()
{
    auto element = CreateProfileElement<Case>();
    if (!element || element->GetTagId() == 0)
        return false;
    if constexpr (requires { element->GetValue(); })
        return element->GetValue() == kFactoryValue;
    else
        return true;
}

template<bool Literal>
auto ProfileName(UI::LiteralName name)
{
    if constexpr (Literal)
        return name;
    else
        return name.GetText();
}

template<bool Literal>
std::unique_ptr<UIElement> BuildNamedElement()
{
    return UI::Element<UIElement>().Id(ProfileName<Literal>(UI::LiteralName{"layout-profile-element"}))
        .Class(ProfileName<Literal>(UI::LiteralName{"layout-profile-root"}),
               ProfileName<Literal>(UI::LiteralName{"layout-profile-value"}),
               ProfileName<Literal>(UI::LiteralName{"layout-profile-root"}),
               ProfileName<Literal>(UI::LiteralName{"selected"})).Build();
}

bool VerifyNamedElement(const UIElement* element)
{
    return element && element->GetId() == "layout-profile-element" &&
        element->GetIdHash() == "layout-profile-element"_sid &&
        element->GetClasses() == std::vector<std::string>{"layout-profile-root", "layout-profile-value", "selected"} &&
        element->GetClassIds() == std::vector<StringId>{"layout-profile-root"_sid, "layout-profile-value"_sid, "selected"_sid};
}

template<bool Literal = false>
std::unique_ptr<UIElement> BuildTree(int rows)
{
    auto root = UI::Element<UIElement>().Class(ProfileName<Literal>(UI::LiteralName{"layout-profile-root"}));
    for (int index = 0; index < rows; ++index)
    {
        root.Children(UI::Element<UIElement>().Class(ProfileName<Literal>(UI::LiteralName{"layout-profile-row"})).Children(
            UI::Element<Label>().Class(ProfileName<Literal>(UI::LiteralName{"layout-profile-caption"})).Set(&Label::SetText, "Field"),
            UI::Element<ProfileElement>().Id("field-" + std::to_string(index))
                .Class(ProfileName<Literal>(UI::LiteralName{"layout-profile-value"}))
                .Set(&ProfileElement::SetValue, index)));
    }
    return std::move(root).Build();
}

std::string MakeXml(int rows)
{
    std::string xml = "<UIElement class='layout-profile-root'>";
    for (int index = 0; index < rows; ++index)
    {
        xml += "<UIElement class='layout-profile-row'><Label class='layout-profile-caption' text='Field'/>"
               "<layoutprofileprobe class='layout-profile-value' id='field-" + std::to_string(index) +
               "' profile-value='" + std::to_string(index) + "'/></UIElement>";
    }
    xml += "</UIElement>";
    return xml;
}

std::unique_ptr<UIElement> ParseTree(const std::string& xml)
{
    std::unique_ptr<UIElement> root;
    if (!UIParsing::XMLParser::ParseLayoutFromString(xml, root))
        return nullptr;
    return root;
}

bool VerifyTree(const UIElement* root, int rows)
{
    if (!root || !root->HasClass("layout-profile-root") || root->GetChildren().size() != static_cast<size_t>(rows))
        return false;
    for (int index = 0; index < rows; ++index)
    {
        const UIElement& row = *root->GetChildren()[index];
        if (!row.HasClass("layout-profile-row") || row.GetChildren().size() != 2)
            return false;
        const auto* label = dynamic_cast<const Label*>(row.GetChildren()[0].get());
        const auto* value = dynamic_cast<const ProfileElement*>(row.GetChildren()[1].get());
        if (!label || label->GetText() != "Field" || !value || value->GetValue() != index ||
            value->GetId() != "field-" + std::to_string(index) || !value->HasClass("layout-profile-value"))
            return false;
    }
    return true;
}

bool VerifyWorkloads()
{
    for (int population : kPopulations)
    {
        RegistryPopulation registrations(population);
        auto defaults = UI::Element<ProfileElement>().Build();
        if (!defaults || defaults->GetValue() != kDefaultValue ||
            !VerifyFactory<FactoryCase::Inferred>() || !VerifyFactory<FactoryCase::Explicit>() ||
            !VerifyFactory<FactoryCase::Alias>() || !VerifyFactory<FactoryCase::Offset>() ||
            !VerifyFactory<FactoryCase::Virtual>() || !VerifyFactory<FactoryCase::Erased>() ||
            !VerifyNamedElement(BuildNamedElement<false>().get()) || !VerifyNamedElement(BuildNamedElement<true>().get()))
            return false;
        for (int rows : kRowCounts)
        {
            if (!VerifyTree(BuildTree(rows).get(), rows) || !VerifyTree(BuildTree<true>(rows).get(), rows) ||
                !VerifyTree(ParseTree(MakeXml(rows)).get(), rows))
                return false;
        }
    }
    return true;
}

template<FactoryCase Case>
void ProfileFactory(benchmark::State& state)
{
    RegistryPopulation registrations(static_cast<int>(state.range(0)));
    if (!VerifyFactory<Case>())
    {
        s_WorkloadFailed = true;
        state.SkipWithError("Factory workload did not create the expected control");
        return;
    }
    for (auto iteration : state)
    {
        (void)iteration;
        auto element = CreateProfileElement<Case>();
        if (!element)
        {
            s_WorkloadFailed = true;
            state.SkipWithError("Factory returned null during timing");
            break;
        }
        benchmark::DoNotOptimize(element.get());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["extra_registrations"] = static_cast<double>(state.range(0));
}

template<bool Literal>
void ProfileNames(benchmark::State& state)
{
    RegistryPopulation registrations(static_cast<int>(state.range(0)));
    if (!VerifyNamedElement(BuildNamedElement<Literal>().get()))
    {
        s_WorkloadFailed = true;
        state.SkipWithError("Name workload did not preserve text, hashes, and class order");
        return;
    }
    for (auto iteration : state)
    {
        (void)iteration;
        auto element = BuildNamedElement<Literal>();
        if (!element)
        {
            s_WorkloadFailed = true;
            state.SkipWithError("Name construction failed during timing");
            break;
        }
        benchmark::DoNotOptimize(element.get());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["extra_registrations"] = static_cast<double>(state.range(0));
}

template<bool Xml, bool Literal = false>
void ProfileTree(benchmark::State& state)
{
    const int rows = static_cast<int>(state.range(0));
    RegistryPopulation registrations(static_cast<int>(state.range(1)));
    const std::string xml = MakeXml(rows);
    auto verification = Xml ? ParseTree(xml) : BuildTree<Literal>(rows);
    if (!VerifyTree(verification.get(), rows))
    {
        s_WorkloadFailed = true;
        state.SkipWithError("Tree workload did not create the expected controls and values");
        return;
    }
    verification.reset();
    for (auto iteration : state)
    {
        (void)iteration;
        auto root = Xml ? ParseTree(xml) : BuildTree<Literal>(rows);
        if (!root)
        {
            s_WorkloadFailed = true;
            state.SkipWithError("Tree construction failed during timing");
            break;
        }
        benchmark::DoNotOptimize(root.get());
        benchmark::ClobberMemory();
    }
    const std::int64_t nodes = 1 + 3 * rows;
    state.SetItemsProcessed(state.iterations() * nodes);
    state.counters["nodes_per_tree"] = static_cast<double>(nodes);
    state.counters["extra_registrations"] = static_cast<double>(state.range(1));
}

template<FactoryCase Case>
void RegisterFactoryCase(const char* name)
{
    auto* test = benchmark::RegisterBenchmark(name, &ProfileFactory<Case>);
    test->ArgNames({"extra"});
    for (int population : kPopulations)
        test->Arg(population);
}

void RegisterBenchmarks()
{
    RegisterFactoryCase<FactoryCase::Inferred>("Factory/Inferred");
    RegisterFactoryCase<FactoryCase::Explicit>("Factory/Explicit");
    RegisterFactoryCase<FactoryCase::Alias>("Factory/Alias");
    RegisterFactoryCase<FactoryCase::Offset>("Factory/Offset");
    RegisterFactoryCase<FactoryCase::Virtual>("Factory/Virtual");
    RegisterFactoryCase<FactoryCase::Erased>("Factory/ErasedControl");
    auto* runtimeNames = benchmark::RegisterBenchmark("Names/Runtime", &ProfileNames<false>);
    auto* literalNames = benchmark::RegisterBenchmark("Names/Literal", &ProfileNames<true>);
    runtimeNames->ArgNames({"extra"});
    literalNames->ArgNames({"extra"});
    for (int population : kPopulations)
    {
        runtimeNames->Arg(population);
        literalNames->Arg(population);
    }
    auto* dsl = benchmark::RegisterBenchmark("Tree/DSL", &ProfileTree<false>);
    auto* literalDsl = benchmark::RegisterBenchmark("Tree/DSLLiteral", &ProfileTree<false, true>);
    auto* xml = benchmark::RegisterBenchmark("Tree/XMLControl", &ProfileTree<true>);
    dsl->ArgNames({"rows", "extra"});
    literalDsl->ArgNames({"rows", "extra"});
    xml->ArgNames({"rows", "extra"});
    for (int rows : kRowCounts)
        for (int population : kPopulations)
        {
            dsl->Args({rows, population});
            literalDsl->Args({rows, population});
            xml->Args({rows, population});
        }
}
}

int main(int argc, char** argv)
{
    RegisterProfileControls();
    RegisterBenchmarks();
    if (argc == 2 && std::string_view(argv[1]) == "--verify")
    {
        const bool valid = VerifyWorkloads();
        std::cout << (valid ? "All layout workloads verified\n" : "Layout workload verification failed\n");
        return valid ? 0 : 1;
    }
#ifndef GE_LAYOUT_PROFILE_RELEASE
    std::cerr << "Timing requires a Release build. Use --verify for untimed workload checks.\n";
    return 1;
#else
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
    {
        benchmark::Shutdown();
        return 1;
    }
    benchmark::AddCustomContext("build_config", GE_LAYOUT_PROFILE_CONFIG);
    benchmark::AddCustomContext("scope", "control construction and destruction; no layout solve or rendering");
    const std::size_t matched = benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return matched != 0 && !s_WorkloadFailed ? 0 : 1;
#endif
}
