import { ButtonBuilder, ButtonStyle, ChannelType, MessageFlags, type ButtonInteraction } from "discord.js";
import { exchangeOf } from "./conversation.ts";
import { channelIds, teamRoleIds, tierOf } from "./limit.ts";
import { logging, logNote } from "./log.ts";
import { reviewSolved } from "./review.ts";
import { addThread, callHuman, forgetThread, supportThread, threadOf, type SupportThread } from "./threads.ts";

// Private threads for someone's problem, with only them and the team in it. The bot offers one under an answer, and in
// it answers the person without a mention, until it's solved or they ask for a human.

const ephemeral = (content: string) => ({ content, flags: MessageFlags.Ephemeral }) as const;

// Under an answer that offers a private thread. Only the person who asked can use it, and it goes once they have.
export const openThreadButton = (asker: string) =>
  new ButtonBuilder().setCustomId(`thread:${asker}`).setLabel("Open a private thread").setStyle(ButtonStyle.Primary);

// Who is having a thread opened right now, so pressing the button twice quickly doesn't open two.
const opening = new Set<string>();

// Under each answer in a private thread. Get a human goes once someone has pressed it.
export const threadButtons = (thread: SupportThread) => [
  new ButtonBuilder().setCustomId("solved").setLabel("Solved").setStyle(ButtonStyle.Success),
  ...(thread.human ? [] : [new ButtonBuilder().setCustomId("human").setLabel("Get a human").setStyle(ButtonStyle.Secondary)]),
];

async function openThread(interaction: ButtonInteraction<"cached">, asker: string) {
  if (interaction.user.id !== asker) {
    await interaction.reply(ephemeral(`This one is for <@${asker}>. Ask me yourself, and I can open one for you too.`));
    return;
  }
  // One open thread each. One that archived is left behind, so a new problem gets a new thread.
  const open = threadOf(asker);
  if (open) {
    const existing = await interaction.client.channels.fetch(open).catch(() => null);
    if (existing?.isThread() && !existing.archived) {
      await interaction.reply(ephemeral(`You have one open already: ${existing}`));
      return;
    }
    forgetThread(open);
  }
  if (opening.has(asker)) {
    await interaction.deferUpdate();
    return;
  }
  // In the channel the answer is in, or its thread's, when it's one of the support channels or there are none. Otherwise
  // in the first support channel.
  const here = interaction.channel?.isThread() ? interaction.channel.parent : interaction.channel;
  const parent =
    !channelIds.length || (here && channelIds.includes(here.id)) ? here : await interaction.client.channels.fetch(channelIds[0]).catch(() => null);
  if (parent?.type !== ChannelType.GuildText) {
    await interaction.reply(ephemeral("I can't open private threads here."));
    return;
  }
  opening.add(asker);
  try {
    // The button goes as soon as it's pressed, and comes back only if the thread can't be opened.
    const buttons = interaction.message.components;
    await interaction.update({ components: [] });
    const { question } = await exchangeOf(interaction.message);
    const name = [interaction.member.displayName, question.replace(/\s+/g, " ")].filter(Boolean).join(": ");
    let thread;
    try {
      thread = await parent.threads.create({ name: name.slice(0, 100), type: ChannelType.PrivateThread, invitable: false });
      await thread.members.add(asker);
    } catch (error) {
      await interaction.editReply({ components: buttons });
      await interaction.followUp(ephemeral("I couldn't open a private thread. Someone on the team can check my permissions."));
      throw error;
    }
    addThread(thread.id, asker);
    // The bot reads a thread's first message with every question, so the question it was opened for stays in view.
    const intro = `<@${asker}> Only you and the team can see this thread. Send logs and screenshots here, and ask without mentioning me.`;
    await thread.send({ content: question ? `${intro}\n\n>>> ${question.slice(0, 1500)}` : intro, allowedMentions: { users: [asker] } });
    await interaction.followUp(ephemeral(`Here's your thread: ${thread}`));
    await logNote(interaction.client, `<@${asker}> opened a private thread: ${thread}`);
  } finally {
    opening.delete(asker);
  }
}

// The person the thread is for and the team can use its buttons. Gives back the thread when this one can.
async function threadFor(interaction: ButtonInteraction<"cached">): Promise<SupportThread | undefined> {
  const thread = supportThread(interaction.channelId);
  if (!thread) {
    await interaction.reply(ephemeral("This thread is closed."));
    return undefined;
  }
  if (interaction.user.id !== thread.asker && tierOf(interaction.member) !== "team") {
    await interaction.reply(ephemeral(`Only <@${thread.asker}> and the team can do that.`));
    return undefined;
  }
  return thread;
}

async function getHuman(interaction: ButtonInteraction<"cached">) {
  const thread = await threadFor(interaction);
  if (!thread) return;
  if (thread.human) {
    await interaction.reply(ephemeral("The team knows already."));
    return;
  }
  callHuman(interaction.channelId);
  // Mentioning a role in a private thread adds its members who can see the channel, as long as it has fewer than 100.
  const roles = teamRoleIds.map((id) => `<@&${id}>`).join(" ");
  await interaction.reply({
    content: `${roles} <@${thread.asker}> asked for a human. I'll only answer here when someone mentions me.`.trim(),
    allowedMentions: { roles: teamRoleIds },
  });
  await logNote(interaction.client, `<@${interaction.user.id}> asked for a human in ${interaction.channel}.`);
}

async function solve(interaction: ButtonInteraction<"cached">) {
  const thread = await threadFor(interaction);
  const channel = interaction.channel;
  if (!thread || !channel?.isThread()) return;
  forgetThread(channel.id);
  await interaction.reply("Solved, so I'm closing the thread.");
  // Locked, only the team can open it again.
  await channel.edit({ archived: true, locked: true });
  if (logging) await reviewSolved(channel, interaction.user);
}

export const supportButtons = { thread: openThread, human: getHuman, solved: solve };
