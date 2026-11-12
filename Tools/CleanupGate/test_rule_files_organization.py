"""Tests 04_files_organization: the loose_feature_file pattern over the folders in Fixtures/FeatureFolders/, and the function heads
that one_free_function_file counts over the fixtures in Fixtures/."""
import os
import sys
import unittest

sys.dont_write_bytecode = True
kGateFolder = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, kGateFolder)

import gate_common  # noqa: E402
import rule_files_organization  # noqa: E402

kFixtureRoot = os.path.join(kGateFolder, "Fixtures", "FeatureFolders")


def loose_files(folder):
    """Returns the files under one fixture folder that loose_feature_file counts, relative to the fixture root."""
    paths = []
    for directory, _, names in os.walk(os.path.join(kFixtureRoot, folder)):
        paths.extend(os.path.relpath(os.path.join(directory, name), kFixtureRoot).replace(os.sep, "/") for name in names)
    return loose_paths(paths)


def loose_paths(paths):
    """Returns the paths of a list that loose_feature_file counts."""
    return sorted(site.file for site in rule_files_organization.loose_feature_sites(sorted(paths)))


class FeatureFolderRuleTests(unittest.TestCase):
    def test_three_files_of_one_feature_in_a_flat_folder_count_three(self):
        self.assertEqual(loose_files("Flat"), ["Flat/ProjectPickerCard.h", "Flat/ProjectPickerImportView.h", "Flat/ProjectPickerModal.h"])

    def test_the_same_files_in_the_feature_folder_count_zero(self):
        self.assertEqual(loose_files("Foldered"), [])

    def test_files_that_share_one_word_of_five_letters_count_zero(self):
        self.assertEqual(loose_files("OneShortWord"), [])

    def test_a_straggler_beside_its_feature_folder_counts_one(self):
        self.assertEqual(loose_files("Straggler"), ["Straggler/ProjectPickerRecentList.h"])

    def test_trailing_owner_words_leave_the_feature(self):
        self.assertEqual(loose_paths(["P/SceneViewPanel.cpp", "P/SceneViewPanelMenus.cpp", "P/SceneViewQuad.cpp", "P/Other.cpp"]),
                         ["P/SceneViewPanel.cpp", "P/SceneViewPanelMenus.cpp", "P/SceneViewQuad.cpp"])

    def test_owner_words_alone_are_no_feature(self):
        self.assertEqual(loose_paths(["P/InspectorPanel.cpp", "P/InspectorPanelTabs.cpp", "P/InspectorPanelSearch.cpp", "P/Other.cpp"]), [])

    def test_two_files_of_a_feature_count_zero(self):
        self.assertEqual(loose_paths(["P/ProjectPickerCard.h", "P/ProjectPickerModal.h", "P/Other.h"]), [])

    def test_a_folder_named_for_the_feature_in_another_case_or_plural_counts_zero(self):
        self.assertEqual(loose_paths([f"P/{folder}/{feature}{kind}.h" for folder, feature in (("MARKUPS", "Markup"), ("Entities", "Entity"))
                                      for kind in ("Card", "Row", "List")] + ["P/MARKUPS/Toolbar.h", "P/Entities/Toolbar.h"]), [])

    def test_a_feature_that_holds_every_file_of_its_folder_counts_zero(self):
        self.assertEqual(loose_paths(["Network/SplineNetworkGraph.cpp", "Network/SplineNetworkJunction.cpp", "Network/SplineNetworkSolver.cpp"]), [])

    def test_the_longest_feature_names_the_folder(self):
        sites = rule_files_organization.loose_feature_sites(["P/Other.h", "P/SplineFenceCard.h", "P/SplineFenceList.h", "P/SplineFenceRow.h",
                                                             "P/SplineRecipe.h"])
        self.assertEqual([site.piece for site in sites], ["the folder P/SplineFence/"] * 3 + ["the folder P/Spline/"])

    def test_a_prefixed_sub_feature_inside_its_parent_folder_counts_zero(self):
        self.assertEqual(loose_paths(["Sky/SkySun/SkySunRow.h", "Sky/SkySun/SkySunPathCaptions.h", "Sky/SkySun/SkySunPathRows.h",
                                      "Sky/SkySun/SkySunPathSwitch.h"]), [])

    def test_a_prefixed_sub_feature_in_a_flat_folder_counts(self):
        self.assertEqual(loose_paths(["Sky/SkyDayCurveRow.h", "Sky/SkySunPathCaptions.h", "Sky/SkySunPathRows.h", "Sky/SkySunPathSwitch.h"]),
                         ["Sky/SkySunPathCaptions.h", "Sky/SkySunPathRows.h", "Sky/SkySunPathSwitch.h"])

    def test_a_feature_that_does_not_start_with_its_folder_name_counts(self):
        self.assertEqual(loose_paths(["Humanoid/HumanoidRig.h", "Humanoid/KHRHumanoidImporter.h", "Humanoid/KHRHumanoidShared.cpp",
                                      "Humanoid/KHRHumanoidShared.h"]),
                         ["Humanoid/KHRHumanoidImporter.h", "Humanoid/KHRHumanoidShared.cpp", "Humanoid/KHRHumanoidShared.h"])

    def test_a_feature_inside_a_plural_or_scope_root_folder_counts(self):
        self.assertEqual(len(loose_paths(["Assets/AssetBrowserCard.h", "Assets/AssetBrowserGrid.h", "Assets/AssetBrowserRow.h",
                                          "Assets/TextureImporter.h"])), 3)
        self.assertEqual(len(loose_paths(["M/Source/Other.cpp", "M/Source/SourceControlA.cpp", "M/Source/SourceControlB.cpp",
                                          "M/Source/SourceControlC.cpp"])), 3)


class FunctionHeadTests(unittest.TestCase):
    def test_a_free_function_whose_signature_wraps_counts_as_a_one_function_file(self):
        source = gate_common.load_source(os.path.join(kGateFolder, "Fixtures"), "wrapped_free_function.h")
        sites = [site for site in rule_files_organization.Rule([]).scan_file(source) if site.pattern == "one_free_function_file"]
        self.assertEqual([(site.line, site.piece) for site in sites], [(7, "the utility header of the module that owns SpawnPrefab, beside its kin")])


if __name__ == "__main__":
    unittest.main()
