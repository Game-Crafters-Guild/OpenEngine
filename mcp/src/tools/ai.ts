import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

export const tools: ToolDef[] = [
  proxyTool({
    name: "get_ai_trace",
    category: "scene",
    description:
      "Read one AI agent's last decisions from its AiTrace component (the agent records only while it has AiTrace; add the component to trace it). Returns entityId, recorded (decisions since AiTrace was added) and records, oldest first, each with tick (the AI tick of the decision), enteredTick (the AI tick the chosen option started), program (the AiRegistry program id), previous and chosen (option indices in the program's option set; null when no option was active or none scored above 0), score (the chosen option's score), outcome (none, running, succeeded or failed: what the chosen task's tick returned), commandCount (orders the tasks issued in the decision) and commands (the first two: type, targetEntityId, argument). A state-tree agent records in its AiPathTrace component instead: the reply then holds pathRecorded and pathRecords, each with tick, program (the AiRegistry id, with bit 0x8000 set), result (running, succeeded or failed: the tree's result after the step) and states (the active state indices from the root down). Refused with the fix when entityId is missing or the entity has neither AiTrace nor AiPathTrace.",
    schema: {
      entityId: z.coerce.number().int().describe("The agent entity; it must have an AiTrace or AiPathTrace component"),
      ticks: z.coerce.number().int().min(1).max(64).optional().describe("The newest decisions to return, 1 to 64 (default 64)"),
    },
  }),
];
