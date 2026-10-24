// The web emitters (--emit-dts, --emit-json) run end to end through the scanner's entry
// point over a fixture header, beside the C++ registration output of the same parse.
#include "CommentStripper.h"
#include "ComponentScanner.h"
#include "EnumScanner.h"
#include "FieldKind.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace GameEngine::ComponentScanner;
namespace fs = std::filesystem;

namespace
{

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

class WebEmitterTest : public ::testing::Test
{
  protected:
    GameEngine::TestUtils::ScopedTempDir m_Root{GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerWeb")};
    std::ostringstream m_Out;
    std::ostringstream m_Err;

    fs::path ComponentsDir() const { return m_Root.Path() / "Include" / "Components"; }
    fs::path CppPath() const { return m_Root.Path() / "out" / "Reflection.gen.cpp"; }
    fs::path DtsPath() const { return m_Root.Path() / "out" / "components.d.ts"; }
    fs::path JsonPath() const { return m_Root.Path() / "out" / "components.json"; }

    int Scan(const std::string& header)
    {
        fs::create_directories(ComponentsDir());
        std::ofstream(ComponentsDir() / "Fixture.h", std::ios::binary) << header;
        return GameEngine::ComponentScanner::Run({"--in", ComponentsDir().string(), "--out", CppPath().string(), "--emit-dts", DtsPath().string(),
                    "--emit-json", JsonPath().string()},
                   m_Out, m_Err);
    }
};

// Every kind the resolver produces, every marker, an alias, an enum and an editor-only component.
const char* kEveryKindHeader = R"(#pragma once
namespace GameEngine::Components {
using MaterialRef = AssetRef<AssetType::Material>;
enum class Falloff : uint8 { Linear = 0, Smooth = 1, Sharp = 4 };
struct Probe
{
    bool Enabled = true;
    int8 A = 0;
    int16 B = 0;
    int32 C = 0;
    int64 D = 0;
    uint8 E = 0;
    uint16 F = 0;
    uint32 G = 0;
    std::uint64_t H = 0;
    float I = 0.0f;
    double J = 0.0;
    Mathematics::Vector2 Uv;
    Mathematics::Vector3 Position;
    Mathematics::Vector4 Plane;
    Mathematics::Quaternion Rotation;
    ColorLinear Tint;
    MaterialRef Material;
    ECS::EntityHandle Target;
    char Label[16] = {};
    // @ge-tooltip Brightness in lux */ of the probe.
    // @ge-range 0 10
    float Intensity = 1.0f;
    // @ge-range 0.5
    float Radius = 1.0f;
    float Weights[kWeightCount] = {};
    uint8 Bytes[4] = {};
    float Coefficients[3] = {};
    float Samples[5] = {};
    float FovY = 60.0f;
    float IBLScale = 1.0f;
    Falloff Mode = Falloff::Linear;
    Falloff Modes[2] = {};
    uint32 Version = 0; // @ge-readonly
    uint32 CacheSlot = 0; // @ge-hidden
    uint32 LiveBodyId = 0; // [DoNotSerialize]
};
// @ge-editor-only
struct EditorNote
{
    float Size = 1.0f;
};
// @ge-no-add
struct Anchor
{
    Mathematics::Vector3 Offset;
};
}
)";

} // namespace

