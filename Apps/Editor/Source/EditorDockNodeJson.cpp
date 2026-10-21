#include "EditorDockNodeJson.h"

#include <string>
#include <utility>

namespace GameEngine
{

namespace
{
static const char* DockPositionToString(DockPosition p)
{
    switch (p)
    {
    case DockPosition::Left:
        return "Left";
    case DockPosition::Right:
        return "Right";
    case DockPosition::Top:
        return "Top";
    case DockPosition::Bottom:
        return "Bottom";
    case DockPosition::Center:
        return "Center";
    default:
        return "Left";
    }
}

static bool TryParseDockPosition(const nlohmann::json& j, DockPosition& out)
{
    if (!j.is_string())
        return false;
    const std::string s = j.get<std::string>();
    if (s == "Left" || s == "left")
    {
        out = DockPosition::Left;
        return true;
    }
    if (s == "Right" || s == "right")
    {
        out = DockPosition::Right;
        return true;
    }
    if (s == "Top" || s == "top")
    {
        out = DockPosition::Top;
        return true;
    }
    if (s == "Bottom" || s == "bottom")
    {
        out = DockPosition::Bottom;
        return true;
    }
    if (s == "Center" || s == "center")
    {
        out = DockPosition::Center;
        return true;
    }
    return false;
}

} // namespace

nlohmann::json DockNodeToJson(const DockNode* n)
{
    if (!n)
        return nlohmann::json();

    if (n->IsLeaf())
    {
        nlohmann::json j;
        j["kind"] = "leaf";
        j["tabs"] = nlohmann::json::array();
        for (const auto& t : n->GetTabs())
            j["tabs"].push_back(t);
        const std::string active = n->GetActivePanelId();
        if (!active.empty())
            j["active"] = active;
        return j;
    }

    // Split
    nlohmann::json j;
    j["kind"] = "split";
    j["dir"] = DockPositionToString(n->GetSplitDirection());
    j["ratio"] = n->GetSplitRatio();
    j["minFirstPx"] = n->GetMinFirstPx();
    j["minSecondPx"] = n->GetMinSecondPx();
    j["first"] = DockNodeToJson(n->First());
    j["second"] = DockNodeToJson(n->Second());
    return j;
}

std::unique_ptr<DockNode> DockNodeFromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        return nullptr;

    const auto itKind = j.find("kind");
    if (itKind == j.end() || !itKind->is_string())
        return nullptr;
    const std::string kind = itKind->get<std::string>();

    if (kind == "leaf")
    {
        auto leaf = DockNode::MakeLeaf();
        const auto itTabs = j.find("tabs");
        if (itTabs != j.end() && itTabs->is_array())
        {
            for (const auto& t : *itTabs)
            {
                if (t.is_string())
                    leaf->AddTab(t.get<std::string>());
            }
        }
        const auto itActive = j.find("active");
        if (itActive != j.end() && itActive->is_string())
        {
            (void)leaf->ActivateTab(itActive->get<std::string>());
        }
        return leaf;
    }

    if (kind == "split")
    {
        DockPosition dir = DockPosition::Left;
        const auto itDir = j.find("dir");
        (void)TryParseDockPosition(itDir != j.end() ? *itDir : nlohmann::json(), dir);

        float ratio = 0.5f;
        const auto itRatio = j.find("ratio");
        if (itRatio != j.end() && itRatio->is_number())
            ratio = itRatio->get<float>();

        auto first = DockNodeFromJson(j.value("first", nlohmann::json()));
        auto second = DockNodeFromJson(j.value("second", nlohmann::json()));
        if (!first || !second)
            return nullptr;

        auto split = std::make_unique<DockNode>();
        split->SetSplit(dir, ratio, std::move(first), std::move(second));

        float minFirst = 100.0f;
        float minSecond = 100.0f;
        const auto itMinFirst = j.find("minFirstPx");
        const auto itMinSecond = j.find("minSecondPx");
        if (itMinFirst != j.end() && itMinFirst->is_number())
            minFirst = itMinFirst->get<float>();
        if (itMinSecond != j.end() && itMinSecond->is_number())
            minSecond = itMinSecond->get<float>();
        split->SetMinChildSizes(minFirst, minSecond);

        return split;
    }

    return nullptr;
}

std::unique_ptr<DockNode> CloneDockNode(const DockNode* src)
{
    if (!src)
        return nullptr;

    if (src->IsLeaf())
    {
        auto out = DockNode::MakeLeaf();
        for (const auto& tab : src->GetTabs())
        {
            out->AddTab(tab);
        }
        const std::string& active = src->GetActivePanelId();
        if (!active.empty())
        {
            (void)out->ActivateTab(active);
        }
        return out;
    }

    if (src->IsSplit())
    {
        auto out = std::make_unique<DockNode>();
        auto first = CloneDockNode(src->First());
        auto second = CloneDockNode(src->Second());
        out->SetSplit(src->GetSplitDirection(), src->GetSplitRatio(), std::move(first), std::move(second));
        out->SetMinChildSizes(src->GetMinFirstPx(), src->GetMinSecondPx());
        return out;
    }

    return nullptr;
}

} // namespace GameEngine
