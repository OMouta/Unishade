// Tags someone puts in a question to change how the bot answers it. [web] lets the model search the web, [rate] has it
// rate a screenshot, [tldr] has it sum up the conversation, and [think] or [think medium] sets how hard it reasons.
export type Tags = { web: boolean; rate: boolean; tldr: boolean; think?: Effort };

const switches = ["web", "rate", "tldr"] as const;
// Reasoning effort as OpenRouter takes it, but for "none", which openai/gpt-oss-20b rejects.
export const efforts = ["minimal", "low", "medium", "high", "xhigh", "max"] as const;
type Effort = (typeof efforts)[number];
export const effortOf = (level: string) => efforts.find((each) => each === level);

// A level that doesn't exist is an error, so a typo doesn't quietly get the default. [thinking] or [website] aren't
// tags at all.
export function readTags(text: string): Tags | { error: string } {
  const tags: Tags = { web: false, rate: false, tldr: false };
  for (const [, name, level] of text.matchAll(/\[(web|rate|tldr|think)(?:\s+([^\]]*))?\]/gi)) {
    const wanted = level?.trim().toLowerCase();
    if (name.toLowerCase() === "think") {
      const effort = effortOf(wanted || "high");
      if (!effort) return { error: `That's not a think level. Use ${efforts.slice(0, -1).join(", ")} or max, or plain [think] for high.` };
      tags.think = effort;
      continue;
    }
    const tag = switches.find((each) => each === name.toLowerCase());
    if (tag && !wanted) tags[tag] = true;
  }
  return tags;
}

// What an answer counts toward the daily limit: one, plus one for a web search and one for [tldr]. Asked before the
// answer, with searched as whether it may search.
export const questionsFor = (tags: Tags, searched: boolean) => 1 + Number(searched) + Number(tags.tldr);

// For the line under the answer, such as ["web", "think high"].
export const tagNames = (tags: Tags) => [...switches.filter((tag) => tags[tag]), ...(tags.think ? [`think ${tags.think}`] : [])];
