#pragma once

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

// Scans the C++ ECS component headers and emits the self-registering reflection TU the
// engine compiles (ComponentReflection.gen.cpp), and from the same parse the web library's
// components.d.ts and components.json.
//
// Modes, selected by the command line:
//   engine (default)            structs declared directly in namespace GameEngine::Components
//   --detect-base <Base>        structs inheriting a base named <Base>, in any namespace (user
//                               native scripts; include paths relative to each --in root)
//   --detect-system-base <Base> structs inheriting <Base> or EntitySystem<Self>: emits the
//                               user-system registration TU instead (no field extraction)
//   --emit-api <dir> --api-modules <dir> [--api-search <dir>]...
//                               the script API description of the module tree's
//                               *ScriptApi.h headers (ApiScanner.h), one <Module>.json per
//                               module; its own invocation, exit 1 with every refusal named
//                               when a marked declaration is refused
//     [--emit-native <dir>]     with it, the native bindings of each module
//                               (<Module>.script.gen.h and .cpp, NativeBindingEmitter.h)
//     [--emit-csharp <dir>]     with it, the C# binding of each module (<Module>.g.cs,
//                               CSharpBindingEmitter.h); with either, a declaration the
//                               generators cannot emit is refused (BindingRules.h)
//     [--api-components <dir>]  (repeatable) with it, the C# mirrors of the components of
//                               these roots (ComponentMirror.h): component-classes.txt in the
//                               --emit-api dir, Components/<header>.g.cs under --emit-csharp,
//                               Components.layout.gen.cpp in --emit-native; a component the
//                               mirror rules refuse fails the scan
// Options: --in <dir> (repeatable; include paths are computed relative to each root's
// "Include" dir, and structs already seen from an earlier root are de-duplicated by
// fully-qualified name), --out <file>, --emit-dts <file>, --emit-json <file>, --dry-run,
// --enum-include-root <dir> (repeatable; an include root searched for the headers a component
// header includes, below), --depfile <file> (the headers read that way, as a make-style
// dependency file of --out).
//
// An enum a reflected field names that no scanned header declares is read from the headers the
// component header includes, followed transitively: beside the including header, then under the
// scan's include roots and each --enum-include-root. Only that enum is taken from those headers.
//
// A struct is skipped, and named in the report, when its fields look non-POD (pointers,
// references, templates other than AssetRef<...>, std:: containers, virtual members) or hide
// behind an unexpanded field-injection macro. The report ends with a stable summary line
// ("ComponentScanner.summary reflected=N skipped=N sourcewarnings=N") the native-script
// build pipeline parses.
namespace GameEngine::ComponentScanner
{

// The command-line entry point: `args` excludes the program name and is UTF-8 text (paths
// included, on every platform). Returns the process exit code (0 success, 1 the web types
// could not be generated or an output could not be written, 2 bad usage); the report goes
// to `out`, warnings and errors to `err`.
int Run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

// True if the base-clause inherits a base whose simple (unqualified) name matches baseName —
// InheritsBase("public ECS::ComponentBase", "ComponentBase") == true. Strips access / virtual
// specifiers, namespace qualifiers, and any template arguments.
bool InheritsBase(std::string_view bases, std::string_view baseName);

// Names of structs in `cleanedSource` whose base-clause inherits baseName. Used to warn when
// a macro-free component is authored in a non-header file the scanner won't register.
std::vector<std::string> InheritingStructNames(std::string_view cleanedSource, std::string_view baseName);

} // namespace GameEngine::ComponentScanner
