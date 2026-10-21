#include "ApiJsonEmitter.h"

#include "JsonWriter.h"

#include "Types/Fnv1a.h"

#include <cstdio>

namespace GameEngine::ComponentScanner::ApiJsonEmitter
{
namespace
{

std::string Id(const ApiModule& module, const std::string& name)
{
    return module.Name + "." + name;
}

std::string HexHash(std::uint64_t hash)
{
    char text[19];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// The type's fields in place: "kind", then "type" or "element".
void WriteType(JsonWriter& w, const ApiType& type)
{
    w.String("kind", ApiKindName(type.Kind));
    if (!type.TypeId.empty())
        w.String("type", type.TypeId);
    if (!type.Element.empty())
    {
        w.StartObject("element");
        WriteType(w, type.Element.front());
        w.EndObject();
    }
}

void WriteSource(JsonWriter& w, const ApiSource& source)
{
    w.String("header", source.Header);
    w.Number("line", static_cast<std::int64_t>(source.Line));
}

void WriteConstant(JsonWriter& w, const ApiModule& module, const ApiConstant& constant)
{
    w.StartObject();
    w.String("id", Id(module, constant.Name));
    w.String("name", constant.Name);
    w.String("cpp", constant.CppName);
    WriteSource(w, constant.Source);
    WriteType(w, constant.Type);
    w.String("value", constant.Literal);
    w.EndObject();
}

void WriteEnum(JsonWriter& w, const ApiModule& module, const ApiEnum& e)
{
    w.StartObject();
    w.String("id", Id(module, e.Name));
    w.String("name", e.Name);
    w.String("cpp", e.CppName);
    WriteSource(w, e.Source);
    w.String("doc", e.Doc);
    w.String("underlying", ApiKindName(e.Underlying));
    if (e.Flags)
        w.Boolean("flags", true);
    w.StartArray("values");
    for (const ApiEnumValue& value : e.Values)
    {
        w.StartObject();
        w.String("name", value.Name);
        w.Number("value", value.Value);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

void WriteEvent(JsonWriter& w, const ApiModule& module, const ApiEvent& event)
{
    w.StartObject();
    w.String("id", Id(module, event.Name));
    w.String("name", event.Name);
    WriteSource(w, event.Source);
    w.String("doc", event.Doc);
    w.StartObject("scope");
    WriteType(w, event.Scope);
    w.EndObject();
    w.String("payload", event.PayloadId);
    w.String("delivery", ApiDeliveryName(event.Delivery));
    w.String("subscribe", event.SubscribeCpp);
    w.String("unsubscribe", event.UnsubscribeCpp);
    if (event.Context)
        w.String("context", *event.Context);
    w.EndObject();
}

void WriteDefault(JsonWriter& w, const std::optional<ApiDefault>& value)
{
    if (!value)
        return;
    w.String("default", value->Literal);
    if (value->ConstantId)
        w.String("defaultConstant", *value->ConstantId);
}

void WriteFunction(JsonWriter& w, const ApiModule& module, const ApiFunction& fn)
{
    w.StartObject();
    w.String("id", Id(module, fn.Name));
    w.String("name", fn.Name);
    w.String("cpp", fn.CppName);
    WriteSource(w, fn.Source);
    w.String("doc", fn.Doc);
    w.String("thread", ApiThreadName(fn.Thread));
    if (fn.Try)
        w.Boolean("try", true);
    if (fn.Context)
        w.String("context", *fn.Context);
    w.StartObject("returns");
    WriteType(w, fn.Returns);
    w.EndObject();
    w.StartArray("params");
    for (const ApiParam& param : fn.Params)
    {
        w.StartObject();
        w.String("name", param.Name);
        WriteType(w, param.Type);
        if (param.Out)
            w.Boolean("out", true);
        WriteDefault(w, param.Default);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

void WriteHandle(JsonWriter& w, const ApiModule& module, const ApiHandle& handle)
{
    w.StartObject();
    w.String("id", Id(module, handle.Name));
    w.String("name", handle.Name);
    w.String("cpp", handle.CppName);
    WriteSource(w, handle.Source);
    w.String("doc", handle.Doc);
    w.Number("width", static_cast<std::int64_t>(handle.Width));
    w.EndObject();
}

void WriteRecord(JsonWriter& w, const ApiModule& module, const ApiRecord& record)
{
    w.StartObject();
    w.String("id", Id(module, record.Name));
    w.String("name", record.Name);
    w.String("cpp", record.CppName);
    WriteSource(w, record.Source);
    w.String("doc", record.Doc);
    w.Number("size", static_cast<std::int64_t>(record.Size));
    w.Number("align", static_cast<std::int64_t>(record.Align));
    w.StartArray("fields");
    for (const ApiField& field : record.Fields)
    {
        w.StartObject();
        w.String("name", field.Name);
        WriteType(w, field.Type);
        w.Number("offset", static_cast<std::int64_t>(field.Offset));
        if (field.Default.size() == 1)
            w.String("default", field.Default.front());
        else if (!field.Default.empty())
        {
            w.StartArray("default");
            for (const std::string& component : field.Default)
                w.String({}, component);
            w.EndArray();
        }
        if (field.DefaultConstantId)
            w.String("defaultConstant", *field.DefaultConstantId);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

std::string TypeKey(const ApiType& type)
{
    std::string key(ApiKindName(type.Kind));
    if (!type.TypeId.empty())
        key += "(" + type.TypeId + ")";
    if (!type.Element.empty())
        key += "<" + TypeKey(type.Element.front()) + ">";
    return key;
}

} // namespace

std::string ModuleHashInput(const ApiModule& module)
{
    std::string text = "module " + module.Name + "\n";
    for (const ApiConstant& c : module.Constants)
        text += "constant " + c.CppName + " " + TypeKey(c.Type) + " " + c.Literal + "\n";
    for (const ApiEnum& e : module.Enums)
    {
        text += "enum " + e.CppName + " " + std::string(ApiKindName(e.Underlying)) + (e.Flags ? " flags" : "");
        for (const ApiEnumValue& v : e.Values)
            text += " " + v.Name + "=" + std::to_string(v.Value);
        text += "\n";
    }
    for (const ApiEvent& ev : module.Events)
        text += "event " + ev.Name + " " + TypeKey(ev.Scope) + " " + ev.PayloadId + " " + std::string(ApiDeliveryName(ev.Delivery)) +
                " " + ev.SubscribeCpp + " " + ev.UnsubscribeCpp + "\n";
    for (const ApiFunction& fn : module.Functions)
    {
        text += "function " + fn.CppName + " " + std::string(ApiThreadName(fn.Thread)) + (fn.Try ? " try" : "") + " " +
                TypeKey(fn.Returns) + " (";
        for (const ApiParam& p : fn.Params)
        {
            text += " " + p.Name + ":" + TypeKey(p.Type) + (p.Out ? " out" : "");
            if (p.Default)
                text += "=" + p.Default->Literal;
        }
        text += " )\n";
    }
    for (const ApiHandle& h : module.Handles)
        text += "handle " + h.CppName + " " + std::to_string(h.Width) + "\n";
    for (const ApiRecord& r : module.Records)
    {
        text += "record " + r.CppName + " " + std::to_string(r.Size) + " " + std::to_string(r.Align);
        for (const ApiField& f : r.Fields)
        {
            text += " " + f.Name + ":" + TypeKey(f.Type) + "@" + std::to_string(f.Offset);
            for (const std::string& d : f.Default)
                text += "=" + d;
        }
        text += "\n";
    }
    return text;
}

std::uint64_t ModuleHash(const ApiModule& module)
{
    return Hashing::Fnv1a64(ModuleHashInput(module));
}

std::string EmitModule(const ApiModule& module)
{
    JsonWriter w;
    w.StartObject();
    w.Number("format", kApiDescriptionFormat);
    w.String("name", module.Name);
    w.String("hash", HexHash(ModuleHash(module)));
    w.StartArray("constants");
    for (const ApiConstant& c : module.Constants)
        WriteConstant(w, module, c);
    w.EndArray();
    w.StartArray("enums");
    for (const ApiEnum& e : module.Enums)
        WriteEnum(w, module, e);
    w.EndArray();
    w.StartArray("events");
    for (const ApiEvent& ev : module.Events)
        WriteEvent(w, module, ev);
    w.EndArray();
    w.StartArray("functions");
    for (const ApiFunction& fn : module.Functions)
        WriteFunction(w, module, fn);
    w.EndArray();
    w.StartArray("handles");
    for (const ApiHandle& h : module.Handles)
        WriteHandle(w, module, h);
    w.EndArray();
    w.StartArray("records");
    for (const ApiRecord& r : module.Records)
        WriteRecord(w, module, r);
    w.EndArray();
    w.EndObject();
    return w.Text + "\n";
}

} // namespace GameEngine::ComponentScanner::ApiJsonEmitter