TEST_F(WebEmitterTest, EmitsOneInterfacePerComponentWithEveryKindAndMarker)
{
    ASSERT_EQ(0, Scan(kEveryKindHeader)) << m_Err.str();
    const char* expected = R"(// Generated from the engine's component headers by ComponentScanner --emit-dts. Do not edit.
// One interface and one ComponentType value per reflected component; & NoAdd marks the ones a page
// cannot add or remove. Each field's type follows its reflected kind.

import type { AssetRef, Color, ComponentType, Entity, NoAdd, Quat, Vec2, Vec3, Vec4 } from './opengine';

export type Falloff = 'Linear' | 'Smooth' | 'Sharp';

export interface Anchor {
    offset: Vec3;
}
export declare const Anchor: ComponentType<Anchor> & NoAdd;

export interface Probe {
    enabled: boolean;
    a: number;
    b: number;
    c: number;
    d: bigint;
    e: number;
    f: number;
    g: number;
    h: bigint;
    i: number;
    j: number;
    uv: Vec2;
    position: Vec3;
    plane: Vec4;
    rotation: Quat;
    tint: Color;
    material: AssetRef;
    target: Entity | null;
    label: string;
    /**
     * Brightness in lux *\/ of the probe.
     * Range: 0 to 10.
     */
    intensity: number;
    /** Minimum: 0.5. */
    radius: number;
    weights: number[];
    bytes: number[];
    coefficients: [number, number, number];
    samples: number[];
    fovY: number;
    iblScale: number;
    mode: Falloff;
    modes: Falloff[];
    readonly version: number;
}
export declare const Probe: ComponentType<Probe>;
)";
    EXPECT_EQ(expected, ReadFile(DtsPath()));
}

TEST_F(WebEmitterTest, EmitsTheSameModelAsJson)
{
    ASSERT_EQ(0, Scan(kEveryKindHeader)) << m_Err.str();
    const char* expected = R"({
  "components": [
    {
      "name": "Anchor",
      "fields": [
        {
          "name": "Offset",
          "tsName": "offset",
          "kind": "Vec3"
        }
      ]
    },
    {
      "name": "Probe",
      "fields": [
        {
          "name": "Enabled",
          "tsName": "enabled",
          "kind": "Bool"
        },
        {
          "name": "A",
          "tsName": "a",
          "kind": "Int8"
        },
        {
          "name": "B",
          "tsName": "b",
          "kind": "Int16"
        },
        {
          "name": "C",
          "tsName": "c",
          "kind": "Int32"
        },
        {
          "name": "D",
          "tsName": "d",
          "kind": "Int64"
        },
        {
          "name": "E",
          "tsName": "e",
          "kind": "UInt8"
        },
        {
          "name": "F",
          "tsName": "f",
          "kind": "UInt16"
        },
        {
          "name": "G",
          "tsName": "g",
          "kind": "UInt32"
        },
        {
          "name": "H",
          "tsName": "h",
          "kind": "UInt64"
        },
        {
          "name": "I",
          "tsName": "i",
          "kind": "Float"
        },
        {
          "name": "J",
          "tsName": "j",
          "kind": "Double"
        },
        {
          "name": "Uv",
          "tsName": "uv",
          "kind": "Vec2"
        },
        {
          "name": "Position",
          "tsName": "position",
          "kind": "Vec3"
        },
        {
          "name": "Plane",
          "tsName": "plane",
          "kind": "Vec4"
        },
        {
          "name": "Rotation",
          "tsName": "rotation",
          "kind": "Quat"
        },
        {
          "name": "Tint",
          "tsName": "tint",
          "kind": "Color"
        },
        {
          "name": "Material",
          "tsName": "material",
          "kind": "AssetGuid"
        },
        {
          "name": "Target",
          "tsName": "target",
          "kind": "EntityHandle"
        },
        {
          "name": "Label",
          "tsName": "label",
          "kind": "String",
          "count": 16
        },
        {
          "name": "Intensity",
          "tsName": "intensity",
          "kind": "Float",
          "tooltip": "Brightness in lux */ of the probe.",
          "min": 0,
          "max": 10
        },
        {
          "name": "Radius",
          "tsName": "radius",
          "kind": "Float",
          "min": 0.5
        },
        {
          "name": "Weights",
          "tsName": "weights",
          "kind": "Float",
          "count": "kWeightCount"
        },
        {
          "name": "Bytes",
          "tsName": "bytes",
          "kind": "UInt8",
          "count": 4
        },
        {
          "name": "Coefficients",
          "tsName": "coefficients",
          "kind": "Float",
          "count": 3
        },
        {
          "name": "Samples",
          "tsName": "samples",
          "kind": "Float",
          "count": 5
        },
        {
          "name": "FovY",
          "tsName": "fovY",
          "kind": "Float"
        },
        {
          "name": "IBLScale",
          "tsName": "iblScale",
          "kind": "Float"
        },
        {
          "name": "Mode",
          "tsName": "mode",
          "kind": "UInt8",
          "enum": "Falloff"
        },
        {
          "name": "Modes",
          "tsName": "modes",
          "kind": "UInt8",
          "enum": "Falloff",
          "count": 2
        },
        {
          "name": "Version",
          "tsName": "version",
          "kind": "UInt32",
          "readonly": true
        }
      ]
    }
  ],
  "enums": [
    {
      "name": "Falloff",
      "values": [
        {
          "name": "Linear",
          "value": 0
        },
        {
          "name": "Smooth",
          "value": 1
        },
        {
          "name": "Sharp",
          "value": 4
        }
      ]
    }
  ],
  "omitted": []
}
)";
    EXPECT_EQ(expected, ReadFile(JsonPath()));
}

