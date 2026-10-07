import { Message, MessageFlags, type AttachmentPayload, type MessageMentionOptions, type PollData } from "discord.js";
import { answer, type Answer } from "./answer.ts";
import { conversation, type Ask } from "./conversation.ts";
import { extras, shownWhileAnswering, type Extras } from "./extras.ts";
import { logAnswer } from "./log.ts";
import { questionsFor, tagNames, type Tags } from "./tags.ts";
import { recordAnswer } from "./usage.ts";

// Discord takes about 5 edits every 5 seconds in a channel, so the reply is edited this often at most. Each edit waits
// for the one before, so when Discord holds them back, the animation slows down instead of piling up.
const editInterval = 1_500;

// The line under the reply while the answer comes in, such as "-# 🎨 Tonemapping . .". The emoji changes every edit,
// the word every fourth, and the dots count up.
const emojis = ["✨", "🎨", "🖌️", "🌈", "🔆", "🔮", "🧪", "🎛️", "🌀", "💡", "🪄", "📸", "🕶️", "🌅", "🖼️"];
const words = [
  "Tonemapping",
  "Color grading",
  "Compiling shaders",
  "Sampling the depth buffer",
  "Tracing rays",
  "Chasing god rays",
  "Dithering",
  "Blooming",
  "Calibrating gamma",
  "Denoising",
  "Upscaling",
  "Polishing pixels",
  "Bending light",
  "Tweaking the LUT",
  "Defocusing",
  "Antialiasing",
  "Bouncing photons",
  "Squinting at pixels",
  "Warming up the GPU",
  "Reticulating splines",
  "Mipmapping",
  "Vignetting",
];
const pick = (list: readonly string[]) => list[Math.floor(Math.random() * list.length)];

// What the reply is posted and edited with. The answer comes from a model reading what users typed, so it pings nobody
// but the person asking. Links don't unfurl, but for the songs from [music], where no flags lets them.
type Edit = { content: string; allowedMentions: MessageMentionOptions; flags: MessageFlags.SuppressEmbeds | []; files?: AttachmentPayload[] };
// show posts the reply the first time and edits it after, giving back the message. follow posts the poll from [poll]
// under it, since a poll can't be edited into a message.
type Reply = {
  show: (edit: Edit) => Promise<Message>;
  follow: (message: { poll: PollData; allowedMentions: MessageMentionOptions }) => Promise<unknown>;
};

// Answers a message in a reply to it.
export function replyTo(message: Message<true>, userId: string, tags: Tags) {
  let posted: Message<true> | undefined;
  return respond(message, userId, tags, {
    show: async (edit) => (posted = posted ? await posted.edit(edit) : await message.reply(edit)),
    follow: (poll) => message.channel.send(poll),
  });
}

// Answers the question, shown right away with the line saying it's working and edited as the answer comes in. When it's
// done, the line says how long it took and which tags it used, and the answer goes to the log. The answer counts
// toward userId, whoever asked for it.
export async function respond(question: Message<true> | Ask, userId: string, tags: Tags, reply: Reply) {
  const started = Date.now();
  // A slash command doesn't show what was asked, so /ask quotes it above the answer.
  const asked =
    question instanceof Message
      ? { user: question.author, text: question.cleanContent, files: [...question.attachments.values()], heading: "" }
      : {
          user: question.member.user,
          text: question.text,
          files: question.file ? [question.file] : [],
          heading: `> ${question.text.length > 200 ? `${question.text.slice(0, 199)}…` : question.text}`,
        };

  // What's shown of the answer so far, and in the end, the finished one.
  let text = "";
  let shown: Message | undefined;
  async function update(line: string, final?: { files: AttachmentPayload[] }) {
    // Room for the line, and for closing a code block the answer is still in the middle of, which would take the line
    // in with it.
    let body = text.trim().slice(0, 2000 - asked.heading.length - line.length - 6);
    if ((body.match(/```/g)?.length ?? 0) % 2) body += "\n```";
    const content = [asked.heading, body, line].filter(Boolean).join("\n");
    const flags: Edit["flags"] = final && tags.music ? [] : MessageFlags.SuppressEmbeds;
    shown = await reply.show({ content, allowedMentions: { parse: [], repliedUser: true }, flags, ...final });
  }

  // Edits until the answer is in. wake ends the wait between two edits early, so the finished answer isn't held back.
  let done = false;
  let wake = () => {};
  const animation = (async () => {
    let word = pick(words);
    for (let edit = 0; !done; edit++) {
      if (edit && edit % 4 === 0) word = pick(words);
      const dots = ". ".repeat((edit % 3) + 1).trim();
      await update(`-# ${pick(emojis)} ${word} ${dots}`).catch((error) => console.error(`Could not update the reply to ${question.id}:`, error));
      if (done) break;
      await new Promise<void>((resolve) => {
        wake = () => resolve();
        setTimeout(resolve, editInterval);
      });
    }
  })();

  let result: Answer | { error: string };
  let made: Extras = { text: "I couldn't answer that right now. Try again in a minute.", files: [] };
  try {
    result = await answer(await conversation(question, tags), (sofar) => {
      text = shownWhileAnswering(sofar, tags);
    });
    made = await extras(result.text, tags);
    recordAnswer(userId, result, questionsFor(tags, result.searched));
  } catch (error) {
    console.error(`Could not answer ${question.id}:`, error);
    result = { error: error instanceof Error ? error.message : String(error) };
  }
  const seconds = Math.round((Date.now() - started) / 1000);
  const names = tagNames(tags);
  done = true;
  wake();
  await animation;
  text = made.text;
  // A failed answer gets no line.
  await update(
    "error" in result
      ? ""
      : [`-# Done in ${seconds}s`, names.length && `Tags: ${names.join(", ")}`, result.searched && "Searched the web"].filter(Boolean).join(" · "),
    { files: made.files },
  );
  if (made.poll) {
    await reply.follow({ poll: made.poll, allowedMentions: { parse: [] } }).catch((error) => console.error(`Could not post the poll for ${question.id}:`, error));
  }

  await logAnswer({
    asker: asked.user,
    channel: question.channel,
    question: [asked.text, ...asked.files.map((file) => `[file: ${file.name}]`)].filter(Boolean).join(" "),
    tags: names,
    url: shown?.url,
    seconds,
    result,
  }).catch((error) => console.error(`Could not log the answer to ${question.id}:`, error));
}
