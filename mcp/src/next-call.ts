import { ipcConfig, kDefaultIpcHost, kDefaultIpcPort } from "./config.js";
import { camelToKebab } from "./cli/args.js";

// The call a reply tells the agent to make next, in the two forms an agent pastes:
// the MCP tool call and the CLI command line. A tool reports the arguments
// (ToolOutput.next); the MCP server and the CLI format them here.

const kBareCliValue = /^[\w.,:/+-]+$/;

/** `get_log since=57 pattern="shader|pipeline"`: the MCP reply's `next` field. */
export function mcpNextCall(tool: string, args: Record<string, unknown>): string {
  const parts = Object.entries(args).map(([key, value]) => `${key}=${JSON.stringify(value)}`);
  return [tool, ...parts].join(" ");
}

// Single quotes are literal in bash and PowerShell. A value that holds one is
// double-quoted with bash escapes, for the Bash tool.
function quoteCliValue(value: unknown): string {
  const text = String(value);
  if (kBareCliValue.test(text)) return text;
  if (!text.includes("'")) return `'${text}'`;
  return `"${text.replace(/(["\\$`])/g, "\\$1")}"`;
}

/**
 * `node mcp/ge.mjs get_log --since 57 --pattern 'shader|pipeline' --port 9850`:
 * the CLI's last line. The port and host are named whenever they are not the
 * defaults, so a pasted line reaches the editor that answered, whatever the
 * environment of the next shell holds.
 */
export function cliNextCall(tool: string, args: Record<string, unknown>): string {
  const parts = Object.entries(args).map(([key, value]) => `--${camelToKebab(key)} ${quoteCliValue(value)}`);
  const cfg = ipcConfig();
  if (cfg.port !== kDefaultIpcPort) parts.push(`--port ${cfg.port}`);
  if (cfg.host !== kDefaultIpcHost) parts.push(`--host ${quoteCliValue(cfg.host)}`);
  return ["node mcp/ge.mjs", tool, ...parts].join(" ");
}
