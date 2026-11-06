"""Tests free-helper detection at constructor initializer lists and genuine function definitions."""
import os
import sys
import unittest

sys.dont_write_bytecode = True
kGateFolder = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, kGateFolder)

import gate_common  # noqa: E402
import rule_files_organization  # noqa: E402


class FunctionHeadRuleTests(unittest.TestCase):
    def duplicate_sites(self, first, second):
        paths = ["Engine/Modules/Example/Source/FirstPanel.cpp", "Engine/Modules/Example/Source/SecondPanel.cpp"]
        rule = rule_files_organization.Rule(paths)
        for path, text in zip(paths, (first, second)):
            source = gate_common.Source(path, "Example", list(gate_common.code_lines(text.splitlines())))
            rule.scan_file(source)
        return [site for site in rule.finish() if site.pattern == "duplicate_helper_bodies"]

    def test_base_initializers_are_not_duplicate_free_helpers(self):
        first = 'FirstPanel::FirstPanel()\n    : DockPanel("First")\n{\n    BuildUI();\n}\n'
        second = 'SecondPanel::SecondPanel()\n    : DockPanel("Second")\n{\n    BuildUI();\n}\n'
        self.assertEqual(self.duplicate_sites(first, second), [])

    def test_initializer_continuations_are_not_free_helpers(self):
        initializers = [
            ': Base(1),\n      Value(2)',
            ': Base(1)\n    , Value(2)',
            ': Base{MakeValue()},\n      Value(2)',
            'try : Base(1),\n      Value(2)',
            'try :Base(1)',
        ]
        for initializer in initializers:
            with self.subTest(initializer=initializer):
                text = f'Panel::Panel()\n    {initializer}\n{{\n    BuildUI();\n}}\n'
                self.assertEqual(self.duplicate_sites(text, text), [])

    def test_qualified_constructor_initializers_are_not_helpers(self):
        signatures = [
            'Panel::Panel()',
            'Editor::Panel::Panel()',
            'inline Panel::Panel()',
            'Panel<Value>::Panel()',
            'Outer<Value>::Panel::Panel()',
        ]
        for signature in signatures:
            with self.subTest(signature=signature):
                text = f'{signature}\n    : Base(1)\n{{\n    BuildUI();\n}}\n'
                self.assertEqual(self.duplicate_sites(text, text), [])

    def test_a_real_helper_beside_a_constructor_is_still_reported(self):
        text = 'int ComputeValue()\n{\n    return 1;\n}\nPanel::Panel()\n    : Base(1)\n{\n    BuildUI();\n}\n'
        sites = self.duplicate_sites(text, text)
        self.assertEqual(len(sites), 1)
        self.assertIn('one ComputeValue ', sites[0].piece)

    def test_member_functions_and_destructors_are_not_free_helpers(self):
        text = 'void Panel::BuildUI()\n{\n    BuildLayout();\n}\nPanel::~Panel()\n{\n    BuildLayout();\n}\n'
        self.assertEqual(self.duplicate_sites(text, text), [])

    def test_real_free_function_bodies_are_still_reported(self):
        signatures = [
            'void BuildShared()',
            'Widget DockPanel()',
            'static int ComputeShared(int value)',
            'inline std::string FormatShared()',
            '[[nodiscard]] constexpr auto ComputeShared() noexcept -> int',
            '::std::string FormatShared()',
            'const ::std::string& GetShared()',
        ]
        for signature in signatures:
            with self.subTest(signature=signature):
                text = f'{signature}\n{{\n    return {{}};\n}}\n'
                sites = self.duplicate_sites(text, text)
                self.assertEqual(len(sites), 1)
                self.assertEqual(sites[0].file, 'Engine/Modules/Example/Source/SecondPanel.cpp')

    def test_different_free_function_bodies_are_not_duplicates(self):
        first = 'int ComputeValue()\n{\n    return 1;\n}\n'
        second = 'int ComputeValue()\n{\n    return 2;\n}\n'
        self.assertEqual(self.duplicate_sites(first, second), [])


if __name__ == "__main__":
    unittest.main()
