import type { AttachmentPayload, PollData } from "discord.js";
import type { Tags } from "./tags.ts";

// What some tags make of the answer: [shader] attaches its code block as a .fx file, [poll] turns its poll block into a
// Discord poll, and [music] keeps only the Spotify and SoundCloud links that lead somewhere.
export type Extras = { text: string; files: AttachmentPayload[]; poll?: PollData };

// Code blocks, and while the answer comes in, the one that isn't closed yet.
const codeBlocks = /```[\s\S]*?(?:```|$)/g;
// Spotify and SoundCloud links, raw or inside [text](link).
const musicLinks = /https:\/\/(?:open\.spotify\.com|(?:on\.)?soundcloud\.com)\/[^\s)>\]]+/g;

// The shader and the poll aren't shown, so they're left out while the answer comes in.
export const shownWhileAnswering = (text: string, tags: Tags) => (tags.shader || tags.poll ? text.replace(codeBlocks, "") : text);

export async function extras(answer: string, tags: Tags): Promise<Extras> {
  const result: Extras = { text: answer, files: [] };
  for (const [block, language, code] of answer.matchAll(/```(\w*)[^\n]*\n([\s\S]*?)```/g)) {
    // Asked for both, the poll is the block marked as one.
    if (tags.poll && !result.poll && (language === "poll" || !tags.shader)) {
      result.poll = pollOf(code);
      result.text = result.text.replace(block, "");
    } else if (tags.shader && !result.files.length) {
      const name = code.match(/technique\s+(\w+)/)?.[1] ?? "Shader";
      result.files.push({ attachment: Buffer.from(code), name: `${name}.fx` });
      result.text = result.text.replace(block, "");
    }
  }
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
