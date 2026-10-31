#include "UI/ElementBuilder.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/UIEvents.h"

#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

using namespace GameEngine;
using GameEngine::UI::operator""_ui;

namespace
{
template<class T>
concept CanDeclare = requires { UI::Element<T>(); };

template<class T, class Setter, class... Arguments>
concept CanSet = requires(UI::ElementBuilder<T>& builder, Setter setter, Arguments... arguments)
{
    builder.Set(setter, arguments...);
};

template<class Child>
concept CanAppend = requires(UI::ElementBuilder<UIElement>& builder, Child&& child)
{
    builder.Children(std::forward<Child>(child));
};

static_assert(!CanDeclare<int>);
static_assert(!std::is_copy_constructible_v<UI::ElementBuilder<Label>>);
static_assert(std::is_nothrow_move_constructible_v<UI::ElementBuilder<Label>>);
static_assert(CanSet<Label, decltype(&UIElement::SetEnabled), bool>);
static_assert(!CanSet<Label, decltype(&Button::SetText), std::string>);
static_assert(!CanSet<Label, decltype(&Label::SetText), int>);
static_assert(!CanSet<Label, decltype(&Label::GetText)>);
static_assert(CanSet<Label, decltype(&Label::SetText), std::string, decltype(&UIElement::SetEnabled), bool>);
static_assert(!CanSet<Label, decltype(&Label::SetText), std::string, decltype(&UIElement::SetEnabled)>);
static_assert(!CanSet<Label, decltype(&Label::SetText), std::string, decltype(&Button::SetText), std::string>);
static_assert(!CanSet<Label, decltype(&Label::SetText), std::string, decltype(&UIElement::SetEnabled), std::string>);
static_assert(!CanAppend<UI::ElementBuilder<Label>&>);
static_assert(!CanAppend<std::unique_ptr<Label>&>);
static_assert(CanAppend<UI::ElementBuilder<Label>>);
static_assert(CanAppend<std::unique_ptr<Label>>);
static_assert(UI::LiteralName{"MixedCase"}.GetHash() == "MixedCase"_sid);
static_assert(UI::LiteralName{""}.GetHash() == HashStringId(""));
static_assert(UI::LiteralName{"\xC3\xA9"}.GetHash() == HashStringId("\xC3\xA9"));
static_assert(UI::LiteralName{std::string_view{"a\0b", 3}}.GetText().size() == 3);
static_assert(std::is_same_v<decltype("name"_ui), UI::LiteralName>);
static_assert("MixedCase"_ui.GetText() == "MixedCase");
static_assert("MixedCase"_ui.GetHash() == "MixedCase"_sid);
static_assert(""_ui.GetText().empty());
static_assert(""_ui.GetHash() == HashStringId(""));
static_assert("\xC3\xA9"_ui.GetHash() == HashStringId("\xC3\xA9"));
static_assert("a\0b"_ui.GetText() == std::string_view{"a\0b", 3});
static_assert("a\0b"_ui.GetHash() == HashStringId(std::string_view{"a\0b", 3}));

class BuilderProbe : public UIElement
{
public:
    explicit BuilderProbe(int count) : m_Count(count) {}
    void SetCount(int count) { m_Count = count; }
    void SetCountWithOffset(int count, int offset) { m_Count = count + offset; }
    int GetCount() const { return m_Count; }
    int GetAppendCount() const { return m_AppendCount; }
    void AddChild(std::unique_ptr<UIElement> child) override
    {
        ++m_AppendCount;
        UIElement::AddChild(std::move(child));
    }
private:
    int m_Count;
    int m_AppendCount = 0;
};

class UnregisteredBuilderElement : public UIElement {};

class LeadingBuilderBase
{
public:
    virtual ~LeadingBuilderBase() = default;
    int Value = 17;
};

class OffsetBuilderElement : public LeadingBuilderBase, public UIElement {};
class VirtualBuilderElement : public virtual UIElement {};

constexpr std::uint64_t kBuilderTestOwner = 0x4255494c444552;

class ElementBuilderTests : public testing::Test
{
protected:
    void SetUp() override
    {
        UIRegistration::RegisterBuiltInControls();
        UIRegistration::Registrar<BuilderProbe>("builderprobe",
            [] { return std::make_unique<BuilderProbe>(17); }, kBuilderTestOwner)
            .Attr("count", &BuilderProbe::SetCount, 23)
            .TagAlias("builderalias");
    }

