import {
  ActivityType,
  ApplicationCommandType,
  ContextMenuCommandBuilder,
  InteractionContextType,
  MessageFlags,
  PermissionFlagsBits,
  SlashCommandBuilder,
  type ChatInputCommandInteraction,
  type Interaction,
} from "discord.js";
import { removeMessage, saveMessage, savedMessages } from "./context.ts";
import { channelId, describeLimits, inChannel, limit, tierOf } from "./limit.ts";
import { logNote } from "./log.ts";
import { hasFlag, setFlag, type Flag } from "./members.ts";
import { replyTo, respond } from "./reply.ts";
import { effortOf, efforts, questionsFor, readTags, type Tags } from "./tags.ts";
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

// Discord shows these, all but /limits and /ask, to members who can manage the server. Server Settings > Integrations
// changes who that is.
export const commands = [
  new SlashCommandBuilder()
    .setName("limits")
    .setDescription("See how many questions you can ask the bot")
    .setContexts(InteractionContextType.Guild),
  new SlashCommandBuilder()
    .setName("ask")
    .setDescription("Ask the bot a question")
    .setContexts(InteractionContextType.Guild)
    .addStringOption((option) => option.setName("question").setDescription("What to ask").setRequired(true).setMaxLength(1000))
    .addAttachmentOption((option) => option.setName("file").setDescription("A log or screenshot to go with it"))
    .addBooleanOption((option) => option.setName("web").setDescription("Let it search the web. Counts as 2 when it does"))
    .addBooleanOption((option) => option.setName("rate").setDescription("Rate the screenshot out of 10"))
    .addBooleanOption((option) => option.setName("tldr").setDescription("Sum up this channel. Counts as 2"))
    .addStringOption((option) =>
      option
        .setName("think")
        .setDescription("How hard it thinks")
        .addChoices(...efforts.map((effort) => ({ name: effort, value: effort }))),
    ),
  new SlashCommandBuilder()
    .setName("pause")
    .setDescription("Stop the bot answering anyone, or start it again")
    .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
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

// Set with /pause. Paused, the bot answers nobody, the team included, until someone runs /pause again or the bot
// restarts.
let paused = false;
export const isPaused = () => paused;
const pausedReply = "The bot is paused for now.";

async function togglePause(interaction: ChatInputCommandInteraction<"cached">) {
  paused = !paused;
  // Mentions get no reply while paused, so the bot's status says it is.
  interaction.client.user.setPresence(
    paused ? { status: "dnd", activities: [{ name: "Paused", state: "Paused", type: ActivityType.Custom }] } : { status: "online", activities: [] },
  );
  await interaction.reply(ephemeral(paused ? "Paused. The bot answers nobody until someone runs /pause again." : "The bot answers again."));
  await logNote(interaction.client, `<@${interaction.user.id}> ${paused ? "paused" : "unpaused"} the bot.`);
}

// Like a mention, but it can take a file and its tags are options.
async function ask(interaction: ChatInputCommandInteraction<"cached">) {
  const { member, channel, options } = interaction;
  if (hasFlag("excluded", member.id)) {
    await interaction.reply(ephemeral("The bot doesn't answer you."));
    return;
  }
  if (paused) {
    await interaction.reply(ephemeral(pausedReply));
    return;
  }
  // Discord leaves out a channel the bot can't see, such as a private thread it isn't in.
  if (!channel) {
    await interaction.reply(ephemeral("I can't read this channel."));
    return;
  }
  const text = options.getString("question", true);
  const typed = readTags(text);
  if ("error" in typed) {
    await interaction.reply(ephemeral(typed.error));
    return;
  }
  // The options add to any tags typed in the question.
  const tags: Tags = {
    web: typed.web || !!options.getBoolean("web"),
    rate: typed.rate || !!options.getBoolean("rate"),
    tldr: typed.tldr || !!options.getBoolean("tldr"),
    think: effortOf(options.getString("think") ?? "") ?? typed.think,
  };

  const tier = tierOf(member);
  if (tier !== "team") {
    // Nobody else sees these replies, so they come every time and not just once.
    const verdict = limit(member.id, tier, questionsFor(tags, tags.web));
    if (verdict !== "answer") {
      await interaction.reply(ephemeral(verdict.warn));
      return;
    }
    if (tier === 0 && !inChannel(channel)) {
      await interaction.reply(ephemeral(`Ask me in <#${channelId}>.`));
      return;
    }
  }
  await interaction.deferReply();
  const question = { id: interaction.id, channel, member, text, file: options.getAttachment("file") };
  await respond(question, member.id, tags, (edit) => interaction.editReply(edit));
}

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
    if (interaction.commandName === "ask") {
      await ask(interaction);
      return;
    }
    if (interaction.commandName === "pause") {
      await togglePause(interaction);
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
    if (paused) {
      await interaction.reply(ephemeral(pausedReply));
      return;
    }
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