TEST_F(WebEmitterTest, EditorOnlyComponentIsAbsentFromBothOutputsAndStillRegistered)
{
    ASSERT_EQ(0, Scan(kEveryKindHeader)) << m_Err.str();
    EXPECT_EQ(std::string::npos, ReadFile(DtsPath()).find("EditorNote"));
    EXPECT_EQ(std::string::npos, ReadFile(JsonPath()).find("EditorNote"));
    EXPECT_NE(std::string::npos, ReadFile(CppPath()).find("GE_REFLECT_COMPONENT_EDITORONLY(GameEngine::Components::EditorNote);"));
}

TEST_F(WebEmitterTest, EngineInternalComponentIsAbsentFromTheTypesAndMarkedInJson)
{
    const char* header = R"(namespace GameEngine::Components {
enum class SlotState : uint8 { Free = 0, Taken = 1 };
// @ge-engine-internal
struct GpuSlot
{
    uint32 Slot = 0;
    SlotState State = SlotState::Free;
};
struct Probe
{
    float Size = 1.0f;
};
}
)";
    ASSERT_EQ(0, Scan(header)) << m_Err.str();
    const std::string dts = ReadFile(DtsPath());
    EXPECT_EQ(std::string::npos, dts.find("GpuSlot"));
    EXPECT_EQ(std::string::npos, dts.find("SlotState"));
    EXPECT_NE(std::string::npos, dts.find("export interface Probe {"));
    EXPECT_NE(std::string::npos, ReadFile(JsonPath()).find("\"name\": \"GpuSlot\",\n      \"engineInternal\": true,\n      \"fields\": ["));
    EXPECT_NE(std::string::npos, ReadFile(CppPath()).find("GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::GpuSlot, Slot)"));
}

TEST_F(WebEmitterTest, FieldWithoutAKindIsOmittedAndListedWithWhyTheScannerCannotTypeIt)
{
    const char* header = R"(namespace GameEngine::Rendering {
struct DayKeys { float Values[4]; };
}
namespace GameEngine::Components {
struct Keyed
{
    float Speed = 1.0f;
    Rendering::DayKeys Keys;
    Math::Curve Shape;
    long Count = 0;
};
}
)";
    ASSERT_EQ(0, Scan(header)) << m_Err.str();
    const std::string error = m_Err.str();
    EXPECT_NE(std::string::npos, error.find("warning: Keyed.Keys (type 'Rendering::DayKeys') is left out of the web types: a struct with no reflected kind"));
    EXPECT_NE(std::string::npos, error.find("warning: Keyed.Shape (type 'Math::Curve') is left out of the web types: the type is not declared in the scanned component headers"));
    EXPECT_NE(std::string::npos, ReadFile(DtsPath()).find("export interface Keyed {\n    speed: number;\n}\n"));
    const char* omitted = R"("omitted": [
    {
      "component": "Keyed",
      "field": "Keys",
      "type": "Rendering::DayKeys",
      "reason": "noReflectedKind"
    },
    {
      "component": "Keyed",
      "field": "Shape",
      "type": "Math::Curve",
      "reason": "unresolvedType"
    },
    {
      "component": "Keyed",
      "field": "Count",
      "type": "long",
      "reason": "unresolvedType"
    }
  ])";
    EXPECT_NE(std::string::npos, ReadFile(JsonPath()).find(omitted));
    EXPECT_NE(std::string::npos, ReadFile(CppPath()).find("GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::Keyed, Shape)"));
}

