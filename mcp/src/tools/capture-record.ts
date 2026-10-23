// The record line the capture tools return in place of a listing or inline image
// data: the file, its size and hash, the camera pose and the validation counts.
// The image itself stays on disk, where the agent opens it when it needs to look.

import { createHash } from "crypto";
import * as fs from "fs";

export interface CapturePose {
  position: number[];
  yawDeg: number;
  pitchDeg: number;
}

export interface CaptureValidation {
  errors: number;
  warnings: number;
}

export interface CaptureRecord {
  filePath: string;
  bytes: number | null;
  sha256: string | null;
  /** The scene view camera when the frame was read back; null when the editor did not report one. */
  pose: CapturePose | null;
  /** Graphics API validation messages counted so far in the session; null when validation is off or not reported. */
  validation: CaptureValidation | null;
}

function poseOf(reply: any): CapturePose | null {
  const pose = reply?.pose;
  if (!pose || !Array.isArray(pose.position)) return null;
  return { position: pose.position, yawDeg: Number(pose.yawDeg), pitchDeg: Number(pose.pitchDeg) };
}

function validationOf(reply: any): CaptureValidation | null {
  const validation = reply?.validation;
  if (!validation || validation.enabled === false) return null;
  return { errors: Number(validation.errors ?? 0), warnings: Number(validation.warnings ?? 0) };
}

/** The record of a capture whose bytes came back in the reply (a screenshot). */
export function recordFromBytes(reply: any, bytes: Buffer): CaptureRecord {
  return {
    filePath: String(reply.filePath),
    bytes: bytes.length,
    sha256: createHash("sha256").update(bytes).digest("hex"),
    pose: poseOf(reply),
    validation: validationOf(reply),
  };
}

/**
 * The record of a capture the editor wrote to `filePath` (a RenderDoc capture).
 * Size and hash are null when the file is not on this machine (an editor on
 * another host) or not there yet.
 */
export async function recordFromFile(reply: any, filePath: string): Promise<CaptureRecord> {
  let bytes: number | null = null;
  let sha256: string | null = null;
  try {
    bytes = fs.statSync(filePath).size;
    const hash = createHash("sha256");
    for await (const chunk of fs.createReadStream(filePath)) hash.update(chunk as Buffer);
    sha256 = hash.digest("hex");
  } catch { /* not on this machine */ }
  return { filePath, bytes, sha256, pose: poseOf(reply), validation: validationOf(reply) };
}

/** One line: the file, its size and hash, the camera pose and the validation counts. */
export function formatCaptureRecord(label: string, record: CaptureRecord): string {
  const size = record.bytes === null ? "size unknown (the file is not on this machine)" : `${record.bytes} bytes`;
  const sha = record.sha256 ? `sha256 ${record.sha256}` : "sha256 unknown";
  const pose = record.pose
    ? `scene view pose (${record.pose.position.map((v) => Number(v).toFixed(2)).join(", ")}) yaw ${record.pose.yawDeg.toFixed(1)} pitch ${record.pose.pitchDeg.toFixed(1)}`
    : "pose not reported";
  const validation = record.validation
    ? `validation errors ${record.validation.errors}, warnings ${record.validation.warnings}`
    : "validation not reported";
  return `${label} ${record.filePath}; ${size}; ${sha}; ${pose}; ${validation}`;
}
