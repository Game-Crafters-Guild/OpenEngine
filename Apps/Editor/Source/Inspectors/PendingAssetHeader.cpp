#include "Inspectors/PendingAssetHeader.h"

#include "UI/Controls/Label.h"

namespace GameEngine::Editor
{

PendingAssetHeaderLabels MakePendingAssetHeaderLabels(const std::string& name)
{
    PendingAssetHeaderLabels labels;
    labels.Name = std::make_unique<Label>();
    labels.Name->AddClass("inspector-header");
    labels.Name->AddClass("inspector-header-fit");
    labels.Name->SetText(name);
    labels.Suffix = std::make_unique<Label>();
    labels.Suffix->AddClass("inspector-header-suffix");
    labels.Suffix->AddClass("hidden");
    labels.Suffix->SetId(kPendingAssetSuffixLabelId);
    return labels;
}

} // namespace GameEngine::Editor