TEST_F(WebEmitterTest, EnumIsMatchedBeforeAnAliasOfTheSameName)
{
    const char* header = R"(namespace GameEngine::Components {
enum class Mode : uint8 { Off, On };
struct Helper { using Mode = int32; };
struct Lamp { Mode State = Mode::Off; };
}
)";
    ASSERT_EQ(0, Scan(header)) << m_Err.str();
    EXPECT_NE(std::string::npos, ReadFile(DtsPath()).find("    state: Mode;\n"));
    EXPECT_NE(std::string::npos, ReadFile(CppPath()).find("GE_REFLECT_ENUM_FIELD(GameEngine::Components::Lamp, State, Mode)"));
}

TEST_F(WebEmitterTest, EnumNamedLikeAComponentIsRefusedAndNothingIsWritten)
{
    const char* header = R"(namespace Other { enum class Probe : uint8 { A, B }; }
namespace GameEngine::Components {
struct Probe { float X = 0.0f; };
struct Sensor { Other::Probe Kind = Other::Probe::A; };
}
)";
    EXPECT_EQ(1, Scan(header));
    EXPECT_NE(std::string::npos, m_Err.str().find("error: the enum Probe and the component Probe would share one name in components.d.ts"));
    EXPECT_FALSE(fs::exists(DtsPath()));
    EXPECT_FALSE(fs::exists(CppPath()));
}

TEST_F(WebEmitterTest, MultiDimensionalArrayIsRefusedByNameAndNothingIsWritten)
{
    const char* header = R"(namespace GameEngine::Components {
struct Grid { float M[4][4] = {}; };
}
)";
    EXPECT_EQ(1, Scan(header));
    EXPECT_NE(std::string::npos, m_Err.str().find("error: Grid.M is a multi-dimensional array"));
    EXPECT_FALSE(fs::exists(DtsPath()));
    EXPECT_FALSE(fs::exists(CppPath()));
}

