import assert from "node:assert/strict";
import { generateKeyPairSync, sign } from "node:crypto";
import { test } from "node:test";
import { PGlite } from "@electric-sql/pglite";
import { generateDrizzleJson, generateMigration } from "drizzle-kit/api";
import { drizzle } from "drizzle-orm/pglite";
import sharp from "sharp";

// The API against PGlite, with Discord, Roblox and the bucket answered by the fetch below.

const discordKeys = generateKeyPairSync("ed25519");
Object.assign(process.env, {
  DATABASE_URL: "unused",
  PUBLIC_URL: "https://api.test",
  DISCORD_CLIENT_ID: "client",
  DISCORD_CLIENT_SECRET: "secret",
  DISCORD_PUBLIC_KEY: Buffer.from(discordKeys.publicKey.export({ format: "jwk" }).x ?? "", "base64url").toString("hex"),
  DISCORD_BOT_TOKEN: "bot",
  REVIEW_CHANNEL_ID: "review",
  TEAM_ROLE_IDS: "team",
  S3_ENDPOINT: "https://bucket.test",
  S3_REGION: "auto",
  S3_BUCKET: "presets",
  S3_ACCESS_KEY_ID: "key",
  S3_SECRET_ACCESS_KEY: "secret",
});

type Message = {
  channel: string;
  files: string[];
  embeds: { title: string; fields: { name: string; value: string }[] }[];
  components: { components: { custom_id: string }[] }[];
};
const objects = new Map<string, Uint8Array>();
const messages: Message[] = [];
const discordUsers: Record<string, { id: string; username: string; global_name: string }> = {
  tiago: { id: "100", username: "tiago", global_name: "Tiago" },
  alice: { id: "200", username: "alice", global_name: "Alice" },
};

globalThis.fetch = async (input, init) => {
  const request = new Request(input, init);
  const url = new URL(request.url);
  if (url.host === "presets.bucket.test") {
    const key = url.pathname.slice(1);
    if (request.method === "PUT") objects.set(key, new Uint8Array(await request.arrayBuffer()));
    if (request.method === "DELETE") objects.delete(key);
    if (request.method !== "GET") return new Response(null, { status: 200 });
    const object = objects.get(key);
    return object ? new Response(object) : new Response("NoSuchKey", { status: 404 });
  }
  // The code is the access token is the account.
  if (url.href === "https://discord.com/api/v10/oauth2/token")
    return Response.json({ access_token: new URLSearchParams(await request.text()).get("code") });
  if (url.href === "https://discord.com/api/v10/users/@me")
    return Response.json(discordUsers[request.headers.get("authorization")?.replace("Bearer ", "") ?? ""]);
  const channel = url.pathname.match(/^\/api\/v10\/channels\/(\w+)\/messages$/);
  if (channel) {
    const form = await request.formData();
    const files = [...form.keys()].filter((key) => key.startsWith("files["));
    messages.push({ channel: channel[1], files, ...JSON.parse(String(form.get("payload_json"))) });
    return Response.json({ id: "message" });
  }
  if (url.host === "apis.roblox.com") return Response.json({ universeId: url.pathname === "/universes/v1/places/606849621/universe" ? 245662005 : null });
  if (url.href === "https://games.roblox.com/v1/games?universeIds=245662005") return Response.json({ data: [{ id: 245662005, name: "Jailbreak" }] });
  throw new Error(`Unexpected fetch: ${request.method} ${url}`);
};

const schema = await import("../src/schema.ts");
const { createApp } = await import("../src/app.ts");
const { ensureRoblox } = await import("../src/games.ts");
const { dlss5Settings, presetEffects } = await import("../src/ini.ts");

const client = new PGlite();
const db = drizzle(client);
const empty = generateDrizzleJson({});
for (const statement of await generateMigration(empty, generateDrizzleJson(schema)))
  await client.exec(statement);
await ensureRoblox(db);
const app = createApp(db);

const screenshot = await sharp({ create: { width: 1280, height: 720, channels: 3, background: "#336699" } }).png().toBuffer();
const ini = "Techniques=CAS@CAS.fx,Bloom@BloomingHDR.fx\n\n[CAS.fx]\nSharpening=0.5\n";
const moderator = { user: { username: "mod", global_name: "Mod" }, roles: ["team"], permissions: "0" };

