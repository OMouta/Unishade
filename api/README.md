# Unishade API

The backend for sharing presets from the menu. It runs on Railway with Postgres and a bucket.

## Running it

```powershell
cd api
pnpm install
pnpm db:push
pnpm start
```

`pnpm db:push` applies `src/schema.ts` straight to the database. There are no migration files yet, so run it against Railway's database before deploying a schema change. `pnpm test` runs the tests on an in-memory Postgres, with Discord, Roblox and the bucket faked.

Settings go in the environment or in `api/.env`:

- `DATABASE_URL` is the Postgres connection string.
- `PUBLIC_URL` is where the API is reachable, such as `https://api.example.com`. Discord sends sign-ins back to `PUBLIC_URL/auth/callback`.
- `DISCORD_CLIENT_ID`, `DISCORD_CLIENT_SECRET`, `DISCORD_PUBLIC_KEY` and `DISCORD_BOT_TOKEN` come from the API's Discord application.
- `REVIEW_CHANNEL_ID` is the channel new presets and reports go to.
- `TEAM_ROLE_IDS` takes role IDs separated by commas. Members with one of them can review presets, and so can server admins.
- `S3_ENDPOINT`, `S3_REGION`, `S3_BUCKET`, `S3_ACCESS_KEY_ID` and `S3_SECRET_ACCESS_KEY` point at the bucket. On Railway, reference the bucket's `ENDPOINT`, `REGION`, `BUCKET`, `ACCESS_KEY_ID` and `SECRET_ACCESS_KEY`.

On Railway, set the service's root directory to `api`. It builds from the Dockerfile.

## Discord application

The API needs a Discord application of its own. The bot's application gets interactions over the gateway, and an application with an interactions endpoint only gets them over HTTP. People see the application's name when they sign in.

1. Under **OAuth2**, add `PUBLIC_URL/auth/callback` as a redirect.
2. Under **General Information**, set **Interactions Endpoint URL** to `PUBLIC_URL/discord/interactions`. Discord tests the endpoint when you save, so the API has to be running.
3. Invite the application's bot to the server. It needs to send messages and attach files in the review channel.

## Endpoints

Signed-in requests send `Authorization: Bearer <token>`. Errors come back as `{ "error": "..." }` with a message written for the person using the menu. Image links expire after an hour.

| Endpoint | |
|---|---|
| `GET /games/resolve?executable=&place=` | The game for an executable name, and for Roblox the experience a place ID belongs to |
| `GET /presets?game=&effect=&sort=popular\|new&offset=` | Approved presets, 30 at a time, each with a `thumbnail` link |
| `GET /presets/:id` | One approved preset with its ini and `before` and `after` links |
| `POST /presets/:id/saves` | Counts someone keeping a preset, once a day for each address |
| `POST /auth/start` | Returns `{ url, poll }`. Open `url` in the browser |
| `POST /auth/poll` | Send `{ poll }`. Answers 202 while waiting, then `{ token, user }` once, then 410 |
| `POST /auth/logout` | |
| `GET /me` | Signed in |
| `POST /presets` | Signed in. A form, described below |
| `GET /me/presets` | Signed in. Your presets with their status and reject reason |
| `DELETE /presets/:id` | Signed in. Deletes one of your presets |
| `POST /presets/:id/reports` | Signed in. Send `{ reason }` |
| `POST /discord/interactions` | Button clicks and forms from the review channel |

`POST /presets` takes these fields:

- `name`, and `description` if there is one.
- `executable`, the game's file name. `gameName` names the game when nobody has published for it yet.
- `place` to publish for a Roblox experience instead of all of Roblox.
- `ini`, the preset's text. Every technique has to name its file, as in `CAS@CAS.fx`.
- `needsDepth` set to `true` when the preset needs depth estimation.
- `dlss5`, when DLSS5 is on: a JSON object with the keys from `[RENODX-DLSS-preset1]` in ReShade.ini as strings. The API only takes the ones that change the look, listed in `src/ini.ts`.
- `before` and `after`, JPEG or PNG screenshots of the same size.
