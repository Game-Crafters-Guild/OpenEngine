#pragma once

#include "Panels/Scribe/ScribeDocument.h"
#include "UI/AssetBoundDockPanel.h"
#include "UI/UIManagerRef.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
class Button;
class ConfirmActionModal;
class FloatField;
class Label;
class SaveSceneChangesModal;
class ScrollView;
} // namespace GameEngine

namespace GameEngine::Editor
{
class ScribeWorkspace;
class UndoRedoService;

/// The dock id of the Scribe panel.
inline constexpr const char* kScribePanelId = "Scribe";

/// The Scribe panel: an outline of the state-tree program file (.scribe) that the workspace shows.
/// The selected row's fields edit in place, one undo step per edit, and Save writes the file.
/// Each refusal shows as a badge on the row that it names. The notice gives the first refusal with its fix,
/// or only the count of refusals in a narrow panel.
class ScribePanel final : public AssetBoundDockPanel
{
  public:
    /// Registers the panel and the opener of state-tree program files (.scribe): an open of a program file
    /// shows it in the panel. The names of the program are checked against AIECS::AiRegistry::Get().
    static void Register(UndoRedoService* undo);

    explicit ScribePanel(std::shared_ptr<ScribeWorkspace> workspace);
    ~ScribePanel() override;

    /// The program shown; null when no file is open.
    const std::shared_ptr<ScribeDocument>& GetDocument() const;
    /// Builds the panel again when the file, the program or the selection changed. Runs on the UI's
    /// periodic refresh.
    void Refresh();

    void OnOwnerManagerChanged(UIManager* owner) override;
    void OnPostLayout() override;

  private:
    // What a Save does after it writes the file or fails.
    enum class AfterSave
    {
        Stay,
        OpenPending,
    };

    using FloatEdit = std::function<void(ScribeDocument& document, float32 value, ScribeDocument::Phase phase)>;

    struct RowKey
    {
        ScribeOutlineRow::Kind RowKind = ScribeOutlineRow::Kind::State;
        uint16 State = ScribeOutlineRow::kNoState;
        uint16 Item = 0;
    };

    void OnLayoutBound() override;
    void OnLayoutReconciled() override;
    void BindElements();
    void Build();
    void RequestBuild();
    void BuildOutline(UIElement& content);
    void BuildDetails(UIElement& details);
    void AddStateFields(UIElement& details, uint16 state);
    void AddOptionFields(UIElement& details, uint16 state);
    void AddUtilityFields(UIElement& details, uint16 state);
    void AddConsiderationFields(UIElement& details, uint16 state, uint16 consideration);
    void AddTransitionEventField(UIElement& details, uint16 state, uint16 transition);
    // A field that shows each drag step and typed value through `edit` as a preview, and commits at the end.
    // A field that takes no value below `minimum`, also in a drag.
    std::unique_ptr<FloatField> MakeFloatEdit(float value, float minimum, FloatEdit edit);
    // Shows the preview of a field that the user drags: the outline and the header build again, the fields stay.
    void ShowPreview();
    void Select(RowKey key);
    void SaveDocument(AfterSave after);
    void WriteDocument(AfterSave after);
    void ConfirmSaveWithRefusal(AfterSave after);
    void ConfirmPendingOpen();
    void CancelPendingOpen();
    void RevealSelectedRow();
    // The notice: the open and save errors, then the refused rows by cause; `narrow` gives their count alone.
    std::string NoticeText(bool narrow) const;
    void ShowNotice();
    bool IsSelected(const ScribeOutlineRow& row) const;

    std::shared_ptr<ScribeWorkspace> m_Workspace;
    std::vector<ScribeOutlineRow> m_Rows;
    RowKey m_Selected;
    std::weak_ptr<const ScribeDocument> m_ShownDocument;
    uint64 m_ShownVersion = ~uint64{0};
    uint64 m_ShownNamesVersion = ~uint64{0};
    // Why the last Save failed, with the fix; empty after a Save that wrote the file.
    std::string m_SaveError;
    bool m_NeedsBuild = true;
    // Set by a build that shows a selected row: the next layout scrolls the row into view.
    bool m_RevealSelectedRow = false;
    // Set by a layout narrower than the notice breakpoint: the notice gives the count of refusals alone.
    bool m_NarrowNotice = false;

    WeakRef<Label> m_FileName;
    WeakRef<Label> m_DirtyMark;
    WeakRef<Button> m_SaveButton;
    WeakRef<Label> m_Empty;
    WeakRef<Label> m_Notice;
    WeakRef<ScrollView> m_OutlineScroll;
    WeakRef<UIElement> m_OutlineContent;
    WeakRef<UIElement> m_SelectedRow;
    WeakRef<UIElement> m_Details;
    WeakRef<ConfirmActionModal> m_SaveConfirm;
    WeakRef<SaveSceneChangesModal> m_OpenConfirm;

    UIManagerRef m_RefreshManager;
    uint64_t m_RefreshToken = 0;
};

} // namespace GameEngine::Editor
