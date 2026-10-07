import { Message, MessageReferenceType, type Attachment, type GuildMember, type GuildTextBasedChannel, type User } from "discord.js";
import type { Tags } from "./tags.ts";
import { habitsOf } from "./usage.ts";

// How many messages before the mention are sent along, so a question asked over several messages reads as one. [tldr]
// reads the most Discord sends at once, to have more to sum up.
const historyLength = 10;
const tldrHistoryLength = 100;
// Long earlier messages are cut, so ten of them can't cost more than the question itself.
const maxEarlierLength = 400;
// Text files, such as Unishade.log, are read from the newest messages first, this many in all.
const maxFiles = 2;
// Unishade.log stops growing at 16 MB, and nothing bigger is downloaded.
const maxFileSize = 16 * 1024 * 1024;
// A longer file keeps its start, where the log names the version and system, and its end, where things went wrong.
const fileHead = 2_000;
const fileTail = 10_000;
// Pictures in the message and in the one it replies to go along with it, this many at most.
const maxImages = 4;

// The name and two highest roles, such as "Tiago (Admin, Support)", so the model can tell the team apart.
function nameWithRoles(user: User, member: GuildMember | null): string {
  const name = member?.displayName ?? user.displayName;
  const roles = member?.roles.cache
    .filter((role) => role.id !== member.guild.id)
    .sorted((a, b) => b.position - a.position)
    .first(2)
    .map((role) => role.name);
  return roles?.length ? `${name} (${roles.join(", ")})` : name;
}

async function describe(message: Message<true>): Promise<string> {
  if (message.author.id === message.client.user.id) return "You";
  // Messages fetched from history carry no member, so it's looked up. A webhook has none, and neither has someone who
  // left, and the name alone still works for those.
  const member = message.webhookId ? null : await message.guild.members.fetch(message.author.id).catch(() => null);
  return nameWithRoles(message.author, member);
}

const isTextFile = (attachment: Attachment) =>
  attachment.size <= maxFileSize && (/\.(log|txt)$/i.test(attachment.name) || !!attachment.contentType?.startsWith("text/"));
const isImage = (attachment: Attachment) => /^image\/(png|jpeg|webp)$/.test(attachment.contentType ?? "");

// A message without text, such as another bot's post, is read from its embeds. Attachments are listed by name.
function textOf(message: Message<true>): string {
  let text = message.cleanContent || message.embeds.flatMap((embed) => [embed.title, embed.description]).filter(Boolean).join("\n");
  // The line under the bot's own replies, such as "Done in 12s", isn't part of the answer, and the model would copy it.
  if (message.author.id === message.client.user.id) text = text.replace(/(^|\n)-# [^\n]*$/, "");
  return [text, ...message.attachments.map((attachment) => `[file: ${attachment.name}]`)].filter(Boolean).join(" ");
}

function cut(text: string): string {
  return text.length > maxEarlierLength ? `${text.slice(0, maxEarlierLength)}...` : text;
}

async function download(attachment: Attachment): Promise<string> {
  const response = await fetch(attachment.url, { signal: AbortSignal.timeout(30_000) });
  if (!response.ok) throw new Error(`Discord answered ${response.status} for ${attachment.url}`);
  const text = await response.text();
  if (text.length <= fileHead + fileTail) return text;
  return `${text.slice(0, fileHead)}\n[${text.length - fileHead - fileTail} characters left out]\n${text.slice(-fileTail)}`;
}

// A question asked with /ask rather than a mention: its text and the file attached to it. ID is the interaction's,
// which comes after the messages before it, so history is read from there.
export type Ask = { id: string; channel: GuildTextBasedChannel; member: GuildMember; text: string; file: Attachment | null };

// The question, which is the message that mentions the bot with the one it replies to, or the question from /ask, and
// apart from it the background to follow it by: where it was asked, what came before, and attached files. Kept apart,
// the model answers the question and not whatever is still open further up. Images are the URLs of its pictures, which
// the model fetches itself. Tags are the ones it was asked with.
export type Conversation = { background: string; mention: string; images: string[]; tags: Tags };

export async function conversation(question: Message<true> | Ask, tags: Tags): Promise<Conversation> {
  const message = question instanceof Message ? question : null;
  const { channel } = question;
  const [earlier, replied, starter] = await Promise.all([
    channel.messages.fetch({ limit: tags.tldr ? tldrHistoryLength : historyLength, before: question.id }),
    // A deleted message can't be fetched, and the question still stands without it.
    message?.reference?.type === MessageReferenceType.Default ? message.fetchReference().catch(() => null) : null,
    channel.isThread() ? channel.fetchStarterMessage().catch(() => null) : null,
  ]);
  // Newest first, which is also the order files are read in.
  const messages = [message, replied, ...earlier.values(), starter].filter((each) => each != null);
  // One lookup per author, however many messages they wrote.
  const authors = new Map(messages.map((each) => [each.author.id, each]));
  const names = new Map(await Promise.all([...authors].map(async ([id, each]) => [id, await describe(each)] as const)));
  const nameOf = (each: Message<true>) => names.get(each.author.id) ?? each.author.displayName;

  const asked =
    question instanceof Message
      ? { id: question.author.id, name: nameOf(question), text: textOf(question), attachments: [...question.attachments.values()] }
      : {
          id: question.member.id,
          name: nameWithRoles(question.member.user, question.member),
          text: [question.text, question.file && `[file: ${question.file.name}]`].filter(Boolean).join(" "),
          attachments: question.file ? [question.file] : [],
        };

  const background = [channel.isThread() ? `Thread "${channel.name}" in #${channel.parent?.name}` : `Channel: #${channel.name}`];
  // In a long thread, the question that opened it is further back than the earlier messages go.
  if (starter && starter.id !== message?.id) background.push(`The thread starts with this message from ${nameOf(starter)}:\n${textOf(starter)}`);
  if (earlier.size) {
    const lines = [...earlier.values()].reverse().map((previous) => `${nameOf(previous)}: ${cut(textOf(previous))}`);
    background.push(`Earlier messages, oldest first:\n${lines.join("\n")}`);
  }
  if (tags.touchgrass) background.push(`How much ${asked.name} has asked you: ${habitsOf(asked.id)}.`);

  // Keyed by ID, since the message replied to can be one of the earlier ones too.
  const attached = [
    ...asked.attachments.map((attachment) => ({ attachment, from: asked.name })),
    ...[replied, ...earlier.values(), starter].flatMap((each) => (each ? each.attachments.map((attachment) => ({ attachment, from: nameOf(each) })) : [])),
  ];
  const files = new Map(attached.filter(({ attachment }) => isTextFile(attachment)).map((each) => [each.attachment.id, each]));
  const read = [...files.values()]
    .slice(0, maxFiles)
    .map(async ({ attachment, from }) => `<file name="${attachment.name}" from="${from}">\n${await download(attachment)}\n</file>`);
  background.push(...(await Promise.all(read)));

  const mention = `${asked.name}:\n${asked.text}`;
  return {
    background: background.join("\n\n"),
    mention: replied ? `In reply to this message from ${nameOf(replied)}:\n${textOf(replied)}\n\n${mention}` : mention,
    images: [...asked.attachments, ...(replied?.attachments.values() ?? [])]
      .filter(isImage)
      .slice(0, maxImages)
      .map((attachment) => attachment.url),
    tags,
  };
}
