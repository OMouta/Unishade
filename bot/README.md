# Unishade bot (Unibot)

The Discord bot for the Unishade server. Mention it and it answers anything, in English. For Unishade questions it goes by the docs and by messages the admins picked. Members can ask 15 questions an hour and 50 a day, in the channels in `CHANNEL_ID`. Members with a role in `TIER1_ROLE_IDS` can ask 25 an hour and 80 a day, and members with a role in `TIER2_ROLE_IDS` 60 an hour and 200 a day, both in any channel. Server admins and members with a role in `TEAM_ROLE_IDS` have no limit. A member's hour starts with their first question and resets an hour later, and days are UTC. `/limits` shows anyone a bar for each, with when it resets.

`/ask` asks with a slash command instead. It takes a file, such as a log or a screenshot, and the tags below as options.

To have the bot answer a message that doesn't mention it, right-click the message and pick **Apps > Answer this**. Only members who can manage the server see it, and it has no limit.

The reply shows up right away with a line under it while the bot works. When it's done, the line says how long it took and which tags it used.

## Tags

Put these in a question to change how the bot answers it. `/tags` lists them for anyone.

- `[web]` lets it search the web.
- `[music]` has it recommend up to three songs, found by searching Spotify and SoundCloud. The bot checks each link before posting it and drops any that lead nowhere, and these replies show the songs' previews.
- `[rate]` has it rate an attached screenshot out of 10 and say what to change.
- `[shader]` has it write a ReShade FX shader for what you describe, attached to the reply as a `.fx` file.
- `[tldr]` has it sum up the channel or thread, reading the last 100 messages instead of 10.
- `[bug]` has it write a bug report from the conversation and `Unishade.log`, and link to a new GitHub issue with the bug report form filled in.
- `[poll]` has it turn the question into a Discord poll, posted under the reply and open for a day.
- `[touchgrass]` has it look at how much you've used the bot and stage an intervention.
- `[think]` has it reason harder. `[think medium]` picks the level: `minimal`, `low`, `medium`, `high`, `xhigh` or `max`. Plain `[think]` is `high`, and without the tag it's `low`. Any other level gets an error reply, not an answer.

`[tldr]` counts as one more question, and so do `[web]` and `[music]` when the bot searches.

## Private threads

When someone needs to share `Unishade.log`, or their problem will take some back and forth, the bot can offer a private thread with a button under its answer. Only the person who asked can press it. The thread opens where they asked when that's one of the channels in `CHANNEL_ID` or it's not set, and in the first of them otherwise. Only they and members with **Manage Threads** see it. Each member gets one open thread at a time.

In the thread, the bot answers them without a mention. It waits until they've stopped writing for 4 seconds, so a question split over several messages gets one answer. These answers don't count toward the hour or the day, and a thread gets 40 at most.

Under each answer in the thread, **Solved** closes and locks it, and **Get a human** pings the roles in `TEAM_ROLE_IDS`, which adds them to the thread. After that, the bot answers there only when someone mentions it. The person the thread is for and the team can press either.

The bot needs **Create Private Threads**, **Send Messages in Threads** and **Manage Threads** in that channel.

## Context

The bot always reads the docs in `website/src/content/docs`. To give it more, such as an announcement, right-click a message and pick **Apps > Add to context**. **Remove from context** takes it out again, and `/context` lists what's in. Only members who can manage the server see these.

With the question, the bot also reads the 10 messages before it, the message it replies to, the first message of the thread, and the two newest text files attached in all of those, such as `Unishade.log`. Pictures in the message and in the one it replies to go along too, when the model takes images.

## Feedback

With `LOG_CHANNEL_ID` set, anyone can right-click an answer and pick **Apps > Report answer**. That posts the question and the answer to the log with **Add a correction**, which opens a form for what the bot should know and adds it to the context.

When a private thread is solved, the bot sums it up in the log with **Add to context** and **Discard**. **Add to context** opens the summary in a form, so the team can fix it before it goes in. **Remove from context** on the log message takes either out again.

## Members

Right-click a member and pick **Apps > Exclude from AI** to have the bot ignore them, or **Remove limits** to treat them like the team. **Include in AI** and **Restore limits** undo these. Only members who can manage the server see them.

## Pausing

`/pause` stops the bot answering anyone, the team included, until someone runs it again or the bot restarts. While it's paused, its status shows Do Not Disturb and mentions get no reply. Only members who can manage the server see `/pause`, and Server Settings > Integrations can give it to the team's roles.

## Changing limits

`/limit-settings boost` multiplies everyone's hourly and daily limits, such as `multiplier: 2` for a 2x week. Give it `days` to have it end on its own, or set it back to 1 to end it. While it's on, `/limits` tells members. `/limit-settings set` changes the hourly or daily limit for everyone or a tier, `/limit-settings threads` changes how many answers a private thread gets, and `/limit-settings show` lists the limits. `/limit-settings reset` gives a member a full hour and day of questions again, or everyone without a member. Changes last across restarts and go to the log. Only members who can manage the server see the command.

## Log

With `LOG_CHANNEL_ID` set, every answer goes to that channel: who asked and where, the question, the answer or why it failed, the tags, the model, tokens and cost. So does who paused or unpaused the bot, opened a private thread or asked for a human.

## Usage

`/usage` shows what the OpenRouter key spent today, this week and this month, what's left of its limit and of the account's credits, and how many free model requests it made today. It also counts the bot's answers and web searches today and over the last 30 days, and lists the 5 members it answered most. Only members who can manage the server see it.

## Running it

```powershell
cd bot
pnpm install
pnpm start
```

Set `DISCORD_TOKEN` to the bot's token and `OPENROUTER_API_KEY` to an OpenRouter API key, either in the environment or in `bot/.env`. `OPENROUTER_MODEL` picks the model, `openai/gpt-oss-20b` if it's not set. List several separated by commas, and when one fails, such as a free model at its daily limit, the next one answers. `CHANNEL_ID` takes the IDs of the channels members can ask in, separated by commas, threads in them included. Without it, they can ask anywhere. `TIER1_ROLE_IDS`, `TIER2_ROLE_IDS` and `TEAM_ROLE_IDS` take role IDs separated by commas. `LOG_CHANNEL_ID` is the channel for the log. The bot also needs **Message Content Intent** and **Server Members Intent** turned on in the Discord Developer Portal.
