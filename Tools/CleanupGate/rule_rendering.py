"""Class 08, rendering outside the graph: command lists, resources, barriers and submits created or placed from engine code instead of
through the render graph and the managers that own GPU resources.

The four backends and the render graph are the owners and are out of scope; the core managers and the persistent-resource owners
are allowlisted by file.
"""
import re

from gate_common import Site, strip_strings

CLASS_ID = "08_rendering"

kBackend = re.compile(r"Engine/Modules/Rendering/Source/(Vulkan|Metal|DirectX12|WebGPU)/")
kGraph = re.compile(r"Rendering/(Include/Rendering|Source)/Core/RenderGraph/|RenderGraph\w*\.(cpp|h)$|RGFrame|RGGraph|RGRealize")
kPatterns = [
    ("raw_vulkan_call", re.compile(r"\bvk(?:Cmd|Create|Destroy|Allocate|Free|Queue|Begin|End|Map|Unmap|Wait|Reset|Get|Bind|Update|Flush|Invalidate)\w*\s*\("), "the backend behind IDevice and CommandList; no Vulkan call outside Rendering/Source/Vulkan"),
    ("raw_vma_call", re.compile(r"\bvma[A-Z]\w*\s*\("), "the backend's allocator; no VMA call outside Rendering/Source/Vulkan"),
    ("raw_d3d12", re.compile(r"\bID3D12\w+\b|\bD3D12_[A-Z_]+\b"), "the backend behind IDevice; no D3D12 outside Rendering/Source/DirectX12"),
    ("raw_metal", re.compile(r"\bMTL[A-Z]\w+\b|\bid<MTL"), "the backend behind IDevice; no Metal outside Rendering/Source/Metal"),
    ("raw_wgpu", re.compile(r"\bwgpu[A-Z]\w*\s*\(|\bWGPU[A-Z]\w+\b"), "the backend behind IDevice; no WebGPU outside Rendering/Source/WebGPU"),
    ("create_command_list", re.compile(r"(?:->|\.)CreateCommandList\s*\("), "a render graph pass (RGFrame) records into the graph's command list"),
    ("create_texture", re.compile(r"(?:->|\.)CreateTexture(?:View)?\s*\("), "a graph texture (RGFrame Create or Import) or the owning manager (TextureManager)"),
    ("create_buffer", re.compile(r"(?<![Ff]rame)(?:->|\.)CreateBuffer\s*\("), "a graph buffer (RGFrame Create or Import) or the owning manager (BufferManager, RGUploadRing)"),
    ("destroy_resource", re.compile(r"(?:->|\.)Destroy(?:Texture|Buffer|TextureView)\s*\("), "the graph's transient pool or the owning manager releases it"),
    ("manual_barrier", re.compile(r"(?:->|\.)Barrier(?:Batch)?\s*\(|\bResourceBarrier\s*\{|\bResourceBarrier\s+\w+\s*[={;]"), "the graph owns the resource state: declare the Read or Write on the pass"),
    ("texture_upload_submit", re.compile(r"(?:->|\.)SubmitTextureUploads\s*\("), "TextureService or TextureManager submits uploads"),
    ("queue_submit", re.compile(r"(?:->|\.)(?:Submit|SubmitCommandList|ExecuteCommandLists)\s*\(\s*\w*(?:cmd|command|list)"), "the graph submits; a pass never submits"),
]
PATTERNS = [name for name, _, _ in kPatterns]


class Rule:
    def __init__(self, files):
        pass

    def scan_file(self, source):
        if kBackend.search(source.path) or kGraph.search(source.path):
            return []
        sites = []
        for line_number, code, _ in source.code:
            if not code.strip() or code.lstrip().startswith("#"):
                continue
            code = strip_strings(code)
            for name, pattern, piece in kPatterns:
                if pattern.search(code):
                    sites.append(Site(source.path, line_number, name, piece))
        return sites

    def finish(self):
        return []
