import { EmbedBuilder, PermissionFlagsBits, type GuildMember, type GuildTextBasedChannel } from "discord.js";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { dataDir } from "./context.ts";
import { hasFlag } from "./members.ts";
import type { SupportThread } from "./threads.ts";
import { usedToday } from "./usage.ts";

const ids = (list: string | undefined) => list?.split(",").map((id) => id.trim()).filter(Boolean) ?? [];
// Role IDs separated by commas. The team, like admins, has no limit, and tiers 1 and 2 have higher limits. All three
// can ask in any channel. Someone with roles in several gets the highest.
export const teamRoleIds = ids(process.env.TEAM_ROLE_IDS);
const tier2RoleIds = ids(process.env.TIER2_ROLE_IDS);
const tier1RoleIds = ids(process.env.TIER1_ROLE_IDS);
// Channel IDs separated by commas. Tier 0, everyone else, is answered only in these and their threads, or anywhere when
// there are none.
export const channelIds = ids(process.env.CHANNEL_ID);
// Where tier 0 can ask, such as "<#1>, <#2> or <#3>".
const mentions = channelIds.map((id) => `<#${id}>`);
export const channelList = mentions.length > 1 ? `${mentions.slice(0, -1).join(", ")} or ${mentions.at(-1)}` : (mentions[0] ?? "");

export function inChannel(channel: GuildTextBasedChannel): boolean {
  if (!channelIds.length || channelIds.includes(channel.id)) return true;
  return channel.isThread() && !!channel.parentId && channelIds.includes(channel.parentId);
}

export type Tier = 0 | 1 | 2;
type Limits = { perHour: number; perDay: number };
const defaultLimits: Record<Tier, Limits> = {
  0: { perHour: 15, perDay: 50 },
  1: { perHour: 25, perDay: 80 },
  2: { perHour: 60, perDay: 200 },
};
// In a private thread every message gets an answer, so those count toward the thread instead of the hour and the day,
// this many.
const defaultPerThread = 40;

// What the team changed with /limit-settings, kept across restarts. Only what they changed is saved, so the defaults
// above still apply to the rest. A boost multiplies the hour and day limits until it ends, or with no end, until it's
// set back to 1.
type Settings = { tiers: Partial<Record<Tier, Partial<Limits>>>; perThread?: number; boost?: { multiplier: number; until?: number } };
const file = path.join(dataDir, "limits.json");
const settings: Settings = existsSync(file) ? JSON.parse(readFileSync(file, "utf8")) : { tiers: {} };

function save() {
  mkdirSync(dataDir, { recursive: true });
  writeFileSync(file, JSON.stringify(settings, null, 2));
}

const baseLimits = (tier: Tier): Limits => ({ ...defaultLimits[tier], ...settings.tiers[tier] });
const threadLimit = () => settings.perThread ?? defaultPerThread;
const activeBoost = (now: number) => (settings.boost && (!settings.boost.until || settings.boost.until > now) ? settings.boost : undefined);

function limitsOf(tier: Tier, now: number): Limits {
  const { perHour, perDay } = baseLimits(tier);
  const multiplier = activeBoost(now)?.multiplier ?? 1;
  return { perHour: Math.round(perHour * multiplier), perDay: Math.round(perDay * multiplier) };
}

// A multiplier of 1 ends the boost. Without days, it lasts until then.
export function setBoost(multiplier: number, days: number | null, now = Date.now()) {
  settings.boost = multiplier === 1 ? undefined : { multiplier, until: days ? now + days * day : undefined };
  save();
}

export function setTierLimits(tier: Tier, change: Partial<Limits>) {
  settings.tiers[tier] = { ...settings.tiers[tier], ...change };
  save();
}

export function setThreadLimit(answers: number) {
  settings.perThread = answers;
  save();
}

export const tierNames: Record<Tier, string> = { 0: "Everyone", 1: "Tier 1", 2: "Tier 2" };

// What /limit-settings shows the team after each change.
export function describeSettings(now = Date.now()): string {
  const boost = activeBoost(now);
  const tiers = ([0, 1, 2] as const).map((tier) => {
    const { perHour, perDay } = limitsOf(tier, now);
    const base = baseLimits(tier);
    return `${tierNames[tier]}: ${perHour} an hour, ${perDay} a day${boost ? ` (${base.perHour} and ${base.perDay} without the boost)` : ""}`;
  });
  return [
    `Boost: ${boost ? `${boost.multiplier}×, ${boost.until ? `ends ${timestamp(boost.until, "f")}` : "until you set it back to 1"}` : "off"}`,
    ...tiers,
    `Private threads: ${threadLimit()} answers each`,
  ].join("\n");
}

// Members an admin used Remove limits on count as the team.
export function tierOf(member: GuildMember): Tier | "team" {
  if (hasFlag("unlimited", member.id)) return "team";
  if (member.permissions.has(PermissionFlagsBits.Administrator) || member.roles.cache.hasAny(...teamRoleIds)) return "team";
  if (member.roles.cache.hasAny(...tier2RoleIds)) return 2;
  return member.roles.cache.hasAny(...tier1RoleIds) ? 1 : 0;
}

