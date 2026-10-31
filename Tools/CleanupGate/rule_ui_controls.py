"""Class 05, UI controls: imperative assembly instead of XML or typed declarations and the control's own CSS."""
import os
import re

from gate_common import Site

CLASS_ID = "05_ui_controls"
PATTERNS = ["files_assembling_ui", "imperative_no_asset", "add_child_calls", "control_creations"]

kAddChild = re.compile(r"(?:->|\.)(?:AddChild|InsertChild)\s*\(")
kCreate = re.compile(r"std::make_unique\s*<\s*(?:UI::)?(\w+)\s*>")
kLayoutReference = re.compile(r"\"([\w/\-.]+\.uxml)\"|\"([\w/\-.]+\.xml)\"")
kStyleReference = re.compile(r"\"([\w/\-.]+\.css)\"")
kLayoutApi = re.compile(r"InstantiateLayout\w*\s*\(|LoadLayoutFrom\w*\s*\(|LoadStylesheet\w*\s*\(|AddStylesheet\s*\(|Stylesheet\w*Handle|UILayoutAsset|UIStyleAsset")
kHub = re.compile(r"^(EditorApplication|EditorContext|SettingsPanel|InspectorPanel|HierarchyPanel|SceneViewPanel|UIManager|UIElement)\b")
kBaseControls = {"UIElement", "Label", "Button", "TextField", "Toggle", "Dropdown", "Slider", "ScrollView", "Foldout", "Checkbox"}
kAssetPiece = "a control class with XML or UI::Element<T> declarations and its own CSS loaded by asset path"


class Rule:
    def __init__(self, files):
        self.m_ControlTypes = {os.path.splitext(os.path.basename(path))[0] for path in files if "/UI/Controls/" in path and path.endswith(".h")}

    def scan_file(self, source):
        path = source.path
        if not path.endswith((".cpp", ".mm")):
            return []
        sites = []
        cites_asset = False
        for line_number, code, raw in source.code:
            for _ in kAddChild.finditer(code):
                sites.append(Site(path, line_number, "add_child_calls", kAssetPiece))
            for match in kCreate.finditer(code):
                if match.group(1) in self.m_ControlTypes or match.group(1) in kBaseControls:
                    sites.append(Site(path, line_number, "control_creations", kAssetPiece))
            if kLayoutReference.search(raw) or kStyleReference.search(raw) or kLayoutApi.search(code):
                cites_asset = True
        if not sites:
            return []
        sites.append(Site(path, 1, "files_assembling_ui", kAssetPiece))
        stem = re.sub(r"\.(cpp|mm)$", "", os.path.basename(path))
        if not cites_asset and not kHub.match(stem):
            sites.append(Site(path, 1, "imperative_no_asset", kAssetPiece))
        return sites

    def finish(self):
        return []
