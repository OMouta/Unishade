import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { dataDir } from "./context.ts";

// Private threads the bot opened for someone's problem, by thread ID. Asker is who it was opened for, answers how many
// the bot gave in it, and human whether someone asked for the team, after which the bot answers only when mentioned.
export type SupportThread = { asker: string; answers: number; human: boolean };

const file = path.join(dataDir, "threads.json");
const threads: Record<string, SupportThread> = existsSync(file) ? JSON.parse(readFileSync(file, "utf8")) : {};

function save() {
  mkdirSync(dataDir, { recursive: true });
  writeFileSync(file, JSON.stringify(threads, null, 2));
}

export const supportThread = (id: string): SupportThread | undefined => threads[id];

// The ID of the thread opened for someone, if it's still looked after.
export const threadOf = (asker: string) => Object.keys(threads).find((id) => threads[id].asker === asker);

export function addThread(id: string, asker: string) {
  threads[id] = { asker, answers: 0, human: false };
  save();
}

export function countAnswer(id: string) {
  if (!threads[id]) return;
  threads[id].answers++;
  save();
}

export function callHuman(id: string) {
  threads[id].human = true;
  save();
}

// Solved or deleted, the bot stops looking after it.
export function forgetThread(id: string) {
  if (!threads[id]) return;
  delete threads[id];
  save();
}
