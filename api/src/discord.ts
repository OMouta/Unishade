import { createPublicKey, verify } from "node:crypto";
import { env } from "./env.ts";

const api = "https://discord.com/api/v10";
const redirectUri = new URL("/auth/callback", env.publicUrl).toString();

async function ok(response: Response, what: string): Promise<Response> {
  if (!response.ok) throw new Error(`${what} failed: ${response.status} ${await response.text()}`);
  return response;
}

// prompt=none skips Discord's consent screen for people who signed in before.
export function authorizeUrl(state: string): string {
  const query = new URLSearchParams({
    client_id: env.discordClientId,
    response_type: "code",
    scope: "identify",
    redirect_uri: redirectUri,
    state,
    prompt: "none",
  });
  return `https://discord.com/oauth2/authorize?${query}`;
}

// The account that approved a sign-in, from the code Discord's redirect carries.
export async function signedInUser(code: string): Promise<{ id: string; name: string }> {
  const body = new URLSearchParams({
    grant_type: "authorization_code",
    code,
    redirect_uri: redirectUri,
    client_id: env.discordClientId,
    client_secret: env.discordClientSecret,
  });
  const token = (await (await ok(await fetch(`${api}/oauth2/token`, { method: "POST", body }), "Discord sign-in")).json()) as { access_token: string };
  const user = (await (
    await ok(await fetch(`${api}/users/@me`, { headers: { Authorization: `Bearer ${token.access_token}` } }), "Reading the Discord account")
  ).json()) as { id: string; username: string; global_name: string | null };
  return { id: user.id, name: user.global_name ?? user.username };
}

export async function postMessage(channelId: string, message: object, files: { name: string; data: Uint8Array<ArrayBuffer> }[]) {
  const form = new FormData();
  form.set("payload_json", JSON.stringify({ ...message, attachments: files.map((file, id) => ({ id, filename: file.name })) }));
  files.forEach((file, id) => form.set(`files[${id}]`, new Blob([file.data], { type: "image/jpeg" }), file.name));
  const request = { method: "POST", headers: { Authorization: `Bot ${env.discordBotToken}` }, body: form };
  await ok(await fetch(`${api}/channels/${channelId}/messages`, request), "Posting to Discord");
}

const publicKey = createPublicKey({
  key: { kty: "OKP", crv: "Ed25519", x: Buffer.from(env.discordPublicKey, "hex").toString("base64url") },
  format: "jwk",
});

// Discord signs every interaction, and only accepts an endpoint that turns away requests it didn't sign.
export function verifyInteraction(body: string, signature: string | undefined, timestamp: string | undefined): boolean {
  if (!signature || !/^[0-9a-f]{128}$/i.test(signature) || !timestamp) return false;
  return verify(null, Buffer.from(timestamp + body), publicKey, Buffer.from(signature, "hex"));
}