const hour = 3_600_000;
const day = 86_400_000;
// Days are UTC, like the usage records.
const nextDay = (now: number) => (Math.floor(now / day) + 1) * day;
// Discord shows it in each reader's time zone, such as "in 23 minutes", or with "f", the date and time.
const timestamp = (time: number, style: "R" | "f" = "R") => `<t:${Math.floor(time / 1000)}:${style}>`;

// Each person's hour starts with their first question in it, and resets an hour later. It counts each question as the
// most it can count as when it's asked, so a burst of mentions can't all get through before the first is answered. Kept
// in memory, so a restart starts everyone's hour over.
const hours = new Map<string, { start: number; used: number }>();
function hourOf(userId: string, now: number) {
  const current = hours.get(userId);
  return current && now - current.start < hour ? current : undefined;
}

// Who has been told they can't ask since their last answer.
const warned = new Set<string>();

// Whether to tell someone they can't ask, which is once until their next answer, so a mention that keeps going gets one
// reply and not one per message.
export function firstWarning(userId: string): boolean {
  if (warned.has(userId)) return false;
  warned.add(userId);
  return true;
}

// "answer" while the person is under their limits. Over one, what to tell them, with again set when they were told
// already since their last answer. The day is counted from the usage records, so it survives a restart and a failed
// answer doesn't use one up. Questions is the most this one can count as, with its tags. Thread is the private thread
// it's asked in, if it is.
export function limit(userId: string, tier: Tier, questions: number, thread?: SupportThread, now = Date.now()): "answer" | { warn: string; again: boolean } {
  const { perHour, perDay } = limitsOf(tier, now);
  const perThread = threadLimit();
  let warning: string | undefined;
  if (thread) {
    if (thread.answers >= perThread) warning = `I've given this thread all ${perThread} answers I can. Press Get a human under my last one to bring in the team.`;
  } else {
    const today = usedToday(userId);
    const thisHour = hourOf(userId, now);
    const hourUsed = thisHour?.used ?? 0;
    if (today >= perDay) warning = `You've used today's ${perDay} questions. You can ask again ${timestamp(nextDay(now))}.`;
    else if (today + questions > perDay) warning = `This question can count as ${questions}, and you have ${perDay - today} left today.`;
    else if (thisHour && hourUsed >= perHour) warning = `You've used this hour's ${perHour} questions. You can ask again ${timestamp(thisHour.start + hour)}.`;
    else if (hourUsed + questions > perHour) warning = `This question can count as ${questions}, and you have ${perHour - hourUsed} left this hour.`;
    else if (thisHour) thisHour.used += questions;
    else hours.set(userId, { start: now, used: questions });
  }
  if (!warning) {
    warned.delete(userId);
    return "answer";
  }
  return { warn: warning, again: !firstWarning(userId) };
}

// A bar like `██████░░░░░░░░░░`, with any use showing.
const barLength = 16;
function bar(used: number, limit: number): string {
  const filled = Math.min(barLength, Math.ceil((used / limit) * barLength));
  return `\`${"█".repeat(filled)}${"░".repeat(barLength - filled)}\``;
}

// What /limits tells a member about themselves: a bar each for the hour and the day, and for the private thread it's
// used in.
export function describeLimits(member: GuildMember, thread?: SupportThread, now = Date.now()): { content: string } | { embeds: EmbedBuilder[] } {
  if (hasFlag("excluded", member.id)) return { content: "The bot doesn't answer you." };
  const tier = tierOf(member);
  if (tier === "team") return { content: "You can ask as often as you like, in any channel." };
  const { perHour, perDay } = limitsOf(tier, now);
  const perThread = threadLimit();
  const boost = activeBoost(now);
  const thisHour = hourOf(member.id, now);
  const hourUsed = thisHour?.used ?? 0;
  const today = usedToday(member.id);
  const where = tier === 0 && channelIds.length ? `You can ask in ${channelList}.` : "You can ask in any channel.";
  const boosted = boost && `**${boost.multiplier}× limits**${boost.until ? ` until ${timestamp(boost.until, "f")}` : " for now"}.`;
  const embed = new EmbedBuilder()
    .setTitle("Your limits")
    .setDescription(
      [
        boosted,
        `${where} [tldr] and a web search from [web] each count as one more question. Answers in a private thread with me only count toward the thread.`,
      ]
        .filter(Boolean)
        .join("\n\n"),
    )
    .addFields(
      {
        name: "This hour",
        value: `${bar(hourUsed, perHour)} ${hourUsed} of ${perHour}\n${thisHour ? `Resets ${timestamp(thisHour.start + hour)}` : "Starts with your next question"}`,
      },
      { name: "Today", value: `${bar(today, perDay)} ${today} of ${perDay}\nResets ${timestamp(nextDay(now))}` },
      ...(thread ? [{ name: "This thread", value: `${bar(thread.answers, perThread)} ${thread.answers} of ${perThread}` }] : []),
    );
  return { embeds: [embed] };
}
