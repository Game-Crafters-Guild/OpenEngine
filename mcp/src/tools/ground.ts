import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

/**
 * A world point list: `[[x, z], ...]` on a planar terrain, `[[x, y, z], ...]` on a planet.
 *
 * A factory rather than a shared const, for the reason vec3Tuple() documents:
 * one zod instance reused across two properties emits a `$ref` for the second
 * instead of inlining it.
 */
function groundPointList() {
  return z.preprocess(
    (val) => {
      if (typeof val === "string") {
        try { return JSON.parse(val); } catch { return val; }
      }
      return val;
    },
    z.array(z.array(z.coerce.number()).min(2).max(3)),
  );
}

export const tools: ToolDef[] = [
  proxyTool({
    name: "sample_ground_height",
    category: "scene",
    description:
      "Sample the COMPOSED ground height at world XZ points — the CPU heightfield after the modifier stack (flattens, corridors, stamps, noise), which is what the renderer draws and what conforming placement lands on. This is not the base heightmap: a scene-authored corridor exists only in the composed result, so an offline model that reads the shipped heightmap cannot see it. Terrain only — never reports a mesh, prop or bridge deck as ground. Each sample reports `inside` (within the terrain footprint at all) separately from `sampled` (a height came back); inside-but-unsampled means the terrain has no height there yet (its height store is still cooking: surface.kind `pagedNoSourceYet`, or its height pages have not had their first update), and is NOT ground level. A sampled point also reports `level` (0 the finest) and `finest`. On a planar terrain the call loads the finest page under a point before it answers (at most 64 pages a call), so a height to place a camera or an entity on is the finest one; `finest` false means the call used its 64 loads (ask again for those points) or the store has no finest page there (asking again gives the same answer until the store holds the page), and the reply's `note` gives each count. Do not place on a sample with `finest` false. On a planet (surface.kind `planet`) the call loads no page: `finest` false means the finest page is not loaded there yet. On a planet give [x, y, z] points: each sample reports `height` (meters above surface.referenceRadius on the line from surface.center through the point) and `surface` (that ground point in world space).",
    schema: {
      points: groundPointList().describe("World points as [[x, z], ...], or [[x, y, z], ...] on a planet (max 8192)"),
    },
  }),

  proxyTool({
    name: "get_placement_ground_gap",
    category: "scene",
    description:
      "Measure the SIGNED ground-to-underside gap for every piece a spline route placed: POSITIVE = floating above the composed ground, NEGATIVE = buried in it. Probes the piece's OWN underside — the oriented box's lower face, at its centre and four corners, so every probe is a point of the real underside over ground the piece actually covers — and reports per-probe x/z/undersideY/groundY/gap, per-piece minGap/maxGap/centerGap, and a route summary (worstFloating, worstBuried, each with the entity it came from). A tilted piece bedded at its middle reads a negative minGap and a positive maxGap at once; that spread is the reading. `pieces[].status` says why a piece has no numbers (noGroundSampled, undersideNearVertical, degenerateFootprint) instead of reporting a zero. This is the axis a conforming placement fails on — station tilt is anti-correlated with it, because flattening the ground under a slab lowers tilt and opens the gap at the same time. `pieces[].name` is a display label that renumbers with spacing; match on entityId.",
    schema: {
      entityId: z.coerce.number().describe("The spline route entity whose placed pieces to measure"),
    },
  }),
];
