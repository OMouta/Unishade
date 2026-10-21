import type { AttachmentPayload, PollData } from "discord.js";
import type { Tags } from "./tags.ts";

// What some tags make of the answer: [shader] attaches its code block as a .fx file, [bug] turns its report into a link
// to a filled-in GitHub issue, [poll] turns its poll block into a Discord poll, and [music] keeps only the Spotify and
// SoundCloud links that lead somewhere. Without a tag, an answer can offer a private thread, which becomes a button.
export type Extras = { text: string; files: AttachmentPayload[]; poll?: PollData; offersThread: boolean };

// Code blocks, and while the answer comes in, the one that isn't closed yet.
const codeBlocks = /```[\s\S]*?(?:```|$)/g;
// Spotify and SoundCloud links, raw or inside [text](link).
const musicLinks = /https:\/\/(?:open\.spotify\.com|(?:on\.)?soundcloud\.com)\/[^\s)>\]]+/g;
const threadOffer = /\[private thread\]/gi;

// The shader, the bug report, the poll and the thread offer aren't shown, so they're left out while the answer comes in.
export const shownWhileAnswering = (text: string, tags: Tags) =>
  (tags.shader || tags.bug || tags.poll ? text.replace(codeBlocks, "") : text).replace(threadOffer, "");

export async function extras(answer: string, tags: Tags): Promise<Extras> {
  const text = answer.replace(threadOffer, "");
  const result: Extras = { text, files: [], offersThread: text !== answer };
  let report: { link?: string } | undefined;
  for (const [block, language, code] of answer.matchAll(/```(\w*)[^\n]*\n([\s\S]*?)```/g)) {
    // Asked for more than one, the bug report and the poll are the blocks marked as them.
    if (tags.bug && !report?.link && (language === "bug" || (!tags.shader && !tags.poll))) {
      report = { link: reportLink(code) };
      result.text = result.text.replace(block, "");
    } else if (tags.poll && !result.poll && (language === "poll" || !tags.shader)) {
      result.poll = pollOf(code);
      result.text = result.text.replace(block, "");
    } else if (tags.shader && !result.files.length) {
      const name = code.match(/technique\s+(\w+)/)?.[1] ?? "Shader";
      result.files.push({ attachment: Buffer.from(code), name: `${name}.fx` });
      result.text = result.text.replace(block, "");
    }
  }
  // The link goes at the end, apart from whatever the model wrote around the block. Without a block, the model asked for
  // what it needs, such as the log, and that stands.
  if (report) result.text += report.link ? `\n\n[Open the bug report on GitHub](${report.link})` : "\n\nThe bug report didn't come out right. Try asking again.";
  if (tags.poll && !result.poll) result.text += "\n\nThe poll didn't come out right. Try asking again.";
  if (tags.music) result.text = await withoutDeadLinks(result.text);
  result.text = result.text.replace(/\n{3,}/g, "\n\n").trim();
  return result;
}

// The question on the first line and an answer on each after, within Discord's limits. A poll is open for a day.
function pollOf(block: string): PollData | undefined {
  const lines = block.split("\n").map((line) => line.replace(/^\s*(?:[-*•]|\d+[.)])\s+/, "").trim());
  const [question, ...answers] = lines.filter(Boolean);
  if (!question || answers.length < 2) return undefined;
  return {
    question: { text: question.slice(0, 300) },
    answers: answers.slice(0, 10).map((text) => ({ text: text.slice(0, 55) })),
    duration: 24,
    allowMultiselect: false,
  };
}

// GitHub fills in the bug report form from the link, each text field by its ID in .github/ISSUE_TEMPLATE/bug_report.yml.
const reportFields = ["title", "what", "game", "version", "reshade", "windows", "gpu", "console"] as const;
const newIssue = "https://github.com/OMouta/Unishade/issues/new?template=bug_report.yml";
// Discord takes 2000 characters a message, and the link shares them with the line the model writes, the question /ask
// quotes and the line under the reply.
const maxLinkLength = 1400;

// The link to the report in the model's JSON, or undefined when it isn't one.
function reportLink(json: string): string | undefined {
  let report: unknown;
  try {
    report = JSON.parse(json);
  } catch {
    return undefined;
  }
  if (typeof report !== "object" || report === null) return undefined;
  const fields: Record<string, string> = {};
  for (const field of reportFields) {
    const value: unknown = (report as Record<string, unknown>)[field];
    if (typeof value === "string" && value.trim()) fields[field] = value.trim();
  }
  if (!fields.title) return undefined;
  const link = () => `${newIssue}&${new URLSearchParams(fields)}`;
  // Too long, the log loses lines from its start, and then the description is cut short.
  while (link().length > maxLinkLength && fields.console) fields.console = fields.console.split("\n").slice(1).join("\n");
  while (link().length > maxLinkLength && (fields.what ?? "").length > 100) fields.what = `${fields.what.slice(0, -50).trimEnd()}…`;
  return link().length <= maxLinkLength ? link() : undefined;
}

// The model can write a link that looks right and leads nowhere. Spotify's and SoundCloud's oEmbed endpoints answer 404
// for those and need no key. A link that can't be checked goes too.
async function withoutDeadLinks(text: string): Promise<string> {
  const checked = await Promise.all(
    [...new Set(text.match(musicLinks))].map(async (link) => {
      const oembed = link.includes("spotify.com")
        ? `https://open.spotify.com/oembed?url=${encodeURIComponent(link)}`
        : `https://soundcloud.com/oembed?format=json&url=${encodeURIComponent(link)}`;
      const response = await fetch(oembed, { signal: AbortSignal.timeout(10_000) }).catch(() => undefined);
      return { link, alive: !!response?.ok };
    }),
  );
  const kept = checked.reduce((kept, { link, alive }) => (alive ? kept : kept.replaceAll(link, "")), text);
  // A [text](link) whose link went keeps its text.
  return kept.replace(/\[([^\]]*)\]\(\)/g, "$1");
}
