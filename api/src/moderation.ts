import { eq } from "drizzle-orm";
import { Hono } from "hono";
import { deleteObjects } from "./bucket.ts";
import type { Db } from "./db.ts";
import { verifyInteraction } from "./discord.ts";
import { env } from "./env.ts";
import { imageKey, imageNames } from "./images.ts";
import { presets, users } from "./schema.ts";

// New presets and reports go to the review channel as embeds with buttons. Clicks come back to
// /discord/interactions, and the answer replaces the buttons with who decided.

type Preset = typeof presets.$inferSelect;
type Member = { user: { username: string; global_name: string | null }; roles: string[]; permissions: string };
type Interaction = {
  type: number;
  member?: Member;
  data?: { custom_id?: string; components?: { component?: { custom_id?: string; value?: string } }[] };
};

const interactionType = { ping: 1, component: 3, modalSubmit: 5 };
const responseType = { pong: 1, message: 4, updateMessage: 7, modal: 9 };
const buttonStyle = { secondary: 2, success: 3, danger: 4 };
const reviewColor = 0xfee75c;
const reportColor = 0xe67e22;
const administrator = 1n << 3n;

function buttons(...list: [label: string, style: number, customId: string][]) {
  return [{ type: 1, components: list.map(([label, style, custom_id]) => ({ type: 2, label, style, custom_id })) }];
}

export function reviewMessage(preset: Preset, game: string) {
  const fields = [
    { name: "Game", value: game, inline: true },
    { name: "By", value: `<@${preset.userId}>`, inline: true },
    { name: "Effects", value: preset.effects.map((effect) => effect.replace(/\.fx$/i, "")).join(", ").slice(0, 1024) },
  ];
  if (preset.needsDepth) fields.push({ name: "Depth", value: "Needs depth estimation" });
  if (preset.dlss5) {
    const settings = Object.entries(preset.dlss5).map(([key, value]) => `${key.replace("DirectNeuralRendering", "")} ${value}`);
    fields.push({ name: "DLSS5", value: settings.join(", ") });
  }
  return {
    embeds: [
      {
        title: preset.name,
        description: preset.description || undefined,
        color: reviewColor,
        fields,
        image: { url: "attachment://after.jpg" },
        footer: { text: `Preset ${preset.id}` },
      },
      { title: "Before", color: reviewColor, image: { url: "attachment://before.jpg" } },
    ],
    components: buttons(
      ["Approve", buttonStyle.success, `approve:${preset.id}`],
      ["Reject", buttonStyle.danger, `reject:${preset.id}`],
      ["Reject and ban", buttonStyle.danger, `ban:${preset.id}`],
    ),
    allowed_mentions: { parse: [] },
  };
}

export function reportMessage(preset: Preset, game: string, reason: string, reporterId: string) {
  return {
    embeds: [
      {
        title: `Report: ${preset.name}`,
        description: preset.description || undefined,
        color: reportColor,
        fields: [
          { name: "Reason", value: reason },
          { name: "Reported by", value: `<@${reporterId}>`, inline: true },
          { name: "Game", value: game, inline: true },
          { name: "By", value: `<@${preset.userId}>`, inline: true },
        ],
        image: { url: "attachment://after.jpg" },
        footer: { text: `Preset ${preset.id}` },
      },
    ],
    components: buttons(
      ["Reject", buttonStyle.danger, `reject:${preset.id}`],
      ["Reject and ban", buttonStyle.danger, `ban:${preset.id}`],
      ["Dismiss", buttonStyle.secondary, `dismiss:${preset.id}`],
    ),
    allowed_mentions: { parse: [] },
  };
}

function isTeam(member: Member | undefined): member is Member {
  if (!member) return false;
  return (BigInt(member.permissions) & administrator) !== 0n || member.roles.some((role) => env.teamRoleIds.includes(role));
}

// Replaces the buttons with what happened. Reasons are typed by moderators, so nothing in them pings.
function decided(content: string) {
  return { type: responseType.updateMessage, data: { content, components: [], allowed_mentions: { parse: [] } } };
}

function reasonModal(action: "reject" | "ban", id: number) {
  return {
    type: responseType.modal,
    data: {
      custom_id: `${action}:${id}`,
      title: action === "ban" ? "Reject and ban" : "Reject",
      components: [
        {
          type: 18,
          label: "Reason",
          description: "The uploader sees this. You can leave it empty.",
          component: { type: 4, custom_id: "reason", style: 2, required: false, max_length: 300 },
        },
      ],
    },
  };
}

async function approve(db: Db, id: number, moderator: string): Promise<string> {
  const [preset] = await db.select({ status: presets.status }).from(presets).where(eq(presets.id, id));
  if (!preset) return "The uploader deleted this preset.";
  if (preset.status !== "pending") return `Already ${preset.status}.`;
  await db.update(presets).set({ status: "approved" }).where(eq(presets.id, id));
  return `Approved by ${moderator}`;
}

// Bans go through even when the preset was rejected already, such as from a second report.
async function reject(db: Db, id: number, moderator: string, reason: string | undefined, ban: boolean): Promise<string> {
  const [preset] = await db.select({ status: presets.status, userId: presets.userId }).from(presets).where(eq(presets.id, id));
  if (!preset) return "The uploader deleted this preset.";
  if (preset.status === "rejected" && !ban) return "Already rejected.";
  if (preset.status !== "rejected") {
    await db.update(presets).set({ status: "rejected", rejectReason: reason ?? null }).where(eq(presets.id, id));
    await deleteObjects(imageNames.map((name) => imageKey(id, name)));
  }
  if (ban) await db.update(users).set({ bannedAt: new Date() }).where(eq(users.id, preset.userId));
  const by = `${ban ? "Rejected and banned" : "Rejected"} by ${moderator}`;
  return reason ? `${by}: ${reason}` : by;
}

export function moderationRoutes(db: Db) {
  return new Hono().post("/discord/interactions", async (c) => {
    const body = await c.req.text();
    if (!verifyInteraction(body, c.req.header("x-signature-ed25519"), c.req.header("x-signature-timestamp"))) return c.text("Bad signature", 401);
    const interaction = JSON.parse(body) as Interaction;
    if (interaction.type === interactionType.ping) return c.json({ type: responseType.pong });

    const member = interaction.member;
    if (!isTeam(member)) return c.json({ type: responseType.message, data: { content: "Only the team can review presets.", flags: 64 } });
    const moderator = member.user.global_name ?? member.user.username;
    const [action, idText] = (interaction.data?.custom_id ?? "").split(":");
    const id = Number(idText);
    if (!Number.isSafeInteger(id)) return c.text("Unknown interaction", 400);

    if (interaction.type === interactionType.component) {
      if (action === "approve") return c.json(decided(await approve(db, id, moderator)));
      if (action === "reject" || action === "ban") return c.json(reasonModal(action, id));
      if (action === "dismiss") return c.json(decided(`Dismissed by ${moderator}`));
    }
    if (interaction.type === interactionType.modalSubmit && (action === "reject" || action === "ban")) {
      const reason = interaction.data?.components?.find((label) => label.component?.custom_id === "reason")?.component?.value?.trim();
      return c.json(decided(await reject(db, id, moderator, reason || undefined, action === "ban")));
    }
    return c.text("Unknown interaction", 400);
  });
}