    void TearDown() override
    {
        UIRegistration::ElementFactoryRegistry::Instance().UnregisterFactoriesOwnedBy(kBuilderTestOwner);
    }
};
}

TEST_F(ElementBuilderTests, CreatesTypedControlsAndInvokesInheritedSetters)
{
    auto label = UI::Element<Label>().Id("caption").Class("title").Class("title")
        .Set(&Label::SetText, "Hello").Set(&UIElement::SetEnabled, false).Build();
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->GetText(), "Hello");
    EXPECT_EQ(label->GetId(), "caption");
    EXPECT_TRUE(label->HasClass("title"));
    EXPECT_EQ(label->GetClasses().size(), 1u);
    EXPECT_FALSE(label->IsEnabled());
    EXPECT_EQ(label->GetTagId(), UIRegistration::ElementFactoryRegistry::Instance().GetTagId("label"));
}

TEST_F(ElementBuilderTests, UsesCustomFactoryAndAppliesDefaultsBeforeExplicitValues)
{
    auto defaults = UI::Element<BuilderProbe>().Build();
    auto explicitValue = UI::Element<BuilderProbe>().Set(&BuilderProbe::SetCount, 31).Build();
    ASSERT_NE(defaults, nullptr);
    ASSERT_NE(explicitValue, nullptr);
    EXPECT_EQ(defaults->GetCount(), 23);
    EXPECT_EQ(explicitValue->GetCount(), 31);
}

TEST_F(ElementBuilderTests, LiteralNamesMatchXmlIdentityAndSelectorsInMixedClassLists)
{
    std::string dynamicClass = "runtime-class-with-owned-storage";
    auto builder = UI::Element<Label>();
    EXPECT_EQ(&builder.Id("MixedCase"_ui)
        .Class("title"_ui, dynamicClass, "title"_ui, "Title"), &builder);
    dynamicClass.clear();
    auto label = std::move(builder).Class("last"_ui).Build();
    ASSERT_NE(label, nullptr);
    std::unique_ptr<UIElement> xml;
    ASSERT_TRUE(UIParsing::XMLParser::ParseLayoutFromString(
        "<Label id='MixedCase' class='title runtime-class-with-owned-storage Title last'/>", xml));
    EXPECT_EQ(label->GetId(), xml->GetId());
    EXPECT_EQ(label->GetIdHash(), xml->GetIdHash());
    EXPECT_EQ(label->GetClasses(), xml->GetClasses());
    EXPECT_EQ(label->GetClassIds(), xml->GetClassIds());
    ASSERT_EQ(label->GetClassIds().size(), 4u);
    EXPECT_NE(label->GetClassIds()[0], label->GetClassIds()[2]);

    Stylesheet sheet;
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(
        ".title { height: 19px; } #MixedCase { width: 37px; }", sheet));
    const auto style = UIParsing::CSSParser::ComputeStyleFor(*label, sheet, UIParsing::ElementState{});
    EXPECT_FLOAT_EQ(style.Layout.Height.Value, 19.0f);
    EXPECT_FLOAT_EQ(style.Layout.Width.Value, 37.0f);
}

