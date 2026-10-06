import { EmbedBuilder, type Client, type GuildTextBasedChannel, type MessageCreateOptions, type User } from "discord.js";
import type { Answer } from "./answer.ts";
import { count, usd } from "./usage.ts";

// Every answer the bot gives or fails to give goes to this channel for the team, and so does who paused it. Nothing is
// logged when it's not set.
const channelId = process.env.LOG_CHANNEL_ID;

async function send(client: Client, options: MessageCreateOptions) {
  if (!channelId) return;
  const channel = await client.channels.fetch(channelId);
  if (!channel?.isSendable()) throw new Error(`Can't post in the log channel ${channelId}`);
  // The log names members, and nobody gets pinged for it.
  await channel.send({ ...options, allowedMentions: { parse: [] } });
}

export const logNote = (client: Client, text: string) => send(client, { content: text });

export type Entry = {
  asker: User;
  channel: GuildTextBasedChannel;
  // What they asked, with the names of the files they attached.
  question: string;
  tags: string[];
  // The reply, unless it couldn't be posted.
  url: string | undefined;
  seconds: number;
  // The answer, or why there's none.
  result: Answer | { error: string };
};

const clip = (text: string, length: number) => (text.length > length ? `${text.slice(0, length - 1)}…` : text);
const quote = (text: string) => text.split("\n").map((line) => `> ${line}`).join("\n");

export async function logAnswer({ asker, channel, question, tags, url, seconds, result }: Entry) {
  const failed = "error" in result;
  const where = `<@${asker.id}> in <#${channel.id}>${url ? ` · [Reply](${url})` : ""}`;
  const embed = new EmbedBuilder()
    .setAuthor({ name: asker.displayName, iconURL: asker.displayAvatarURL() })
    // Clipped after quoting, since quoting a question of many short lines adds to it.
    .setDescription([where, clip(quote(question), 1000), failed ? `**Couldn't answer:** ${clip(result.error, 1000)}` : clip(result.text, 2000)].join("\n\n"))
    .setFooter({
      text: [
        tags.length && `Tags: ${tags.join(", ")}`,
        `${seconds}s`,
        !failed && result.model,
        !failed && `${count(result.tokens)} tokens`,
        !failed && usd(result.cost),
        !failed && result.searched && "Searched the web",
      ]
        .filter(Boolean)
        .join(" · "),
    })
    .setTimestamp();
  if (failed) embed.setColor(0xed4245);
  return send(channel.client, { embeds: [embed] });
}
