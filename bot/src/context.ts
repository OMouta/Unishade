import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const site = "https://unishade.me";

// A Discord message an admin added to what the bot answers from.
export type SavedMessage = {
  id: string;
  url: string;
  // Where and when it was posted, such as "#announcements, 2026-10-01".
  posted: string;
  text: string;
};

const here = path.dirname(fileURLToPath(import.meta.url));
const docsDir = process.env.DOCS_DIR ?? path.join(here, "../../website/src/content/docs");
// Railway sets the second one when the service has a volume.
export const dataDir = process.env.DATA_DIR ?? process.env.RAILWAY_VOLUME_MOUNT_PATH ?? path.join(here, "../data");
const file = path.join(dataDir, "context.json");

// The docs pages as the site shows them: links point at the site, and images are left out.
const docs = readdirSync(docsDir)
  .filter((name) => name.endsWith(".md"))
  .sort()
  .map((name) => {
    const text = readFileSync(path.join(docsDir, name), "utf8");
    const [, frontmatter = "", body = text] = text.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n([\s\S]*)$/) ?? [];
    const title = frontmatter.match(/^title:\s*(.+)$/m)?.[1] ?? name;
    const content = body
      .replace(/!\[[^\]]*\]\([^)]*\)\s*/g, "")
      .replaceAll("](/", `](${site}/`)
      // The site fills these in from website/src/links.ts.
      .replace(/\[([^\]]+)\]\(links:\w+\)/g, "$1")
      .trim();
    return `<page title="${title}" url="${site}/docs/${name.replace(/\.md$/, "")}/">\n${content}\n</page>`;
  });

const saved: SavedMessage[] = existsSync(file) ? JSON.parse(readFileSync(file, "utf8")) : [];

function save() {
  mkdirSync(dataDir, { recursive: true });
  writeFileSync(file, JSON.stringify(saved, null, 2));
}

export function savedMessages(): readonly SavedMessage[] {
  return saved;
}

// Returns whether the message was in the context already, in which case its text is replaced.
export function saveMessage(message: SavedMessage): boolean {
  const index = saved.findIndex((existing) => existing.id === message.id);
  if (index < 0) saved.push(message);
  else saved[index] = message;
  save();
  return index >= 0;
}

export function removeMessage(id: string): boolean {
  const index = saved.findIndex((message) => message.id === id);
  if (index < 0) return false;
  saved.splice(index, 1);
  save();
  return true;
}

// Everything the bot answers from.
export function renderContext(): string {
  const messages = saved.map(({ posted, text }) => `<message posted="${posted}">\n${text}\n</message>`);
  return [...docs, ...messages].join("\n\n");
}
