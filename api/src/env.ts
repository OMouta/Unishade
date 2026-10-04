function required(name: string): string {
  const value = process.env[name];
  if (!value) throw new Error(`${name} is not set`);
  return value;
}

export const env = {
  databaseUrl: required("DATABASE_URL"),
  // Where the API is reachable from the internet, for Discord's sign-in redirect.
  publicUrl: required("PUBLIC_URL"),
  discordClientId: required("DISCORD_CLIENT_ID"),
  discordClientSecret: required("DISCORD_CLIENT_SECRET"),
  discordPublicKey: required("DISCORD_PUBLIC_KEY"),
  discordBotToken: required("DISCORD_BOT_TOKEN"),
  reviewChannelId: required("REVIEW_CHANNEL_ID"),
  teamRoleIds: (process.env.TEAM_ROLE_IDS ?? "").split(",").map((id) => id.trim()).filter(Boolean),
  s3Endpoint: new URL(required("S3_ENDPOINT")),
  s3Region: required("S3_REGION"),
  s3Bucket: required("S3_BUCKET"),
  s3AccessKeyId: required("S3_ACCESS_KEY_ID"),
  s3SecretAccessKey: required("S3_SECRET_ACCESS_KEY"),
};