TEST_F(ElementBuilderTests, LiteralNamesPreserveEmptyAndEmbeddedNullIdentity)
{
    constexpr auto kEmbedded = "a\0b"_ui;
    auto element = UI::Element<UIElement>().Id(kEmbedded).Class(kEmbedded, ""_ui).Build();
    ASSERT_NE(element, nullptr);
    EXPECT_EQ(element->GetId(), std::string("a\0b", 3));
    EXPECT_EQ(element->GetIdHash(), HashStringId(std::string_view{"a\0b", 3}));
    ASSERT_EQ(element->GetClassIds().size(), 2u);
    EXPECT_EQ(element->GetClasses()[0], std::string("a\0b", 3));
    EXPECT_EQ(element->GetClassIds()[0], kEmbedded.GetHash());
    EXPECT_EQ(element->GetClassIds()[1], HashStringId(""));
    element->SetId(""_ui);
    EXPECT_TRUE(element->GetId().empty());
    EXPECT_EQ(element->GetIdHash(), 0u);
    EXPECT_EQ(UI::Element<UnregisteredBuilderElement>().Id(kEmbedded).Class(kEmbedded).Build(), nullptr);
}

TEST_F(ElementBuilderTests, LiteralNameMutationsPreserveRuleCacheInvalidation)
{
    auto root = UI::Element<UIElement>().Id(UI::LiteralName{"before"})
        .Class(UI::LiteralName{"present"}).Children(UI::Element<UIElement>()).Build();
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->GetChildren().size(), 1u);
    UIElement& child = *root->GetChildren()[0];
    root->m_RuleCacheValid = true;
    child.m_RuleCacheValid = true;
    root->SetId(UI::LiteralName{"before"});
    root->AddClass(UI::LiteralName{"present"});
    EXPECT_TRUE(root->m_RuleCacheValid);
    EXPECT_TRUE(child.m_RuleCacheValid);
    root->SetId(UI::LiteralName{"after"});
    EXPECT_FALSE(root->m_RuleCacheValid);
    EXPECT_FALSE(child.m_RuleCacheValid);
    root->m_RuleCacheValid = true;
    child.m_RuleCacheValid = true;
    root->AddClass(UI::LiteralName{"added"});
    EXPECT_FALSE(root->m_RuleCacheValid);
    EXPECT_FALSE(child.m_RuleCacheValid);
    root->RemoveClass("added");
    EXPECT_FALSE(root->HasClass("added"));
}

TEST_F(ElementBuilderTests, TypedCreationAdjustsANonzeroBaseOffset)
{
    UIRegistration::Registrar<OffsetBuilderElement>("builderoffset", kBuilderTestOwner);
    auto element = UI::Element<OffsetBuilderElement>().Id("offset").Set(&UIElement::SetEnabled, false).Build();
    ASSERT_NE(element, nullptr);
    EXPECT_NE(static_cast<void*>(element.get()), static_cast<void*>(static_cast<UIElement*>(element.get())));
    EXPECT_EQ(element->Value, 17);
    EXPECT_EQ(element->GetId(), "offset");
    EXPECT_FALSE(element->IsEnabled());
}

TEST_F(ElementBuilderTests, TypedCreationSupportsAVirtualBaseAndRejectsMismatchedFactories)
{
    UIRegistration::Registrar<VirtualBuilderElement>("buildervirtual", kBuilderTestOwner);
    auto element = UI::Element<VirtualBuilderElement>().Id("virtual").Set(&UIElement::SetEnabled, false).Build();
    ASSERT_NE(element, nullptr);
    EXPECT_EQ(dynamic_cast<VirtualBuilderElement*>(static_cast<UIElement*>(element.get())), element.get());
    EXPECT_EQ(element->GetId(), "virtual");
    EXPECT_FALSE(element->IsEnabled());

    auto& registry = UIRegistration::ElementFactoryRegistry::Instance();
    ASSERT_TRUE(registry.RegisterFactory("buildervirtualwrong", [] { return std::make_unique<UIElement>(); },
                                        typeid(VirtualBuilderElement), kBuilderTestOwner));
    EXPECT_EQ(registry.Create<VirtualBuilderElement>("buildervirtualwrong"), nullptr);
    EXPECT_EQ(registry.Create<VirtualBuilderElement>("missing-virtual"), nullptr);
}

