import { MessageFlags, type Message } from "discord.js";
import { answer } from "./answer.ts";
import { conversation } from "./conversation.ts";
import { recordAnswer } from "./usage.ts";

// Discord takes about 5 edits every 5 seconds in a channel, so the reply catches up with the answer this often at most.
const editInterval = 1_500;

// Answers the message in a reply to it, posted with the first words of the answer and edited as the rest comes in. The
// answer counts toward userId, whoever asked for it.
export async function replyTo(message: Message<true>, userId: string) {
  await message.channel.sendTyping();

  // Discord shows "is typing" for 10 seconds and a slow model takes longer, so it's sent again until the answer is in.
  // A failed one only costs the indicator.
  const typing = setInterval(() => void message.channel.sendTyping().catch(() => {}), 8_000);

  let text = "";
  let done = false;
  let shown = "";
  let posted: Message<true> | undefined;
  async function show() {
    if (!text.trim()) return;
    // Until the answer is complete, it ends in "..." to show more is coming.
    const content = done ? text.trim().slice(0, 2000) : `${text.trim().slice(0, 1997)}...`;
    if (content === shown) return;
    // The answer comes from a model reading what users typed, so it pings nobody but the person asking.
    const allowedMentions = { parse: [], repliedUser: true };
    posted = posted
      ? await posted.edit({ content, allowedMentions })
      : await message.reply({ content, allowedMentions, flags: MessageFlags.SuppressEmbeds });
    shown = content;
  }
  // One at a time, so the reply is posted once and each edit shows the newest text.
  let updates = Promise.resolve();
  let lastUpdate = 0;
  function update() {
    lastUpdate = Date.now();
    const next = updates.then(show);
    updates = next.catch(() => {});
    return next;
  }

  try {
    const answered = await answer(await conversation(message), (sofar) => {
      text = sofar;
      if (Date.now() - lastUpdate < editInterval) return;
      update().catch((error) => console.error(`Could not update the reply to message ${message.id}:`, error));
    });
    recordAnswer(userId, answered.tokens, answered.cost);
    text = answered.text;
  } catch (error) {
    console.error(`Could not answer message ${message.id}:`, error);
    text = "I couldn't answer that right now. Try again in a minute.";
  } finally {
    clearInterval(typing);
  }
  done = true;
  await update();
}
