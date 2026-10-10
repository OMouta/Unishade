// Tags someone puts in a question to change how the bot answers it, such as [web], or [think medium] for the one that
// takes a level. /tags lists them.
export const switches = ["web", "music", "rate", "shader", "tldr", "bug", "poll", "touchgrass"] as const;
type Switch = (typeof switches)[number];
export type Tags = Record<Switch, boolean> & { think?: Effort };

// What /tags and the /ask options say about each, with how many questions an answer with it counts as.
const about: Record<Switch, { does: string; counts: string }> = {
  web: { does: "searches the web", counts: "×2 when it searches" },
  music: { does: "finds songs on Spotify or SoundCloud", counts: "×2 when it searches" },
  rate: { does: "rates your screenshot out of 10", counts: "×1" },
  shader: { does: "writes a ReShade shader you can load", counts: "×1" },
  tldr: { does: "sums up the channel or thread", counts: "×2" },
  bug: { does: "writes a bug report you can open on GitHub", counts: "×1" },
  poll: { does: "starts a poll", counts: "×1" },
  touchgrass: { does: "checks whether you should go outside", counts: "×1" },
};

// Reasoning effort as OpenRouter takes it, but for "none", which openai/gpt-oss-20b rejects.
export const efforts = ["minimal", "low", "medium", "high", "xhigh", "max"] as const;
type Effort = (typeof efforts)[number];
export const effortOf = (level: string) => efforts.find((each) => each === level);

// A level that doesn't exist is an error, so a typo doesn't quietly get the default. [thinking] or [website] aren't
// tags at all.
export function readTags(text: string): Tags | { error: string } {
  const tags: Tags = { web: false, music: false, rate: false, shader: false, tldr: false, bug: false, poll: false, touchgrass: false };
  for (const [, name, level] of text.matchAll(new RegExp(`\\[(${switches.join("|")}|think)(?:\\s+([^\\]]*))?\\]`, "gi"))) {
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

export const maySearch = (tags: Tags) => tags.web || tags.music;

// What an answer counts toward the limits: one, plus one for a web search and one for [tldr]. Asked before the
// answer, with searched as whether it may search.
export const questionsFor = (tags: Tags, searched: boolean) => 1 + Number(searched) + Number(tags.tldr);

// For the line under the answer, such as ["web", "think high"].
export const tagNames = (tags: Tags) => [...switches.filter((tag) => tags[tag]), ...(tags.think ? [`think ${tags.think}`] : [])];

const capitalized = (text: string) => text[0].toUpperCase() + text.slice(1);
export const optionDescription = (tag: Switch) => `${capitalized(about[tag].does)} (${about[tag].counts})`;

// What /tags says.
export const tagList = [
  "Put these in a question when you mention me, or pick them in /ask. They stack, like `[web] [think max]`, and ×2 means the answer counts as 2 questions.",
  ...switches.map((tag) => `\`[${tag}]\` ${about[tag].does} · ${about[tag].counts}`),
  "`[think]` thinks harder, and `[think low]` to `[think max]` set how hard · ×1",
].join("\n");
