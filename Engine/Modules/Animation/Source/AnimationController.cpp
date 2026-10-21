#include "Animation/AnimationController.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Serialization/JsonRead.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <nlohmann/json.hpp>
#include <utility>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

AssetType MotionAssetTypeFromString(const std::string& value)
{
    if (value == "Timeline") return AssetType::Timeline;
    if (value == "ClipSet") return AssetType::ClipSet;
    if (value == "AnimationLibrary") return AssetType::AnimationLibrary;
    if (value == "AnimationController") return AssetType::AnimationController;
    if (value == "SpriteFrames") return AssetType::SpriteFrames;
    return AssetType::Animation;
}

// A motion object; a field of the wrong type is refused into `reader`, named after `context`.
AnimationStateMotion LoadMotion(const json& doc, Serialization::JsonFieldReader& reader, std::string context)
{
    reader.SetContext(std::move(context));
    AnimationStateMotion motion;
    if (doc.contains("assetGuid") && doc["assetGuid"].is_string())
        motion.AssetGuid = GUID(doc["assetGuid"].get<std::string>());
    std::string type = "Animation";
    reader.Read(doc, "type", type);
    motion.Type = MotionAssetTypeFromString(type);
    motion.LibraryName.clear();
    reader.Read(doc, "libraryName", motion.LibraryName);
    motion.Speed = 1.0f;
    reader.Read(doc, "speed", motion.Speed);
    motion.Loop = true;
    reader.Read(doc, "loop", motion.Loop);
    return motion;
}

json SaveMotion(const AnimationStateMotion& motion)
{
    json doc;
    doc["assetGuid"] = motion.AssetGuid.ToString();
    doc["type"] = AssetTypeToString(motion.Type);
    doc["libraryName"] = motion.LibraryName;
    doc["speed"] = motion.Speed;
    doc["loop"] = motion.Loop;
    return doc;
}

} // namespace

const char* AnimationParameterTypeToString(AnimationParameterType type)
{
    switch (type)
    {
        case AnimationParameterType::Float: return "Float";
        case AnimationParameterType::Int: return "Int";
        case AnimationParameterType::Bool: return "Bool";
        case AnimationParameterType::Trigger: return "Trigger";
    }
    return "Float";
}

AnimationParameterType AnimationParameterTypeFromString(const std::string& value)
{
    if (value == "Int") return AnimationParameterType::Int;
    if (value == "Bool") return AnimationParameterType::Bool;
    if (value == "Trigger") return AnimationParameterType::Trigger;
    return AnimationParameterType::Float;
}

const char* AnimationTransitionComparisonToString(AnimationTransitionComparison comparison)
{
    switch (comparison)
    {
        case AnimationTransitionComparison::Always: return "Always";
        case AnimationTransitionComparison::Equals: return "Equals";
        case AnimationTransitionComparison::NotEquals: return "NotEquals";
        case AnimationTransitionComparison::Greater: return "Greater";
        case AnimationTransitionComparison::Less: return "Less";
    }
    return "Always";
}

AnimationTransitionComparison AnimationTransitionComparisonFromString(const std::string& value)
{
    if (value == "Equals") return AnimationTransitionComparison::Equals;
    if (value == "NotEquals") return AnimationTransitionComparison::NotEquals;
    if (value == "Greater") return AnimationTransitionComparison::Greater;
    if (value == "Less") return AnimationTransitionComparison::Less;
    return AnimationTransitionComparison::Always;
}

const char* BlendTreeTypeToString(BlendTreeType type)
{
    switch (type)
    {
        case BlendTreeType::None: return "None";
        case BlendTreeType::Blend1D: return "Blend1D";
        case BlendTreeType::Blend2D: return "Blend2D";
    }
    return "None";
}

BlendTreeType BlendTreeTypeFromString(const std::string& value)
{
    if (value == "Blend1D") return BlendTreeType::Blend1D;
    if (value == "Blend2D") return BlendTreeType::Blend2D;
    return BlendTreeType::None;
}

