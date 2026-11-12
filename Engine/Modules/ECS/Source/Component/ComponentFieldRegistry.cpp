#include "ECS/Component/ComponentFieldRegistry.h"

#include "ECS/ModuleRegistration.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {
namespace ECS {

namespace {
struct Entry
{
    // Non-owning view of the type's constexpr Reflection<T>::Fields by default (zero per-component
    // heap). The first per-field mutation (SetFieldFlags / SetFieldEnum) copies the table into Owned
    // ONCE (copy-on-write) and re-points Fields at it — the constexpr source is never touched, so a
    // component with no policy flags / enum bindings stays copy-free.
    std::span<const FieldInfo> Fields;
    std::vector<FieldInfo> Owned;  // filled lazily on first mutation; Fields then views it
    std::string_view CanonicalName;
    // RegisterOwned storage: field names + canonical name deep-copied from the caller (the
    // scripting ABI passes marshaled temporaries). OwnedStrings is filled once per registration
    // and never resized afterwards — Owned's Name views point into it, and a resize would move
    // the (SSO) string objects out from under them.
    std::vector<std::string> OwnedStrings;
    std::string OwnedName;
    bool DoNotSerialize = false;   // "// [DoNotSerialize]" runtime/derived component — scene skips it
    bool EditorOnly = false;       // "// @ge-editor-only" — a game export strips it from staged scenes
    ScriptWrite ScriptAccess = ScriptWrite::Allowed; // a struct-level @ge-readonly or @ge-engine-internal
    std::string_view Writer;                         // the system that writes it, for a refused script write
    // The byte runs a script's write takes from the script, kept with Fields (UpdateScriptWritableRanges).
    std::vector<ScriptWritableRange> ScriptWritableRanges;
    // Owning module + load generation (C12). The default Register path keeps
    // NON-OWNING views into the registrant's constexpr tables (module rdata),
    // so entries of an unmapping module MUST be purged with it, and an entry
    // still stamped with an older generation blocks that image's unload.
    ModuleRegistrationStamp Module;
};

// Node-based map: an Entry (and its Owned storage) is address-stable across other inserts, so a span
// returned by Get() stays valid.
std::unordered_map<ComponentTypeId, Entry>& FieldTables()
{
    static std::unordered_map<ComponentTypeId, Entry> tables;
    return tables;
}

// Copy the constexpr field view into Owned the first time a field is mutated, then point Fields at
// Owned. Idempotent: later mutations edit Owned in place (no realloc), so Fields stays valid.
void EnsureOwned(Entry& e)
{
    if (e.Owned.empty() && !e.Fields.empty())
    {
        e.Owned.assign(e.Fields.begin(), e.Fields.end());
        e.Fields = e.Owned;
    }
}

// The field named `fieldName` in the entry's registry-owned copy, made writable first; nullptr when
// the entry has no such field.
FieldInfo* FindOwnedField(Entry& e, std::string_view fieldName)
{
    EnsureOwned(e);
    for (FieldInfo& field : e.Owned)
        if (field.Name == fieldName)
            return &field;
    return nullptr;
}

// Recompute the byte runs of the fields a script may write: every field with no Hidden, ReadOnly or
// Transient flag, in offset order, adjacent fields joined. Runs after every change of Fields or a flag.
void UpdateScriptWritableRanges(Entry& e)
{
    constexpr FieldFlags kEngineKept = FieldFlags::Hidden | FieldFlags::ReadOnly | FieldFlags::Transient;
    std::vector<ScriptWritableRange> ranges;
    for (const FieldInfo& field : e.Fields)
        if (!HasAnyFlag(field.Flags, kEngineKept) && field.Size != 0)
            ranges.push_back(ScriptWritableRange{field.Offset, field.Offset + field.Size});
    std::sort(ranges.begin(), ranges.end(),
              [](const ScriptWritableRange& a, const ScriptWritableRange& b) { return a.Begin < b.Begin; });
    e.ScriptWritableRanges.clear();
    for (const ScriptWritableRange& range : ranges)
    {
        if (!e.ScriptWritableRanges.empty() && range.Begin <= e.ScriptWritableRanges.back().End)
            e.ScriptWritableRanges.back().End = std::max(e.ScriptWritableRanges.back().End, range.End);
        else
            e.ScriptWritableRanges.push_back(range);
    }
}

// FindByName's index: each canonical name a type registered with, in registration order. A record never
// moves, and its Name does not change after the append, so FindByName reads the index on any thread with no
// lock while the main thread registers. The writers lock only each other out.
struct NameRecord
{
    std::string Name;
    std::atomic<ComponentTypeId> Id{0}; // 0 while no registered type has this name
};

struct NameRecordBlock
{
    static constexpr std::size_t kRecords = 128;
    std::array<NameRecord, kRecords> Records;
    std::atomic<NameRecordBlock*> Next{nullptr}; // owned by NameIndex::Blocks
};

struct NameIndex
{
    NameRecordBlock First;
    std::vector<std::unique_ptr<NameRecordBlock>> Blocks; // the blocks after First; only the writers read it
    NameRecordBlock* Last = &First;    // the block that the next append writes
    std::atomic<std::size_t> Count{0}; // the records a reader may read
    std::mutex WriterMutex;
};

NameIndex& Names()
{
    static NameIndex index;
    return index;
}

// The first published record, in append order, for which `match` returns true; null when none does.
template <class Match>
NameRecord* FindRecord(NameIndex& index, Match&& match)
{
    const std::size_t count = index.Count.load(std::memory_order_acquire);
    NameRecordBlock* block = &index.First;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0 && i % NameRecordBlock::kRecords == 0)
            block = block->Next.load(std::memory_order_acquire);
        NameRecord& record = block->Records[i % NameRecordBlock::kRecords];
        if (match(record))
            return &record;
    }
    return nullptr;
}