async function signIn(account: string): Promise<string> {
  const start = await (await app.request("/auth/start", { method: "POST" })).json();
  const poll = () => app.request("/auth/poll", { method: "POST", body: JSON.stringify({ poll: start.poll }), headers: { "Content-Type": "application/json" } });
  assert.equal((await poll()).status, 202);
  const state = new URL(start.url).searchParams.get("state");
  assert.equal((await app.request(`/auth/callback?code=${account}&state=${state}`)).status, 200);
  const { token } = await (await poll()).json();
  assert.equal((await poll()).status, 410, "a sign-in gives out one session");
  return token;
}

function publish(token: string, fields: Record<string, string>) {
  const form = new FormData();
  for (const [key, value] of Object.entries({ name: "Crisp", executable: "RobloxPlayerBeta.exe", ini, ...fields })) form.set(key, value);
  form.set("before", new Blob([screenshot]), "before.png");
  form.set("after", new Blob([screenshot]), "after.png");
  return app.request("/presets", { method: "POST", body: form, headers: { Authorization: `Bearer ${token}` } });
}

function interact(body: object) {
  const text = JSON.stringify(body);
  const timestamp = String(Math.floor(Date.now() / 1000));
  const signature = sign(null, Buffer.from(timestamp + text), discordKeys.privateKey).toString("hex");
  return app.request("/discord/interactions", {
    method: "POST",
    body: text,
    headers: { "x-signature-ed25519": signature, "x-signature-timestamp": timestamp },
  });
}

function rejectWith(action: "reject" | "ban", id: number, reason: string) {
  return interact({ type: 5, member: moderator, data: { custom_id: `${action}:${id}`, components: [{ type: 18, component: { type: 4, custom_id: "reason", value: reason } }] } });
}

test("effects come from Techniques, each file once", () => {
  assert.deepEqual(presetEffects("Techniques=A@CAS.fx,B@cas.FX, C@FXAA.fx\r\n[CAS.fx]\nTechniques=X@Other.fx"), ["CAS.fx", "FXAA.fx"]);
  assert.throws(() => presetEffects("Techniques=A@..\\..\\evil.fx"));
  assert.throws(() => presetEffects("Techniques=A"));
  assert.throws(() => presetEffects("[CAS.fx]\nSharpening=1"));
});

test("only DLSS5 look settings can be shared", () => {
  assert.deepEqual(dlss5Settings('{"DirectNeuralRenderingStyle":"2","DirectNeuralRenderingIntensity":"0.8"}'), {
    DirectNeuralRenderingStyle: "2",
    DirectNeuralRenderingIntensity: "0.8",
  });
  assert.throws(() => dlss5Settings('{"DirectNeuralRenderingRequireDlss":"1"}'));
  assert.throws(() => dlss5Settings('{"DirectNeuralRenderingStyle":"high"}'));
});

test("resolve finds Roblox experiences by place", async () => {
  const known = await (await app.request("/games/resolve?executable=RobloxPlayerBeta.exe&place=606849621")).json();
  assert.equal(known.game.name, "Roblox");
  assert.equal(known.experience.name, "Jailbreak");
  const unknownPlace = await (await app.request("/games/resolve?executable=RobloxPlayerBeta.exe&place=1")).json();
  assert.equal(unknownPlace.experience, null);
  assert.deepEqual(await (await app.request("/games/resolve?executable=Game.exe")).json(), { game: null, experience: null });
});