// A render layer mask is its own kind, not the UInt32 it holds: the page reads the layer word as a number.
TEST_F(WebEmitterTest, RenderLayerMaskFieldResolvesToItsKind)
{
    ASSERT_EQ(0, Scan(R"(#pragma once
namespace GameEngine::Components {
struct Layered
{
    Rendering::RenderLayerMask RenderLayers;
};
}
)")) << m_Err.str();
    const std::string json = ReadFile(JsonPath());
    EXPECT_NE(std::string::npos, json.find(R"("kind": "RenderLayerMask")")) << json;
    EXPECT_NE(std::string::npos, ReadFile(DtsPath()).find("    renderLayers: number;"));
    EXPECT_EQ(std::string::npos, m_Err.str().find("warning")) << m_Err.str();
}

// AssetRef<...> is the one templated field type the registry classifies (Components/AssetRef.h):
// a component holding one reflects, with the field as an asset GUID.
TEST_F(WebEmitterTest, AssetRefFieldReflectsAsAnAssetGuid)
{
    ASSERT_EQ(0, Scan(R"(#pragma once
namespace GameEngine::Components {
struct Linked
{
    AssetRef<AssetType::Model> Mesh;
    float Scale = 1.0f;
};
}
)")) << m_Err.str();
    EXPECT_NE(std::string::npos, m_Out.str().find("ComponentScanner.summary reflected=1 skipped=0")) << m_Out.str();
    EXPECT_NE(std::string::npos, ReadFile(CppPath()).find("GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::Linked, Mesh)"));
    const std::string json = ReadFile(JsonPath());
    EXPECT_NE(std::string::npos, json.find(R"("name": "Mesh",
          "tsName": "mesh",
          "kind": "AssetGuid")")) << json;
}

// An enum declared in a module header the component header includes, outside every scanned root,
// gets its name table from that header; the depfile names the header so an edit re-runs the scan.
TEST_F(WebEmitterTest, EnumDeclaredInAnIncludedModuleHeaderGetsItsNameTable)
{
    const fs::path moduleInclude = m_Root.Path() / "Modules" / "Grid" / "Include";
    fs::create_directories(moduleInclude / "Grid");
    std::ofstream(moduleInclude / "Grid" / "GridTypes.h", std::ios::binary)
        << "#pragma once\nnamespace GameEngine::Grid {\nenum class GridType : uint8 { Square = 0, Hex = 1 };\n"
           "enum class Unused : uint8 { A = 0 };\n}\n";
    fs::create_directories(ComponentsDir());
    std::ofstream(ComponentsDir() / "Fixture.h", std::ios::binary)
        << "#pragma once\n#include \"Grid/GridTypes.h\"\nnamespace GameEngine::Components {\n"
           "struct Surface { Grid::GridType Cells = Grid::GridType::Square; float Size = 1.0f; };\n}\n";
    const fs::path depFile = m_Root.Path() / "out" / "Reflection.d";

    ASSERT_EQ(0, GameEngine::ComponentScanner::Run({"--in", ComponentsDir().string(), "--enum-include-root",
                                                    moduleInclude.string(), "--out", CppPath().string(), "--emit-json",
                                                    JsonPath().string(), "--depfile", depFile.string()},
                                                   m_Out, m_Err))
        << m_Err.str();
    const std::string cpp = ReadFile(CppPath());
    EXPECT_NE(std::string::npos, cpp.find("GE_REFLECT_ENUM_TABLE_BEGIN(GridType)\n"
                                          "    GE_REFLECT_ENUM_TABLE_VALUE(\"Square\", 0)\n"
                                          "    GE_REFLECT_ENUM_TABLE_VALUE(\"Hex\", 1)\n"))
        << cpp;
    EXPECT_NE(std::string::npos, cpp.find("GE_REFLECT_ENUM_FIELD(GameEngine::Components::Surface, Cells, GridType)"));
    EXPECT_EQ(std::string::npos, cpp.find("Unused"));
    EXPECT_NE(std::string::npos, ReadFile(JsonPath()).find(R"("enum": "GridType")"));
    EXPECT_NE(std::string::npos, ReadFile(depFile).find("Grid/GridTypes.h")) << ReadFile(depFile);
}

// "// @ge-label" on a field and on an enumerator, in the component header and in a header it
// includes, reaches the registration, the JSON and the types; an unmarked field or enumerator
// gets no label.
TEST_F(WebEmitterTest, LabelMarkersReachTheRegistrationTheJsonAndTheTypes)
{
    const fs::path moduleInclude = m_Root.Path() / "Modules" / "Grid" / "Include";
    fs::create_directories(moduleInclude / "Grid");
    std::ofstream(moduleInclude / "Grid" / "GridTypes.h", std::ios::binary)
        << "#pragma once\nnamespace GameEngine::Grid {\nenum class GridType : uint8 {\n    Square = 0,\n"
           "    // @ge-label six-sided\n    Hex = 1,\n};\n}\n";
    fs::create_directories(ComponentsDir());
    std::ofstream(ComponentsDir() / "Fixture.h", std::ios::binary) << R"(#pragma once
#include "Grid/GridTypes.h"
namespace GameEngine::Components {
enum class Rate : uint32 {
    Once = 0,
    Realtime = 1, // @ge-label Realtime (on change)
};
struct Probe
{
    // @ge-label Rays / Probe
    int32 RaysPerProbe = 64;
    float BlendDistance = 1.0f;
    Rate Mode = Rate::Once;
    Grid::GridType Cells = Grid::GridType::Square;
};
}
)";
    ASSERT_EQ(0, GameEngine::ComponentScanner::Run({"--in", ComponentsDir().string(), "--enum-include-root",
                                                    moduleInclude.string(), "--out", CppPath().string(), "--emit-dts",
                                                    DtsPath().string(), "--emit-json", JsonPath().string()},
                                                   m_Out, m_Err))
        << m_Err.str();

    const std::string cpp = ReadFile(CppPath());
    EXPECT_NE(std::string::npos,
              cpp.find("GE_REFLECT_FIELD_LABEL(GameEngine::Components::Probe, RaysPerProbe, \"Rays / Probe\");\n"))
        << cpp;
    EXPECT_EQ(cpp.find("GE_REFLECT_FIELD_LABEL("), cpp.rfind("GE_REFLECT_FIELD_LABEL(")) << "one labeled field";
    EXPECT_NE(std::string::npos, cpp.find("    GE_REFLECT_ENUM_TABLE_VALUE(\"Once\", 0)\n"
                                          "    GE_REFLECT_ENUM_TABLE_LABELED_VALUE(\"Realtime\", 1, \"Realtime (on change)\")\n"))
        << cpp;
    EXPECT_NE(std::string::npos, cpp.find("    GE_REFLECT_ENUM_TABLE_VALUE(\"Square\", 0)\n"
                                          "    GE_REFLECT_ENUM_TABLE_LABELED_VALUE(\"Hex\", 1, \"six-sided\")\n"))
        << cpp;

    const std::string json = ReadFile(JsonPath());
    EXPECT_NE(std::string::npos, json.find(R"("kind": "Int32",
          "label": "Rays / Probe")")) << json;
    EXPECT_NE(std::string::npos, json.find(R"j("name": "Realtime",
          "value": 1,
          "label": "Realtime (on change)")j")) << json;

    const std::string dts = ReadFile(DtsPath());
    EXPECT_NE(std::string::npos, dts.find("    /** Label: Rays / Probe */\n    raysPerProbe: number;\n    blendDistance: number;\n"))
        << dts;
    EXPECT_NE(std::string::npos, dts.find("/** 'Realtime': Realtime (on change) */\nexport type Rate = 'Once' | 'Realtime';\n"))
        << dts;
}

// A hidden field has no inspector row: its label is left out of every output, with a warning
// that names the field and the fix.
TEST_F(WebEmitterTest, LabelOnAHiddenFieldIsIgnoredWithAWarning)
{
    ASSERT_EQ(0, Scan(R"(#pragma once
namespace GameEngine::Components {
struct Probe
{
    float BlendDistance = 1.0f;
    // @ge-hidden
    // @ge-label Cache Slot
    uint32 CacheSlot = 0;
};
}
)")) << m_Err.str();
    EXPECT_NE(std::string::npos, m_Err.str().find("Fixture.h:8: field 'CacheSlot' is @ge-hidden and has no inspector row, "
                                                  "so its @ge-label is ignored. Fix: remove one of the two markers."))
        << m_Err.str();
    EXPECT_EQ(std::string::npos, ReadFile(CppPath()).find("GE_REFLECT_FIELD_LABEL("));
    EXPECT_EQ(std::string::npos, ReadFile(JsonPath()).find("\"label\""));
}

// Two included enums share the simple name `Mode`. A field names one of them by its scope, as C++
// finds it from the component, and gets that enum's names; fields naming both leave the name
// ambiguous, and the warning names both headers.
TEST_F(WebEmitterTest, IncludedEnumIsMatchedByScopeAndATieNamesBothHeaders)
{
    const fs::path moduleInclude = m_Root.Path() / "Modules";
    fs::create_directories(moduleInclude / "Power");
    fs::create_directories(moduleInclude / "Speed");
    std::ofstream(moduleInclude / "Power" / "Modes.h", std::ios::binary)
        << "#pragma once\nnamespace GameEngine::Power { enum class Mode : uint8 { Off = 0, On = 1 }; }\n";
    std::ofstream(moduleInclude / "Speed" / "Modes.h", std::ios::binary)
        << "#pragma once\nnamespace GameEngine::Speed { enum class Mode : uint8 { Slow = 0, Fast = 1 }; }\n";
    fs::create_directories(ComponentsDir());
    std::ofstream(ComponentsDir() / "Switch.h", std::ios::binary)
        << "#pragma once\n#include \"Power/Modes.h\"\n#include \"Speed/Modes.h\"\nnamespace GameEngine::Components {\n"
           "struct Switch { Power::Mode State = Power::Mode::Off; };\n}\n";
    const std::vector<std::string> args{"--in", ComponentsDir().string(), "--enum-include-root", moduleInclude.string(),
                                        "--out", CppPath().string()};

    ASSERT_EQ(0, GameEngine::ComponentScanner::Run(args, m_Out, m_Err)) << m_Err.str();
    const std::string scoped = ReadFile(CppPath());
    EXPECT_NE(std::string::npos, scoped.find("GE_REFLECT_ENUM_TABLE_VALUE(\"On\", 1)")) << scoped;
    EXPECT_EQ(std::string::npos, scoped.find("Fast")) << scoped;
    EXPECT_NE(std::string::npos, scoped.find("GE_REFLECT_ENUM_FIELD(GameEngine::Components::Switch, State, Mode)"));

    std::ofstream(ComponentsDir() / "Gearbox.h", std::ios::binary)
        << "#pragma once\n#include \"Power/Modes.h\"\n#include \"Speed/Modes.h\"\nnamespace GameEngine::Components {\n"
           "struct Gearbox { Speed::Mode Gear = Speed::Mode::Slow; };\n}\n";
    m_Err.str("");
    ASSERT_EQ(0, GameEngine::ComponentScanner::Run(args, m_Out, m_Err)) << m_Err.str();
    EXPECT_EQ(std::string::npos, ReadFile(CppPath()).find("GE_REFLECT_ENUM_FIELD("));
    const std::string warning = m_Err.str();
    EXPECT_NE(std::string::npos, warning.find("warning: the enum name `Mode` stands for two enums")) << warning;
    EXPECT_NE(std::string::npos, warning.find("Power/Modes.h:2 (GameEngine::Power::Mode)")) << warning;
    EXPECT_NE(std::string::npos, warning.find("Speed/Modes.h:2 (GameEngine::Speed::Mode)")) << warning;
}

TEST(FieldKind, MatchesTheEnginesFieldTypeId)
{
    const std::string source = CommentStripper::Strip(ReadFile(GE_ECS_REFLECTION_HEADER)).Text;
    ASSERT_FALSE(source.empty()) << GE_ECS_REFLECTION_HEADER;

    std::vector<EnumDecl> declared;
    for (EnumDecl& e : EnumScanner::Scan(source))
        if (e.Name == "FieldTypeId")
            declared.push_back(std::move(e));
    ASSERT_EQ(1u, declared.size());

    const EnumDecl& fieldTypeId = declared[0];
    ASSERT_EQ(std::size(kFieldKinds), fieldTypeId.Members.size());
    for (std::size_t i = 0; i < fieldTypeId.Members.size(); ++i)
    {
        EXPECT_EQ(fieldTypeId.Members[i].Name, kFieldKinds[i].first);
        EXPECT_EQ(fieldTypeId.Members[i].Value, static_cast<std::int64_t>(kFieldKinds[i].second));
    }
}