bool AnimationController::Load()
{
    SetState(AssetState::Loading);
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("AnimationController: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool AnimationController::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void AnimationController::Unload()
{
    m_Parameters.clear();
    m_States.clear();
    m_Transitions.clear();
    m_EntryState.clear();
    SetState(AssetState::Unloaded);
}

bool AnimationController::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

const AnimationControllerState* AnimationController::FindState(const std::string& name) const
{
    for (const auto& state : m_States)
    {
        if (state.Name == name)
            return &state;
    }
    return nullptr;
}

void AnimationController::SetDataForTest(std::vector<AnimationParameter> parameters,
                                         std::vector<AnimationControllerState> states,
                                         std::vector<AnimationControllerTransition> transitions,
                                         std::string entryState)
{
    m_Parameters = std::move(parameters);
    m_States = std::move(states);
    m_Transitions = std::move(transitions);
    m_EntryState = std::move(entryState);
}

bool AnimationController::ParseJson(const std::string& text)
{
    json doc;
    std::string parseError;
    if (!Serialization::ParseJson(text, doc, parseError))
        return Refuse(std::format("it is not JSON ({}); fix the text at that place", parseError));
    if (!doc.is_object())
        return Refuse("its JSON is not an object; write it as an object of parameters, states and transitions");

    Serialization::JsonFieldReader reader;
    m_EntryState.clear();
    reader.Read(doc, "entryState", m_EntryState);
    m_Parameters.clear();
    m_States.clear();
    m_Transitions.clear();

    if (doc.contains("parameters") && doc["parameters"].is_array())
    {
        const json& parameters = doc["parameters"];
        for (size_t index = 0; index < parameters.size(); ++index)
        {
            const json& item = parameters[index];
            reader.SetContext(std::format("parameters[{}].", index));
            AnimationParameter parameter;
            std::string type = "Float";
            parameter.DefaultFloat = 0.0f;
            parameter.DefaultInt = 0;
            parameter.DefaultBool = false;
            reader.Read(item, "name", parameter.Name);
            reader.Read(item, "type", type);
            reader.Read(item, "defaultFloat", parameter.DefaultFloat);
            reader.Read(item, "defaultInt", parameter.DefaultInt);
            reader.Read(item, "defaultBool", parameter.DefaultBool);
            parameter.Type = AnimationParameterTypeFromString(type);
            if (!parameter.Name.empty())
                m_Parameters.push_back(std::move(parameter));
        }
    }

    if (doc.contains("states") && doc["states"].is_array())
    {
        const json& states = doc["states"];
        for (size_t index = 0; index < states.size(); ++index)
        {
            const json& item = states[index];
            const std::string context = std::format("states[{}].", index);
            reader.SetContext(context);
            AnimationControllerState state;
            reader.Read(item, "name", state.Name);
            if (item.is_object() && item.contains("motion") && item["motion"].is_object())
                state.Motion = LoadMotion(item["motion"], reader, context + "motion.");
            if (item.is_object() && item.contains("blendTree") && item["blendTree"].is_object())
            {
                const auto& bt = item["blendTree"];
                reader.SetContext(context + "blendTree.");
                std::string type = "None";
                reader.Read(bt, "type", type);
                state.BlendTree.Type = BlendTreeTypeFromString(type);
                state.BlendTree.ParameterX.clear();
                state.BlendTree.ParameterY.clear();
                reader.Read(bt, "parameterX", state.BlendTree.ParameterX);
                reader.Read(bt, "parameterY", state.BlendTree.ParameterY);
                if (bt.contains("children") && bt["children"].is_array())
                {
                    const json& children = bt["children"];
                    for (size_t childIndex = 0; childIndex < children.size(); ++childIndex)
                    {
                        const json& childJson = children[childIndex];
                        const std::string childContext =
                            std::format("{}blendTree.children[{}].", context, childIndex);
                        AnimationBlendTreeChild child;
                        if (childJson.is_object() && childJson.contains("motion") && childJson["motion"].is_object())
                            child.Motion = LoadMotion(childJson["motion"], reader, childContext + "motion.");
                        reader.SetContext(childContext);
                        child.ThresholdX = 0.0f;
                        child.ThresholdY = 0.0f;
                        reader.Read(childJson, "thresholdX", child.ThresholdX);
                        reader.Read(childJson, "thresholdY", child.ThresholdY);
                        state.BlendTree.Children.push_back(std::move(child));
                    }
                }
            }
            if (!state.Name.empty())
                m_States.push_back(std::move(state));
        }
    }

    if (doc.contains("transitions") && doc["transitions"].is_array())
    {
        const json& transitions = doc["transitions"];
        for (size_t index = 0; index < transitions.size(); ++index)
        {
            const json& item = transitions[index];
            const std::string context = std::format("transitions[{}].", index);
            reader.SetContext(context);
            AnimationControllerTransition transition;
            transition.DurationSeconds = 0.2f;
            transition.ExitTimeSeconds = -1.0f;
            transition.CanInterrupt = true;
            reader.Read(item, "fromState", transition.FromState);
            reader.Read(item, "toState", transition.ToState);
            reader.Read(item, "durationSeconds", transition.DurationSeconds);
            transition.DurationSeconds = std::max(0.0f, transition.DurationSeconds);
            reader.Read(item, "exitTimeSeconds", transition.ExitTimeSeconds);
            reader.Read(item, "canInterrupt", transition.CanInterrupt);
            if (item.is_object() && item.contains("conditions") && item["conditions"].is_array())
            {
                const json& conditions = item["conditions"];
                for (size_t conditionIndex = 0; conditionIndex < conditions.size(); ++conditionIndex)
                {
                    const json& condJson = conditions[conditionIndex];
                    reader.SetContext(std::format("{}conditions[{}].", context, conditionIndex));
                    AnimationTransitionConditionDef condition;
                    std::string comparison = "Always";
                    condition.FloatValue = 0.0f;
                    condition.IntValue = 0;
                    condition.BoolValue = false;
                    reader.Read(condJson, "parameter", condition.Parameter);
                    reader.Read(condJson, "comparison", comparison);
                    reader.Read(condJson, "floatValue", condition.FloatValue);
                    reader.Read(condJson, "intValue", condition.IntValue);
                    reader.Read(condJson, "boolValue", condition.BoolValue);
                    condition.Comparison = AnimationTransitionComparisonFromString(comparison);
                    transition.Conditions.push_back(std::move(condition));
                }
            }
            if (!transition.FromState.empty() && !transition.ToState.empty())
                m_Transitions.push_back(std::move(transition));
        }
    }
    if (reader.Refused())
        return Refuse(reader.Refusal());

    SetState(AssetState::Loaded);
    return true;
}

bool AnimationController::Refuse(std::string_view reason)
{
    Logger::Log::Error("AnimationController: '{}' is not loaded: {}", GetPath().generic_string(), reason);
    Unload();
    SetState(AssetState::Failed);
    return false;
}

std::string AnimationController::SerializeJson() const
{
    json doc;
    doc["schemaVersion"] = kSchemaVersion;
    doc["assetType"] = "AnimationController";
    doc["entryState"] = m_EntryState;

    json parameters = json::array();
    for (const auto& parameter : m_Parameters)
    {
        parameters.push_back(json{
            {"name", parameter.Name},
            {"type", AnimationParameterTypeToString(parameter.Type)},
            {"defaultFloat", parameter.DefaultFloat},
            {"defaultInt", parameter.DefaultInt},
            {"defaultBool", parameter.DefaultBool}
        });
    }
    doc["parameters"] = std::move(parameters);

    json states = json::array();
    for (const auto& state : m_States)
    {
        json stateJson;
        stateJson["name"] = state.Name;
        stateJson["motion"] = SaveMotion(state.Motion);
        json blendTree;
        blendTree["type"] = BlendTreeTypeToString(state.BlendTree.Type);
        blendTree["parameterX"] = state.BlendTree.ParameterX;
        blendTree["parameterY"] = state.BlendTree.ParameterY;
        json children = json::array();
        for (const auto& child : state.BlendTree.Children)
        {
            children.push_back(json{
                {"motion", SaveMotion(child.Motion)},
                {"thresholdX", child.ThresholdX},
                {"thresholdY", child.ThresholdY}
            });
        }
        blendTree["children"] = std::move(children);
        stateJson["blendTree"] = std::move(blendTree);
        states.push_back(std::move(stateJson));
    }
    doc["states"] = std::move(states);

    json transitions = json::array();
    for (const auto& transition : m_Transitions)
    {
        json transitionJson;
        transitionJson["fromState"] = transition.FromState;
        transitionJson["toState"] = transition.ToState;
        transitionJson["durationSeconds"] = transition.DurationSeconds;
        transitionJson["exitTimeSeconds"] = transition.ExitTimeSeconds;
        transitionJson["canInterrupt"] = transition.CanInterrupt;
        json conditions = json::array();
        for (const auto& condition : transition.Conditions)
        {
            conditions.push_back(json{
                {"parameter", condition.Parameter},
                {"comparison", AnimationTransitionComparisonToString(condition.Comparison)},
                {"floatValue", condition.FloatValue},
                {"intValue", condition.IntValue},
                {"boolValue", condition.BoolValue}
            });
        }
        transitionJson["conditions"] = std::move(conditions);
        transitions.push_back(std::move(transitionJson));
    }
    doc["transitions"] = std::move(transitions);
    return doc.dump(2);
}

} // namespace Animation
} // namespace GameEngine