test("a preset goes through review, gets reported and rejected", async () => {
  const tiago = await signIn("tiago");
  const published = await publish(tiago, {
    place: "606849621",
    description: "Sharper and brighter",
    dlss5: '{"DirectNeuralRenderingStyle":"2"}',
    needsDepth: "true",
  });
  assert.equal(published.status, 201, await published.clone().text());
  const { id } = await published.json();

  const review = messages.at(-1);
  assert.equal(review?.channel, "review");
  assert.deepEqual(review?.files, ["files[0]", "files[1]"]);
  assert.equal(review?.embeds[0].fields.find((field) => field.name === "Game")?.value, "Jailbreak (Roblox)");
  assert.equal(review?.embeds[0].fields.find((field) => field.name === "DLSS5")?.value, "Style 2");
  assert.deepEqual(review?.components[0].components.map((button) => button.custom_id), [`approve:${id}`, `reject:${id}`, `ban:${id}`]);
  assert.equal(objects.size, 3);

  const { experience } = await (await app.request("/games/resolve?executable=RobloxPlayerBeta.exe&place=606849621")).json();
  assert.equal((await (await app.request(`/presets?game=${experience.id}`)).json()).presets.length, 0, "pending presets aren't listed");

  const notTeam = await interact({ type: 3, member: { ...moderator, roles: [] }, data: { custom_id: `approve:${id}` } });
  assert.equal((await notTeam.json()).data.flags, 64);
  const approved = await (await interact({ type: 3, member: moderator, data: { custom_id: `approve:${id}` } })).json();
  assert.deepEqual(approved.data.components, []);
  assert.equal(approved.data.content, "Approved by Mod");

  const list = await (await app.request(`/presets?game=${experience.id}&effect=CAS.fx`)).json();
  assert.equal(list.presets.length, 1);
  assert.equal(list.presets[0].author, "Tiago");
  assert.deepEqual(list.presets[0].effects, ["CAS.fx", "BloomingHDR.fx"]);
  assert.match(list.presets[0].thumbnail, /X-Amz-Signature=/);
  assert.equal(list.presets[0].ini, undefined, "lists leave out the ini");
  assert.equal((await (await app.request(`/presets?game=${experience.id}&effect=FXAA.fx`)).json()).presets.length, 0);

  const detail = await (await app.request(`/presets/${id}`)).json();
  assert.equal(detail.ini, ini.trim());
  assert.deepEqual(detail.game, { id: experience.id, name: "Jailbreak" });

  await app.request(`/presets/${id}/saves`, { method: "POST" });
  await app.request(`/presets/${id}/saves`, { method: "POST" });
  assert.equal((await (await app.request(`/presets/${id}`)).json()).saves, 1, "one save a day for each address");

  const alice = await signIn("alice");
  const report = () =>
    app.request(`/presets/${id}/reports`, {
      method: "POST",
      body: JSON.stringify({ reason: "Flashing lights" }),
      headers: { Authorization: `Bearer ${alice}`, "Content-Type": "application/json" },
    });
  const posted = messages.length;
  assert.equal((await report()).status, 204);
  assert.equal((await report()).status, 204);
  assert.equal(messages.length, posted + 1, "reporting twice posts once");
  assert.equal(messages.at(-1)?.embeds[0].fields[0].value, "Flashing lights");

  const modal = await (await interact({ type: 3, member: moderator, data: { custom_id: `reject:${id}` } })).json();
  assert.equal(modal.type, 9);
  const rejected = await (await rejectWith("reject", id, "Too bright")).json();
  assert.equal(rejected.data.content, "Rejected by Mod: Too bright");
  assert.equal(objects.size, 0, "rejecting deletes the screenshots");
  assert.equal((await app.request(`/presets/${id}`)).status, 404);

  const mine = await (await app.request("/me/presets", { headers: { Authorization: `Bearer ${tiago}` } })).json();
  assert.deepEqual(
    mine.presets.map(({ status, rejectReason, thumbnail }: { status: string; rejectReason: string; thumbnail: string | null }) => ({ status, rejectReason, thumbnail })),
    [{ status: "rejected", rejectReason: "Too bright", thumbnail: null }],
  );
});

test("publishing names new games and checks what it's sent", async () => {
  const tiago = await signIn("tiago");
  assert.equal((await publish(tiago, { executable: "C:\\Games\\Cool.exe" })).status, 400, "a new game needs a name");
  const published = await publish(tiago, { executable: "C:\\Games\\Cool.exe", gameName: "Cool Game" });
  assert.equal(published.status, 201);
  assert.equal(messages.at(-1)?.embeds[0].fields[0].value, "Cool Game");
  assert.equal((await (await app.request("/games/resolve?executable=cool.exe")).json()).game.name, "Cool Game");

  assert.equal((await publish(tiago, { executable: "cool.exe", place: "606849621" })).status, 400, "only Roblox has experiences");
  assert.equal((await publish(tiago, { dlss5: '{"DirectNeuralRenderingHookPoint":"4"}' })).status, 400);
  assert.equal((await publish(tiago, { ini: "Techniques=A@C:\\evil.fx" })).status, 400);
  assert.equal((await app.request("/presets", { method: "POST", body: new FormData() })).status, 401);
});

test("banning stops publishing", async () => {
  const alice = await signIn("alice");
  const { id } = await (await publish(alice, {})).json();
  const banned = await (await rejectWith("ban", id, "")).json();
  assert.equal(banned.data.content, "Rejected and banned by Mod");
  assert.equal((await publish(alice, {})).status, 403);
});

test("unsigned interactions are turned away", async () => {
  const response = await app.request("/discord/interactions", {
    method: "POST",
    body: JSON.stringify({ type: 1 }),
    headers: { "x-signature-ed25519": "0".repeat(128), "x-signature-timestamp": "1" },
  });
  assert.equal(response.status, 401);
  assert.deepEqual(await (await interact({ type: 1 })).json(), { type: 1 });
});
