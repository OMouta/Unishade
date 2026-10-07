import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { apiKey } from "./answer.ts";
import { dataDir } from "./context.ts";

// Questions is what the answers count toward the daily limit, and searches how many of them searched the web. Records
// from before tags have neither, and count one question per answer.
type Totals = { answers: number; questions?: number; searches?: number; tokens: number; cost: number };

// What the bot's answers cost, by UTC day and then by the user ID of who asked, such as
// { "2026-10-04": { "123": { answers: 3, questions: 4, searches: 1, tokens: 9000, cost: 0.0074 } } }.
const keptDays = 30;
const file = path.join(dataDir, "usage.json");
const days: Record<string, Record<string, Totals>> = existsSync(file) ? JSON.parse(readFileSync(file, "utf8")) : {};

const dayOf = (time: number) => new Date(time).toISOString().slice(0, 10);
const oldestKept = () => dayOf(Date.now() - (keptDays - 1) * 86_400_000);
const none = (): Totals => ({ answers: 0, questions: 0, searches: 0, tokens: 0, cost: 0 });
const questionsOf = (totals: Totals) => totals.questions ?? totals.answers;

export function usedToday(userId: string): number {
  const totals = days[dayOf(Date.now())]?.[userId];
  return totals ? questionsOf(totals) : 0;
}

// How much someone has used the bot, for [touchgrass].
export function habitsOf(userId: string): string {
  const today = dayOf(Date.now());
  const weekStart = dayOf(Date.now() - 6 * 86_400_000);
  const habits = { today: 0, week: 0, month: 0, days: 0, tokens: 0 };
  for (const [day, members] of Object.entries(days)) {
    const totals = members[userId];
    if (!totals || day < oldestKept()) continue;
    if (day === today) habits.today = totals.answers;
    if (day >= weekStart) habits.week += totals.answers;
    habits.month += totals.answers;
    habits.days++;
    habits.tokens += totals.tokens;
  }
  return `${count(habits.today)} answers today, ${count(habits.week)} in the last 7 days and ${count(habits.month)} in the last ${keptDays}, on ${habits.days} different days, which took ${count(habits.tokens)} tokens`;
}

export function recordAnswer(userId: string, { tokens, cost, searched }: { tokens: number; cost: number; searched: boolean }, questions: number) {
  const totals = ((days[dayOf(Date.now())] ??= {})[userId] ??= none());
  totals.questions = questionsOf(totals) + questions;
  totals.answers++;
  if (searched) totals.searches = (totals.searches ?? 0) + 1;
  totals.tokens += tokens;
  totals.cost += cost;
  const oldest = oldestKept();
  for (const day of Object.keys(days)) if (day < oldest) delete days[day];
  mkdirSync(dataDir, { recursive: true });
  writeFileSync(file, JSON.stringify(days, null, 2));
}

export const usd = (amount: number) => (amount === 0 ? "$0" : `$${amount < 1 ? amount.toPrecision(2) : amount.toFixed(2)}`);
export const count = (amount: number) => amount.toLocaleString("en-US");
const describe = ({ answers, searches, tokens, cost }: Totals) =>
  `${count(answers)} answers${searches ? ` (${count(searches)} searched the web)` : ""}, ${count(tokens)} tokens, ${usd(cost)}`;

function add(into: Totals, totals: Totals) {
  into.answers += totals.answers;
  into.searches = (into.searches ?? 0) + (totals.searches ?? 0);
  into.tokens += totals.tokens;
  into.cost += totals.cost;
}

async function openRouter<T>(endpoint: string): Promise<T> {
  const response = await fetch(`https://openrouter.ai/api/v1/${endpoint}`, {
    headers: { authorization: `Bearer ${apiKey}` },
    signal: AbortSignal.timeout(10_000),
  });
  if (!response.ok) throw new Error(`OpenRouter answered ${response.status}: ${await response.text()}`);
  return ((await response.json()) as { data: T }).data;
}

async function openRouterLines(): Promise<string[]> {
  const [key, credits] = await Promise.all([
    openRouter<{
      limit: number | null;
      limit_remaining: number | null;
      limit_reset: string | null;
      usage_daily: number;
      usage_weekly: number;
      usage_monthly: number;
      free_model_daily_requests?: { used: number; limit: number };
    }>("key"),
    openRouter<{ total_credits: number; total_usage: number }>("credits"),
  ]);
  const lines = [`Spent: ${usd(key.usage_daily)} today, ${usd(key.usage_weekly)} this week, ${usd(key.usage_monthly)} this month`];
  if (key.limit !== null) {
    lines.push(`Key limit: ${usd(key.limit_remaining ?? 0)} left of ${usd(key.limit)}${key.limit_reset ? ` (resets ${key.limit_reset})` : ""}`);
  }
  lines.push(`Credits: ${usd(credits.total_credits - credits.total_usage)} left of ${usd(credits.total_credits)}`);
  const free = key.free_model_daily_requests;
  if (free) lines.push(`Free model requests today: ${count(free.used)} of ${count(free.limit)}`);
  return lines;
}

export async function usageReport(): Promise<string> {
  // The bot's own numbers are still worth showing when OpenRouter can't be reached.
  const openRouterPart = await openRouterLines().catch((error) => {
    console.error("Could not fetch usage from OpenRouter:", error);
    return ["Couldn't reach OpenRouter. The error is in the bot's log."];
  });

  const todayKey = dayOf(Date.now());
  // Days past the window stay in the file until the next answer prunes them.
  const oldest = oldestKept();
  const today = none();
  const all = none();
  const byMember = new Map<string, Totals>();
  for (const [day, members] of Object.entries(days)) {
    if (day < oldest) continue;
    for (const [userId, totals] of Object.entries(members)) {
      if (day === todayKey) add(today, totals);
      add(all, totals);
      let member = byMember.get(userId);
      if (!member) byMember.set(userId, (member = none()));
      add(member, totals);
    }
  }
  const top = [...byMember].sort(([, a], [, b]) => b.answers - a.answers).slice(0, 5);

  return [
    "**OpenRouter**",
    ...openRouterPart,
    "",
    "**Bot**",
    `Today: ${describe(today)}`,
    `Last ${keptDays} days: ${describe(all)}`,
    ...(top.length ? [`Most answers in the last ${keptDays} days:`, ...top.map(([userId, totals]) => `- <@${userId}>: ${describe(totals)}`)] : []),
  ].join("\n");
}