TEST_F(ElementBuilderTests, AddsMultipleClassesInOrderWithoutDuplicates)
{
    const std::string emphasis = "emphasis";
    const std::string_view selected = "selected";
    auto builder = UI::Element<Label>();
    static_assert(std::is_same_v<decltype(builder.Class("a", "b")), UI::ElementBuilder<Label>&>);
    static_assert(std::is_same_v<decltype(std::move(builder).Class("a", "b")), UI::ElementBuilder<Label>&&>);
    EXPECT_EQ(&builder.Class("title", emphasis, selected, "title"), &builder);

    auto label = std::move(builder).Class("selected", "last").Build();
    ASSERT_NE(label, nullptr);
    const auto& classes = label->GetClasses();
    ASSERT_EQ(classes.size(), 4u);
    EXPECT_EQ(classes[0], "title");
    EXPECT_EQ(classes[1], "emphasis");
    EXPECT_EQ(classes[2], "selected");
    EXPECT_EQ(classes[3], "last");
}

TEST_F(ElementBuilderTests, BatchesSettersInArgumentOrder)
{
    auto builder = UI::Element<Label>();
    static_assert(std::is_same_v<decltype(builder.Set(&Label::SetText, "a", &UIElement::SetEnabled, true)),
                                 UI::ElementBuilder<Label>&>);
    static_assert(std::is_same_v<decltype(std::move(builder).Set(&Label::SetText, "a", &UIElement::SetEnabled, true)),
                                 UI::ElementBuilder<Label>&&>);
    EXPECT_EQ(&builder.Set(&Label::SetText, "initial", &UIElement::SetEnabled, true), &builder);

    auto label = std::move(builder)
        .Set(&Label::SetText, "first", &UIElement::SetEnabled, false, &Label::SetText, std::string("last"))
        .Build();
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->GetText(), "last");
    EXPECT_FALSE(label->IsEnabled());
}

TEST_F(ElementBuilderTests, InvokesMultiArgumentSetters)
{
    auto element = UI::Element<BuilderProbe>().Set(&BuilderProbe::SetCountWithOffset, 10, 7).Build();
    ASSERT_NE(element, nullptr);
    EXPECT_EQ(element->GetCount(), 17);
}

TEST_F(ElementBuilderTests, ConsumesMixedChildrenInOrderThroughVirtualAddChild)
{
    std::unique_ptr<UIElement> xml;
    ASSERT_TRUE(UIParsing::XMLParser::ParseLayoutFromString("<Label id='xml' text='XML'/>", xml));
    auto parent = UI::Element<BuilderProbe>();
    parent.Children(UI::Element<Button>().Id("button").Set(&Button::SetText, "Save"), std::move(xml));
    for (int index = 0; index < 2; ++index)
        parent.Children(UI::Element<Label>().Id("item-" + std::to_string(index)));
    auto root = std::move(parent).Build();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(xml, nullptr);
    EXPECT_EQ(root->GetAppendCount(), 4);
    ASSERT_EQ(root->GetChildren().size(), 4u);
    EXPECT_EQ(root->GetChildren()[0]->GetId(), "button");
    EXPECT_EQ(root->GetChildren()[1]->GetId(), "xml");
    EXPECT_EQ(root->GetChildren()[2]->GetId(), "item-0");
    EXPECT_EQ(root->GetChildren()[3]->GetId(), "item-1");
    auto* button = dynamic_cast<Button*>(root->GetChildren()[0].get());
    ASSERT_NE(button, nullptr);
    ASSERT_EQ(button->GetChildren().size(), 1u);
    auto* caption = dynamic_cast<Label*>(button->GetChildren()[0].get());
    ASSERT_NE(caption, nullptr);
    EXPECT_EQ(caption->GetText(), "Save");
}

