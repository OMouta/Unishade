# Unishade bot (Unibot)

The Discord bot for the Unishade server. Mention it and it answers anything, in English. For Unishade questions it goes by the docs and by messages the admins picked. Members can ask 3 questions a minute and 20 a day, in the channel in `CHANNEL_ID`. Members with a role in `TIER1_ROLE_IDS` can ask 4 a minute and 30 a day, and members with a role in `TIER2_ROLE_IDS` 5 a minute and 100 a day, both in any channel. Server admins and members with a role in `TEAM_ROLE_IDS` have no limit. Put `[web]` in a question to let the bot search the web, and an answer that searched counts as 2 questions. Days are UTC. `/limits` shows anyone their own.

To have the bot answer a message that doesn't mention it, right-click the message and pick **Apps > Answer this**. Only members who can manage the server see it, and it has no limit.

## Context

The bot always reads the docs in `website/src/content/docs`. To give it more, such as an announcement, right-click a message and pick **Apps > Add to context**. **Remove from context** takes it out again, and `/context` lists what's in. Only members who can manage the server see these.

With the question, the bot also reads the 10 messages before it, the message it replies to, the first message of the thread, and the two newest text files attached in all of those, such as `Unishade.log`. Pictures in the message and in the one it replies to go along too, when the model takes images.

## Members

Right-click a member and pick **Apps > Exclude from AI** to have the bot ignore them, or **Remove limits** to treat them like the team. **Include in AI** and **Restore limits** undo these. Only members who can manage the server see them.

## Usage

`/usage` shows what the OpenRouter key spent today, this week and this month, what's left of its limit and of the account's credits, and how many free model requests it made today. It also counts the bot's answers and web searches today and over the last 30 days, and lists the 5 members it answered most. Only members who can manage the server see it.

## Running it

```powershell
cd bot
pnpm install
pnpm start
```

Set `DISCORD_TOKEN` to the bot's token and `OPENROUTER_API_KEY` to an OpenRouter API key, either in the environment or in `bot/.env`. `OPENROUTER_MODEL` picks the model, `openai/gpt-oss-20b` if it's not set. List several separated by commas, and when one fails, such as a free model at its daily limit, the next one answers. `CHANNEL_ID` is the channel members can ask in, threads in it included. Without it, they can ask anywhere. `TIER1_ROLE_IDS`, `TIER2_ROLE_IDS` and `TEAM_ROLE_IDS` take role IDs separated by commas. The bot also needs **Message Content Intent** and **Server Members Intent** turned on in the Discord Developer Portal.