bool HoldsId(const NameRecord& record, ComponentTypeId typeId)
{
    return record.Id.load(std::memory_order_relaxed) == typeId;
}

// Gives `typeId` the name `name` in the index, and takes its other name from it. An empty name removes
// the type from the index. A type holds at most one record, and a record that no type holds is used
// again before a new record is appended.
void PublishName(ComponentTypeId typeId, std::string_view name)
{
    NameIndex& index = Names();
    const std::lock_guard lock(index.WriterMutex);
    NameRecord* const held = FindRecord(index, [typeId](const NameRecord& record) { return HoldsId(record, typeId); });
    if (held && held->Name == name)
        return;
    if (held)
        held->Id.store(0, std::memory_order_relaxed);
    if (name.empty())
        return;
    NameRecord* const unused =
        FindRecord(index, [name](const NameRecord& record) { return HoldsId(record, 0) && record.Name == name; });
    if (unused)
    {
        unused->Id.store(typeId, std::memory_order_relaxed);
        return;
    }
    const std::size_t count = index.Count.load(std::memory_order_relaxed);
    if (count != 0 && count % NameRecordBlock::kRecords == 0)
    {
        NameRecordBlock* const block = index.Blocks.emplace_back(std::make_unique<NameRecordBlock>()).get();
        index.Last->Next.store(block, std::memory_order_release);
        index.Last = block;
    }
    NameRecord& record = index.Last->Records[count % NameRecordBlock::kRecords];
    record.Name.assign(name);
    record.Id.store(typeId, std::memory_order_relaxed);
    index.Count.store(count + 1, std::memory_order_release);
}
} // namespace

void ComponentFieldRegistry::Register(ComponentTypeId typeId, std::span<const FieldInfo> fields,
                                      std::string_view canonicalName)
{
    Entry& e = FieldTables()[typeId];
    e.Owned.clear();   // re-registration (hot-reload) re-points to the fresh constexpr source;
    e.OwnedStrings.clear();
    if (!e.OwnedName.empty())
        e.CanonicalName = {}; // was viewing OwnedName — don't leave it dangling
    e.OwnedName.clear();
    e.Fields = fields; // any following Set* calls re-apply via copy-on-write
    if (!canonicalName.empty())
        e.CanonicalName = canonicalName;
    e.Module = GetActiveRegistrationModule();
    UpdateScriptWritableRanges(e);
    PublishName(typeId, e.CanonicalName);
}

void ComponentFieldRegistry::RegisterOwned(ComponentTypeId typeId, std::span<const FieldInfo> fields,
                                           std::string_view canonicalName)
{
    Entry& e = FieldTables()[typeId];
    e.Owned.clear();
    e.OwnedStrings.clear();
    e.OwnedName.clear();

    // Copy the name strings first, sized exactly: the FieldInfo Name views built below point into
    // these strings, so the vector must never reallocate after this loop.
    e.OwnedStrings.reserve(fields.size());
    for (const FieldInfo& f : fields)
        e.OwnedStrings.emplace_back(f.Name);

    e.Owned.assign(fields.begin(), fields.end());
    for (std::size_t i = 0; i < e.Owned.size(); ++i)
        e.Owned[i].Name = e.OwnedStrings[i];
    e.Fields = e.Owned;

    if (!canonicalName.empty())
    {
        e.OwnedName.assign(canonicalName);
        e.CanonicalName = e.OwnedName;
    }
    e.Module = GetActiveRegistrationModule();
    UpdateScriptWritableRanges(e);
    PublishName(typeId, e.CanonicalName);
}

void ComponentFieldRegistry::UnregisterForTests(ComponentTypeId typeId)
{
    FieldTables().erase(typeId);
    PublishName(typeId, {});
}

std::size_t ComponentFieldRegistry::PurgeModuleFieldTables(std::string_view moduleId, std::uint64_t generation)
{
    if (moduleId.empty())
        return 0;
    auto& tables = FieldTables();
    std::size_t purged = 0;
    for (auto it = tables.begin(); it != tables.end();)
    {
        if (it->second.Module.Matches(moduleId, generation))
        {
            PublishName(it->first, {});
            it = tables.erase(it);
            ++purged;
        }
        else
        {
            ++it;
        }
    }
    return purged;
}

