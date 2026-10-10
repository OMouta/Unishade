import { PermissionFlagsBits, type GuildMember, type GuildTextBasedChannel } from "discord.js";
import { hasFlag } from "./members.ts";
import type { SupportThread } from "./threads.ts";
import { usedToday } from "./usage.ts";

const roleIds = (list: string | undefined) => list?.split(",").map((id) => id.trim()).filter(Boolean) ?? [];
// Role IDs separated by commas. The team, like admins, has no limit, and tiers 1 and 2 have higher limits. All three
// can ask in any channel. Someone with roles in several gets the highest.
export const teamRoleIds = roleIds(process.env.TEAM_ROLE_IDS);
const tier2RoleIds = roleIds(process.env.TIER2_ROLE_IDS);
const tier1RoleIds = roleIds(process.env.TIER1_ROLE_IDS);
// Tier 0, everyone else, is answered only in this channel and its threads, or anywhere when it's not set.
export const channelId = process.env.CHANNEL_ID;

export function inChannel(channel: GuildTextBasedChannel): boolean {
  return !channelId || channel.id === channelId || (channel.isThread() && channel.parentId === channelId);
}

export type Tier = 0 | 1 | 2;
const limits: Record<Tier, { perMinute: number; perDay: number }> = {
  0: { perMinute: 5, perDay: 50 },
  1: { perMinute: 6, perDay: 80 },
  2: { perMinute: 8, perDay: 200 },
};
// In a private thread every message gets an answer, so those count toward the thread instead of the day, this many.
const perThread = 40;

// Members an admin used Remove limits on count as the team.
export function tierOf(member: GuildMember): Tier | "team" {
  if (hasFlag("unlimited", member.id)) return "team";
  if (member.permissions.has(PermissionFlagsBits.Administrator) || member.roles.cache.hasAny(...teamRoleIds)) return "team";
  if (member.roles.cache.hasAny(...tier2RoleIds)) return 2;
  return member.roles.cache.hasAny(...tier1RoleIds) ? 1 : 0;
}

const day = 86_400_000;
// Days are UTC, like the usage records. Discord shows the timestamp in each reader's time zone.
const nextDay = (now: number) => `<t:${(Math.floor(now / day) + 1) * (day / 1000)}:R>`;

// When each person's questions in the last minute came in, and who has been told to slow down since their last answer.
const recent = new Map<string, number[]>();
const warned = new Set<string>();

// "answer" while the person is under their limits. Over one, what to tell them, with again set when they were told
// already since their last answer, so a mention that keeps going gets one reply and not one per message. The day is
// counted from the usage records, so it survives a restart and a failed answer doesn't use one up. Questions is the
// most this one can count as, with its tags. Thread is the private thread it's asked in, if it is.
export function limit(userId: string, tier: Tier, questions: number, thread?: SupportThread, now = Date.now()): "answer" | { warn: string; again: boolean } {
  const { perMinute, perDay } = limits[tier];
  const times = (recent.get(userId) ?? []).filter((time) => now - time < 60_000);
  recent.set(userId, times);
  const used = usedToday(userId);
  let warning: string | undefined;
  if (thread) {
    if (thread.answers >= perThread) warning = `I've given this thread all ${perThread} answers I can. Press Get a human under my last one to bring in the team.`;
  } else if (used >= perDay) warning = `You've used today's ${perDay} questions. You can ask again ${nextDay(now)}.`;
  else if (used + questions > perDay) warning = `This question can count as ${questions}, and you have ${perDay - used} left today.`;
  if (!warning && times.length >= perMinute) warning = `You can ask ${perMinute} questions a minute. Try again in a bit.`;
  if (!warning) {
    times.push(now);
    warned.delete(userId);
    return "answer";
  }
  const again = warned.has(userId);
  warned.add(userId);
  return { warn: warning, again };
}

// What /limits tells a member about themselves.
export function describeLimits(member: GuildMember, now = Date.now()): string {
  if (hasFlag("excluded", member.id)) return "The bot doesn't answer you.";
  const tier = tierOf(member);
  if (tier === "team") return "You can ask as often as you like, in any channel.";
  const { perMinute, perDay } = limits[tier];
  const where = tier === 0 && channelId ? `in <#${channelId}>` : "in any channel";
  return `You can ask ${perMinute} questions a minute and ${perDay} a day, ${where}. You've used ${usedToday(member.id)} today, and the count resets ${nextDay(now)}. [tldr] and a web search from [web] each count as one more question. Answers in a private thread with me don't count toward the day, up to ${perThread} a thread.`;
}
