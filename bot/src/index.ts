import { Client, Events, GatewayIntentBits, Partials, type Message } from "discord.js";
import { commands, handleInteraction } from "./commands.ts";
import { removeMessage } from "./context.ts";
import { channelId, limit, tierOf } from "./limit.ts";
import { hasFlag } from "./members.ts";
import { replyTo } from "./reply.ts";
import { questionsFor, readTags } from "./tags.ts";

const token = process.env.DISCORD_TOKEN;
if (!token) throw new Error("DISCORD_TOKEN is not set");
function inChannel(message: Message<true>): boolean {
  const { channel } = message;
  return !channelId || channel.id === channelId || (channel.isThread() && channel.parentId === channelId);
}

async function handleMessage(message: Message) {
  if (message.author.bot || message.system || !message.inGuild()) return;
  // A reply to one of the bot's messages mentions it too, unless the person turned the ping off.
  if (!message.mentions.has(message.client.user, { ignoreEveryone: true, ignoreRoles: true })) return;
  if (hasFlag("excluded", message.author.id)) return;

  const tags = readTags(message.content);
  const tier = message.member ? tierOf(message.member) : 0;
  if (tier !== "team") {
    const verdict = limit(message.author.id, tier, "error" in tags ? 1 : questionsFor(tags, tags.web));
    if (typeof verdict === "object") await message.reply(verdict.warn);
    if (verdict !== "answer") return;
    // Checked after the limit, so pinging the bot elsewhere over and over doesn't get a reply every time.
    if (tier === 0 && !inChannel(message)) {
      await message.reply(`Ask me in <#${channelId}>.`);
      return;
    }
  }
  // After the limit too, for the same reason.
  if ("error" in tags) {
    await message.reply(tags.error);
    return;
  }
  await replyTo(message, message.author.id, tags);
}

const client = new Client({
  // GuildMembers keeps the roles of members the bot has seen current, for the roles it names in the conversation.
  intents: [GatewayIntentBits.Guilds, GatewayIntentBits.GuildMembers, GatewayIntentBits.GuildMessages, GatewayIntentBits.MessageContent],
  // Without this, deleting a message the bot hasn't seen since it started goes unnoticed.
  partials: [Partials.Message],
});

client.once(Events.ClientReady, async (ready) => {
  await ready.application.commands.set(commands);
  console.log(`Logged in as ${ready.user.tag}`);
});

// One failed message or command is logged and doesn't take the bot down.
client.on(Events.MessageCreate, (message) => {
  handleMessage(message).catch((error) => console.error(`Could not reply to message ${message.id}:`, error));
});
client.on(Events.InteractionCreate, (interaction) => {
  handleInteraction(interaction).catch((error) => console.error(`Could not handle interaction ${interaction.id}:`, error));
});
// A deleted message can't be picked for Remove from context any more, so it leaves the context with it.
client.on(Events.MessageDelete, (message) => {
  removeMessage(message.id);
});

await client.login(token);