std::size_t ComponentFieldRegistry::CountSupersededModuleFieldTables(std::string_view moduleId,
                                                                     std::uint64_t currentGeneration)
{
    if (moduleId.empty())
        return 0;
    std::size_t stale = 0;
    for (const auto& [typeId, entry] : FieldTables())
    {
        if (entry.Module.ModuleId == moduleId && entry.Module.Generation < currentGeneration)
            ++stale;
    }
    return stale;
}

std::span<const FieldInfo> ComponentFieldRegistry::Get(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
    {
        return {};
    }
    return it->second.Fields;
}

void ComponentFieldRegistry::SetFieldFlags(ComponentTypeId typeId, std::string_view fieldName,
                                           FieldFlags flags)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    if (FieldInfo* field = FindOwnedField(it->second, fieldName))
    {
        field->Flags = flags;
        UpdateScriptWritableRanges(it->second);
    }
}

ScriptWriteRule ComponentFieldRegistry::GetScriptWriteRule(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return {};
    return ScriptWriteRule{it->second.ScriptAccess, it->second.Writer, it->second.ScriptWritableRanges};
}

void ComponentFieldRegistry::SetFieldRange(ComponentTypeId typeId, std::string_view fieldName,
                                           float minValue, float maxValue)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    if (FieldInfo* field = FindOwnedField(it->second, fieldName))
    {
        field->MinValue = minValue;
        field->MaxValue = maxValue;
        field->HasRange = true;
    }
}

void ComponentFieldRegistry::SetFieldTooltip(ComponentTypeId typeId, std::string_view fieldName,
                                             std::string_view tooltip)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    if (FieldInfo* field = FindOwnedField(it->second, fieldName))
        field->Tooltip = tooltip;
}

void ComponentFieldRegistry::SetFieldLabel(ComponentTypeId typeId, std::string_view fieldName,
                                           std::string_view label)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    if (FieldInfo* field = FindOwnedField(it->second, fieldName))
        field->Label = label;
}

void ComponentFieldRegistry::SetFieldEnum(ComponentTypeId typeId, std::string_view fieldName,
                                          std::span<const EnumNameValue> enumNames)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    if (FieldInfo* field = FindOwnedField(it->second, fieldName))
        field->EnumNames = enumNames;
}

std::string_view ComponentFieldRegistry::GetCanonicalName(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
    {
        return {};
    }
    return it->second.CanonicalName;
}

namespace {
bool IEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Case-insensitive: callers include the scene loader (component names are lowercased by the .scene
// parser) and the editor/MCP (user-typed names). A canonical name also matches by its simple-name suffix
// after "::" (native C++) or "." (a managed blob component's dotted CLR name: "Game.Health" matches "Health").
bool NameMatches(std::string_view canon, std::string_view name)
{
    if (IEquals(canon, name))
        return true;
    if (canon.size() <= name.size() || !IEquals(canon.substr(canon.size() - name.size()), name))
        return false;
    const std::string_view qualifier = canon.substr(0, canon.size() - name.size());
    return qualifier.ends_with("::") || qualifier.ends_with('.');
}
} // namespace

ComponentTypeId ComponentFieldRegistry::FindByName(std::string_view name)
{
    const NameRecord* const record =
        FindRecord(Names(), [name](const NameRecord& r) { return !HoldsId(r, 0) && NameMatches(r.Name, name); });
    return record ? record->Id.load(std::memory_order_relaxed) : 0;
}

const FieldInfo* ComponentFieldRegistry::FindField(ComponentTypeId typeId, std::string_view fieldName)
{
    for (const FieldInfo& field : Get(typeId))
    {
        if (IEquals(field.Name, fieldName))
            return &field;
    }
    return nullptr;
}

bool ComponentFieldRegistry::Has(ComponentTypeId typeId)
{
    return FieldTables().contains(typeId);
}

void ComponentFieldRegistry::SetComponentDoNotSerialize(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    if (it != FieldTables().end())
        it->second.DoNotSerialize = true;
}

bool ComponentFieldRegistry::IsComponentDoNotSerialize(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    return it != FieldTables().end() && it->second.DoNotSerialize;
}

void ComponentFieldRegistry::SetComponentEditorOnly(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    if (it != FieldTables().end())
        it->second.EditorOnly = true;
}

bool ComponentFieldRegistry::IsComponentEditorOnly(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    return it != FieldTables().end() && it->second.EditorOnly;
}

void ComponentFieldRegistry::SetComponentScriptWrite(ComponentTypeId typeId, ScriptWrite access, std::string_view writer)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    it->second.ScriptAccess = access;
    it->second.Writer = writer;
}

} // namespace ECS
} // namespace GameEngine