TEST_F(ElementBuilderTests, MovesWithoutMovingControlsAndBuildTransfersOwnershipOnce)
{
    UIElement::WeakRef<Label> reference;
    auto original = UI::Element<Label>().Capture(reference);
    Label* identity = reference.Get();
    ASSERT_NE(identity, nullptr);
    auto moved = std::move(original);
    EXPECT_EQ(std::move(original).Build(), nullptr);
    auto label = std::move(moved).Build();
    EXPECT_EQ(label.get(), identity);
    EXPECT_EQ(reference.Get(), identity);
    EXPECT_EQ(std::move(moved).Build(), nullptr);
    label.reset();
    EXPECT_EQ(reference.Get(), nullptr);
}

TEST_F(ElementBuilderTests, FailureConsumesAllChildrenAndExpiresCapturedReferences)
{
    UIElement::WeakRef<UIElement> parentReference;
    UIElement::WeakRef<Label> childReference;
    auto child = UI::Element<Label>().Capture(childReference).Build();
    auto root = UI::Element<UIElement>().Capture(parentReference).Children(
        UI::Element<Button>(), std::unique_ptr<UIElement>{}, std::move(child)).Build();
    EXPECT_EQ(root, nullptr);
    EXPECT_EQ(child, nullptr);
    EXPECT_EQ(parentReference.Get(), nullptr);
    EXPECT_EQ(childReference.Get(), nullptr);
}

TEST_F(ElementBuilderTests, MissingOrIncompatibleRegistrationFailsTheDeclaration)
{
    EXPECT_EQ(UI::Element<UnregisteredBuilderElement>().Build(), nullptr);
    EXPECT_EQ(UI::Element<Label>("missing-builder-tag").Build(), nullptr);
    EXPECT_EQ(UI::Element<Label>("button").Build(), nullptr);
    EXPECT_EQ(UI::Element<UIElement>().Children(UI::Element<UnregisteredBuilderElement>()).Build(), nullptr);

    auto& registry = UIRegistration::ElementFactoryRegistry::Instance();
    ASSERT_TRUE(registry.RegisterFactory("builderwrongfactory",
        [] { return std::make_unique<UIElement>(); }, typeid(BuilderProbe), kBuilderTestOwner));
    EXPECT_EQ(UI::Element<BuilderProbe>("builderwrongfactory").Build(), nullptr);
    ASSERT_TRUE(registry.RegisterFactory("buildernullfactory",
        [] { return std::unique_ptr<UIElement>{}; }, typeid(BuilderProbe), kBuilderTestOwner));
    EXPECT_EQ(UI::Element<BuilderProbe>("buildernullfactory").Build(), nullptr);
}

TEST_F(ElementBuilderTests, AliasesShareIdentityAndMultipleCanonicalTagsRequireAnExplicitTag)
{
    auto canonical = UI::Element<BuilderProbe>().Build();
    auto alias = UI::Element<BuilderProbe>("BuilderAlias").Build();
    ASSERT_NE(canonical, nullptr);
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(canonical->GetTagId(), alias->GetTagId());

    auto& registry = UIRegistration::ElementFactoryRegistry::Instance();
    ASSERT_TRUE(registry.RegisterFactory("buildersecond",
        [] { return std::make_unique<BuilderProbe>(5); }, typeid(BuilderProbe), kBuilderTestOwner));
    EXPECT_EQ(UI::Element<BuilderProbe>().Build(), nullptr);
    auto second = UI::Element<BuilderProbe>("buildersecond").Build();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second->GetTagId(), canonical->GetTagId());
}

TEST_F(ElementBuilderTests, HandlersSurviveTheBuilderAndReceiveTheOriginalEvent)
{
    int receivedModifiers = 0;
    auto button = UI::Element<Button>().On(kEventButtonClick,
        [&receivedModifiers](UIEvent& event) { receivedModifiers = event.Mods; }).Build();
    ASSERT_NE(button, nullptr);
    button->TriggerClick(7);
    EXPECT_EQ(receivedModifiers, 7);
}
