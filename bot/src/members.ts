import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { dataDir } from "./context.ts";

// User IDs an admin set apart: "excluded" ones the bot ignores, "unlimited" ones it treats like the team.
type Members = { excluded: string[]; unlimited: string[] };
export type Flag = keyof Members;

const file = path.join(dataDir, "members.json");
const members: Members = existsSync(file) ? JSON.parse(readFileSync(file, "utf8")) : { excluded: [], unlimited: [] };

export function hasFlag(flag: Flag, userId: string): boolean {
  return members[flag].includes(userId);
}

export function setFlag(flag: Flag, userId: string, on: boolean) {
  const list = members[flag];
  const index = list.indexOf(userId);
  if (on === index >= 0) return;
  if (on) list.push(userId);
  else list.splice(index, 1);
  mkdirSync(dataDir, { recursive: true });
  writeFileSync(file, JSON.stringify(members, null, 2));
}
