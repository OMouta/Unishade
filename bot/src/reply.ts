import { MessageFlags, type Message } from "discord.js";
import { answer } from "./answer.ts";
import { conversation } from "./conversation.ts";
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

// Answers the message in a reply to it, posted right away with the line saying it's working and edited as the answer
// comes in. When it's done, the line says how long it took and which tags it used. The answer counts toward userId,
// whoever asked for it.
export async function replyTo(message: Message<true>, userId: string, tags: Tags) {
  const started = Date.now();
  let text = "";
  let posted: Message<true> | undefined;
  async function show(line: string) {
    // Room for the line, and for closing a code block the answer is still in the middle of, which would take the line
    // in with it.
    let body = text.trim().slice(0, 2000 - line.length - 5);
    if ((body.match(/```/g)?.length ?? 0) % 2) body += "\n```";
    const content = [body, line].filter(Boolean).join("\n");
    // The answer comes from a model reading what users typed, so it pings nobody but the person asking.
    const allowedMentions = { parse: [], repliedUser: true };
    posted = posted
      ? await posted.edit({ content, allowedMentions })
      : await message.reply({ content, allowedMentions, flags: MessageFlags.SuppressEmbeds });
  }

  // Edits until the answer is in. wake ends the wait between two edits early, so the finished answer isn't held back.
  let done = false;
  let wake = () => {};
  const animation = (async () => {
    let word = pick(words);
    for (let edit = 0; !done; edit++) {
      if (edit && edit % 4 === 0) word = pick(words);
      const dots = ". ".repeat((edit % 3) + 1).trim();
      await show(`-# ${pick(emojis)} ${word} ${dots}`).catch((error) =>
        console.error(`Could not update the reply to message ${message.id}:`, error),
      );
      if (done) break;
      await new Promise<void>((resolve) => {
        wake = () => resolve();
        setTimeout(resolve, editInterval);
      });
    }
  })();

  let line = "";
  try {
    const answered = await answer(await conversation(message, tags), (sofar) => {
      text = sofar;
    });
    recordAnswer(userId, answered, questionsFor(tags, answered.searched));
    text = answered.text;
    const names = tagNames(tags);
    line = [
      `-# Done in ${Math.round((Date.now() - started) / 1000)}s`,
      names.length && `Tags: ${names.join(", ")}`,
      answered.searched && "Searched the web",
    ]
      .filter(Boolean)
      .join(" · ");
  } catch (error) {
    console.error(`Could not answer message ${message.id}:`, error);
    text = "I couldn't answer that right now. Try again in a minute.";
  }
  done = true;
  wake();
  await animation;
  await show(line);
}
