import { AwsClient } from "aws4fetch";
import { env } from "./env.ts";

// Railway buckets can't be public, so the app gets presigned links that last this long.
const linkSeconds = 60 * 60;

const aws = new AwsClient({
  accessKeyId: env.s3AccessKeyId,
  secretAccessKey: env.s3SecretAccessKey,
  region: env.s3Region,
  service: "s3",
});

function objectUrl(key: string): string {
  return `${env.s3Endpoint.protocol}//${env.s3Bucket}.${env.s3Endpoint.host}/${key}`;
}

async function send(method: string, key: string, init: RequestInit = {}): Promise<Response> {
  const response = await aws.fetch(objectUrl(key), { ...init, method });
  if (!response.ok && !(method === "DELETE" && response.status === 404))
    throw new Error(`Bucket ${method} ${key} failed: ${response.status} ${await response.text()}`);
  return response;
}

export async function putObject(key: string, body: Uint8Array<ArrayBuffer>, contentType: string) {
  await send("PUT", key, { body, headers: { "Content-Type": contentType } });
}

export async function getObject(key: string): Promise<Uint8Array<ArrayBuffer>> {
  return new Uint8Array(await (await send("GET", key)).arrayBuffer());
}

export async function deleteObjects(keys: string[]) {
  await Promise.all(keys.map((key) => send("DELETE", key)));
}

export async function signedUrl(key: string): Promise<string> {
  const url = new URL(objectUrl(key));
  url.searchParams.set("X-Amz-Expires", String(linkSeconds));
  return (await aws.sign(url.toString(), { aws: { signQuery: true } })).url;
}
