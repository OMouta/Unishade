import {
  ActionRowBuilder,
  ButtonBuilder,
  ButtonStyle,
  EmbedBuilder,
  MessageFlags,
  ModalBuilder,
  TextInputStyle,
  type ButtonInteraction,
  type Message,
  type MessageContextMenuCommandInteraction,
  type ModalSubmitInteraction,
  type ThreadChannel,
  type User,
} from "discord.js";
import { summarize, type Answer } from "./answer.ts";
import { saveMessage } from "./context.ts";
import { exchangeOf, transcript } from "./conversation.ts";
import { clip, log } from "./log.ts";
import { count, usd } from "./usage.ts";

// What the team reviews in the log: answers someone reported, and private threads the bot summed up when they were
// solved. Either can go into the context from there, saved under the ID of the log message, so Remove from context on
// that message takes it out again.

const ephemeral = (content: string) => ({ content, flags: MessageFlags.Ephemeral }) as const;
const row = (...buttons: ButtonBuilder[]) => new ActionRowBuilder<ButtonBuilder>().addComponents(buttons);

// Answers reported since the bot started, so the team gets each one once.
const reported = new Set<string>();

// Anyone can right-click one of the bot's answers and pick Report answer.
export async function reportAnswer(interaction: MessageContextMenuCommandInteraction<"cached">) {
  const answer = interaction.targetMessage;
  if (answer.author.id !== interaction.client.user.id) {
    await interaction.reply(ephemeral("Only my answers can be reported."));
    return;
  }
  if (reported.has(answer.id)) {
    await interaction.reply(ephemeral("The team has this one already."));
    return;
  }
  reported.add(answer.id);
  await interaction.reply(ephemeral("Thanks. The team will take a look."));
  const exchange = await exchangeOf(answer);
  const embed = new EmbedBuilder()
    .setTitle("Reported")
    .setURL(answer.url)
    .setDescription(`<@${interaction.user.id}> in ${answer.channel}`)
    .addFields(
      { name: "Question", value: clip(exchange.question || "Not found", 1024) },
      { name: "Answer", value: clip(exchange.answer || "Not found", 1024) },
    )
    .setColor(0xfee75c)
    .setTimestamp();
  const correct = new ButtonBuilder().setCustomId("save:correction").setLabel("Add a correction").setStyle(ButtonStyle.Primary);
  await log(interaction.client, { embeds: [embed], components: [row(correct)] });
}

export async function reviewSolved(thread: ThreadChannel, by: User) {
  const result: Answer | { error: string } = await transcript(thread)
    .then(summarize)
    .catch((error) => {
      console.error(`Could not sum up thread ${thread.id}:`, error);
      return { error: error instanceof Error ? error.message : String(error) };
    });
  const failed = "error" in result;
  const embed = new EmbedBuilder()
    .setTitle(clip(`Solved: ${thread.name}`, 256))
    .setURL(thread.url)
    .setDescription(failed ? `**Couldn't sum it up:** ${clip(result.error, 1000)}` : clip(result.text, 4000))
    .setTimestamp();
  if (failed) embed.setColor(0xed4245);
  else embed.setFooter({ text: [result.model, `${count(result.tokens)} tokens`, usd(result.cost)].join(" · ") });
  const add = new ButtonBuilder().setCustomId("save:solved").setLabel("Add to context").setStyle(ButtonStyle.Primary);
  const discard = new ButtonBuilder().setCustomId("discard").setLabel("Discard").setStyle(ButtonStyle.Secondary);
  await log(thread.client, { content: `<@${by.id}> marked ${thread} as solved.`, embeds: [embed], components: failed ? [] : [row(add, discard)] });
}

// Kind is "solved" for a summary, which the form starts from, or "correction", which starts from the question.
async function openForm(interaction: ButtonInteraction<"cached">, kind: string) {
  const embed = interaction.message.embeds[0];
  const question = embed?.fields.find((field) => field.name === "Question")?.value;
  const text = kind === "solved" ? (embed?.description ?? "") : `Question: ${question ?? ""}\nAnswer: `;
  const modal = new ModalBuilder()
    .setCustomId(`save:${kind}`)
    .setTitle(kind === "solved" ? "Add to context" : "Add a correction")
    .addLabelComponents((label) =>
      label
        .setLabel("What the bot should know")
        .setTextInputComponent((input) => input.setCustomId("text").setStyle(TextInputStyle.Paragraph).setMaxLength(4000).setValue(text.slice(0, 4000))),
    );
  await interaction.showModal(modal);
}

// The log message with a line under it saying what the team did, and its buttons gone.
const resolved = (message: Message, line: string) => ({ content: [message.content, `-# ${line}`].filter(Boolean).join("\n"), components: [] });

async function discard(interaction: ButtonInteraction<"cached">) {
  await interaction.update(resolved(interaction.message, `Discarded by ${interaction.user.displayName}`));
}

export async function saveFromForm(interaction: ModalSubmitInteraction<"cached">) {
  if (!interaction.isFromMessage()) return;
  const { message } = interaction;
  const kind = interaction.customId.split(":")[1];
  const date = new Date().toISOString().slice(0, 10);
  saveMessage({
    id: message.id,
    // The thread or the answer the log message is about.
    url: message.embeds[0]?.url ?? message.url,
    posted: `${kind === "solved" ? "solved support thread" : "correction from the team"}, ${date}`,
    text: interaction.fields.getTextInputValue("text").trim(),
  });
  await interaction.update(resolved(message, `Added to the context by ${interaction.user.displayName}`));
}

export const reviewButtons = { save: openForm, discard };
