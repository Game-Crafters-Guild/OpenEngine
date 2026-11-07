#pragma once

#include "Editor/Entities/EditorECSHelpers.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "UI/UIEvents.h"
#include "UndoRedo/MultiEntityComponentSnapshot.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine::InspectorUI
{
/// Owns a picker's edit across settlement and Inspector rebuilds.
class InspectorColorPickerEdit
{
public:
    struct Binding
    {
        Editor::UndoRedoService* Undo = nullptr;
        std::string Label;
        std::function<Editor::HistorySnapshotTarget()> MakeTarget;
        std::function<void(std::uint32_t, float, Editor::EditChangeKind)> Write;
        std::function<void(Editor::EditChangeKind)> Changed;
        std::function<void()> Applied;
    };

    /// Initializes the session owned by the picker callbacks.
    explicit InspectorColorPickerEdit(Binding binding)
        : m_Binding(std::move(binding))
    {
        EnsureEdit();
    }

    /// Binds Preview, Apply, and Cancel to one edit session.
    static ColorPickerCallbacks Bind(Binding binding)
    {
        auto session = std::make_shared<InspectorColorPickerEdit>(std::move(binding));
        ColorPickerCallbacks callbacks;
        callbacks.onValueChanging = [session](std::uint32_t argb, float intensity) { session->Preview(argb, intensity); };
        callbacks.onApply = [session](std::uint32_t argb, float intensity) { session->Apply(argb, intensity); };
        callbacks.onCancel = [session] { session->Cancel(); };
        return callbacks;
    }

    /// Binds a component writer and row refresh to the shared picker lifecycle.
    template<typename TComponent>
    static Binding ForComponent(InspectorContext context, std::string label, std::string snapshotLabel,
                                void (*writeColor)(TComponent&, std::uint32_t, float),
                                std::function<void()> refresh = {})
    {
        Binding binding;
        binding.Undo = context.Undo;
        binding.Label = std::move(label);
        binding.MakeTarget = [context, snapshotLabel = std::move(snapshotLabel)]
        {
            return Editor::MultiEntityUndo::MakeComponentSnapshotTarget(context.World, {context.Entity},
                ECS::GetComponentTypeId<TComponent>(), context.ChangeNotifications, snapshotLabel);
        };
        binding.Write = [context, writeColor](std::uint32_t argb, float intensity, Editor::EditChangeKind kind)
        {
            WriteComponent(context, writeColor, argb, intensity, kind);
        };
        if (refresh)
        {
            binding.Changed = [refresh](Editor::EditChangeKind kind)
            {
                if (kind != Editor::EditChangeKind::Commit)
                    refresh();
            };
            binding.Applied = std::move(refresh);
        }
        return binding;
    }

    /// Opens a component's picker when its swatch or value receives a left press.
    template<typename TComponent>
    static std::function<void(UIEvent&)> MakeComponentPressHandler(
        InspectorContext context, std::string label, std::string snapshotLabel,
        ColorPickerState (*readColor)(const TComponent&),
        void (*writeColor)(TComponent&, std::uint32_t, float), std::function<void()> refresh)
    {
        auto binding = ForComponent(context, std::move(label), std::move(snapshotLabel), writeColor, std::move(refresh));
        return [context = std::move(context), binding = std::move(binding), readColor](UIEvent& event)
        {
            HandleComponentPress(context, event, binding, readColor);
        };
    }

private:
    template<typename TComponent>
    static void HandleComponentPress(const InspectorContext& context, UIEvent& event, const Binding& binding,
                                      ColorPickerState (*readColor)(const TComponent&))
    {
        if (event.Button != 0)
            return;
        event.Stop();
        if (!context.OpenColorPickerWindow)
            return;
        const auto* component = context.World->GetComponent<TComponent>(context.Entity);
        if (!component)
            return;
        const ColorPickerState state = readColor(*component);
        context.OpenColorPickerWindow(state.Argb, state.Intensity, Bind(binding));
    }

    template<typename TComponent>
    static void WriteComponent(const InspectorContext& context,
                               void (*writeColor)(TComponent&, std::uint32_t, float),
                               std::uint32_t argb, float intensity, Editor::EditChangeKind kind)
    {
        if (context.Undo)
        {
            if (auto* component = context.World->GetComponentForWrite<TComponent>(context.Entity))
                writeColor(*component, argb, intensity);
            return;
        }
        const auto* component = context.World->GetComponent<TComponent>(context.Entity);
        if (!component)
            return;
        TComponent updated = *component;
        writeColor(updated, argb, intensity);
        if (kind == Editor::EditChangeKind::Commit)
            Editor::CommitComponentUpdate(context.World, context.Entity, context.ChangeNotifications, updated);
        else
            Editor::PreviewComponentUpdate(context.World, context.Entity, context.ChangeNotifications, updated);
    }

    bool EnsureEdit()
    {
        if (m_Binding.Undo && !m_Edit)
            m_Edit = m_Binding.Undo->BeginInteractiveEdit(m_Binding.Label, m_Binding.MakeTarget());
        return !m_Binding.Undo || static_cast<bool>(m_Edit);
    }

    bool WriteValue(std::uint32_t argb, float intensity, Editor::EditChangeKind kind)
    {
        if (!EnsureEdit())
            return false;
        if (m_Edit)
            m_Edit.Preview([this, argb, intensity, kind] { m_Binding.Write(argb, intensity, kind); });
        else
            m_Binding.Write(argb, intensity, kind);
        return true;
    }

    void Preview(std::uint32_t argb, float intensity)
    {
        if (WriteValue(argb, intensity, Editor::EditChangeKind::Preview))
            NotifyChanged(Editor::EditChangeKind::Preview);
    }

    void Apply(std::uint32_t argb, float intensity)
    {
        if (!WriteValue(argb, intensity, Editor::EditChangeKind::Commit))
            return;
        NotifyChanged(Editor::EditChangeKind::Commit);
        if (m_Edit)
            m_Edit.Commit();
        if (m_Binding.Applied)
            m_Binding.Applied();
    }

    void Cancel()
    {
        if (m_Edit)
            m_Edit.Cancel();
        NotifyChanged(Editor::EditChangeKind::UndoRedo);
    }

    void NotifyChanged(Editor::EditChangeKind kind)
    {
        if (m_Binding.Changed)
            m_Binding.Changed(kind);
    }

    Binding m_Binding;
    Editor::UndoRedoService::InteractiveEdit m_Edit;
};
} // namespace GameEngine::InspectorUI
