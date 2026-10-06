import {
  ApplicationCommandType,
  ContextMenuCommandBuilder,
  InteractionContextType,
  MessageFlags,
  PermissionFlagsBits,
  SlashCommandBuilder,
  type Interaction,
} from "discord.js";
import { removeMessage, saveMessage, savedMessages } from "./context.ts";
import { describeLimits } from "./limit.ts";
import { setFlag, type Flag } from "./members.ts";
import { replyTo } from "./reply.ts";
import { readTags } from "./tags.ts";
import { usageReport } from "./usage.ts";

const answerThis = "Answer this";
const add = "Add to context";
const remove = "Remove from context";

// Right-click a member to set or clear one of their flags. The reply says where they stand now.
const memberCommands: Record<string, { flag: Flag; on: boolean; reply: (name: string) => string }> = {
  "Exclude from AI": { flag: "excluded", on: true, reply: (name) => `The bot ignores ${name}.` },
  "Include in AI": { flag: "excluded", on: false, reply: (name) => `The bot answers ${name}.` },
  "Remove limits": { flag: "unlimited", on: true, reply: (name) => `${name} can ask as often as they like, in any channel.` },
  "Restore limits": { flag: "unlimited", on: false, reply: (name) => `${name} has the usual limits again.` },
};

const contextMenuCommand = (name: string, type: ApplicationCommandType.Message | ApplicationCommandType.User) =>
  new ContextMenuCommandBuilder()
    .setName(name)
    .setType(type)
    .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
    .setContexts(InteractionContextType.Guild);

// Discord shows these, all but /limits, to members who can manage the server. Server Settings > Integrations changes
// who that is.
export const commands = [
  new SlashCommandBuilder()
    .setName("limits")
    .setDescription("See how many questions you can ask the bot")
    .setContexts(InteractionContextType.Guild),
  contextMenuCommand(answerThis, ApplicationCommandType.Message),
  contextMenuCommand(add, ApplicationCommandType.Message),
  contextMenuCommand(remove, ApplicationCommandType.Message),
  ...Object.keys(memberCommands).map((name) => contextMenuCommand(name, ApplicationCommandType.User)),
  new SlashCommandBuilder()
    .setName("context")
    .setDescription("List the messages the bot answers from")
    .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
    .setContexts(InteractionContextType.Guild),
  new SlashCommandBuilder()
    .setName("usage")
    .setDescription("Show what the bot spent and who it answered")
    .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
    .setContexts(InteractionContextType.Guild),
];

const ephemeral = (content: string) => ({ content, flags: MessageFlags.Ephemeral }) as const;

function list(): string {
  const lines = savedMessages().map(({ url, posted, text }) => `- [${posted}](<${url}>): ${text.replace(/\s+/g, " ").slice(0, 60)}`);
  if (!lines.length) return "No messages in the context. The docs are always included.";
  // A message holds 2000 characters.
  let shown = 0;
  let length = 0;
  while (shown < lines.length && length + lines[shown].length < 1900) length += lines[shown++].length + 1;
  const rest = lines.length - shown;
  return lines.slice(0, shown).join("\n") + (rest ? `\nand ${rest} more` : "");
}

export async function handleInteraction(interaction: Interaction) {
  if (!interaction.inCachedGuild()) return;

  if (interaction.isChatInputCommand()) {
    if (interaction.commandName === "context") {
      await interaction.reply(ephemeral(list()));
      return;
    }
    if (interaction.commandName === "limits") {
      await interaction.reply(ephemeral(describeLimits(interaction.member)));
      return;
    }
    // Asking OpenRouter can take longer than the 3 seconds Discord waits for a reply.
    await interaction.deferReply({ flags: MessageFlags.Ephemeral });
    // The report names members, and nobody gets pinged for it.
    await interaction.editReply({ content: await usageReport(), allowedMentions: { parse: [] } });
    return;
  }
  if (interaction.isUserContextMenuCommand()) {
    const { flag, on, reply } = memberCommands[interaction.commandName];
    setFlag(flag, interaction.targetUser.id, on);
    await interaction.reply(ephemeral(reply(interaction.targetMember?.displayName ?? interaction.targetUser.displayName)));
    return;
  }
  if (!interaction.isMessageContextMenuCommand()) return;

  const message = interaction.targetMessage;
  if (interaction.commandName === answerThis) {
    const tags = readTags(message.content);
    if ("error" in tags) {
      await interaction.reply(ephemeral(tags.error));
      return;
    }
    // The answer goes in a reply to the message. Until it's posted, only whoever picked the command sees the bot
    // thinking. Like the team's questions, these have no limit.
    await interaction.deferReply({ flags: MessageFlags.Ephemeral });
    await replyTo(message, interaction.user.id, tags);
    await interaction.deleteReply();
    return;
  }
  if (interaction.commandName === remove) {
    await interaction.reply(ephemeral(removeMessage(message.id) ? "Removed from the context." : "That message isn't in the context."));
    return;
  }
  const text = [message.cleanContent, ...message.embeds.flatMap((embed) => [embed.title, embed.description])].filter(Boolean).join("\n\n");
  if (!text) {
    await interaction.reply(ephemeral("That message has no text."));
    return;
  }
  const where = interaction.channel ? `#${interaction.channel.name}` : "Discord";
  const posted = `${where}, ${message.createdAt.toISOString().slice(0, 10)}`;
  // Adding a message again, such as after it was edited, replaces what was saved.
  const replaced = saveMessage({ id: message.id, url: message.url, posted, text });
  await interaction.reply(ephemeral(replaced ? "Updated in the context." : "Added to the context."));
}
