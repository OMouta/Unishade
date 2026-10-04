import { HTTPException } from "hono/http-exception";

// Checks on what the app sends from its active preset. Errors are messages for the person publishing.

const invalid = (message: string) => new HTTPException(400, { message });

// An effect file name, such as CAS.fx, with nothing that could make it a path.
const effectFile = /^[^\\/:*?"<>|]+\.fx$/i;

// The effect files a preset turns on, in order and each once, the way the app's PresetEffectFiles reads
// them: from Techniques=Name@File.fx,... before the first section. Names compare without case.
export function presetEffects(ini: string): string[] {
  let techniques: string | undefined;
  for (const raw of ini.replace(/^\uFEFF/, "").split("\n")) {
    const line = raw.trim();
    if (line.startsWith("[")) break;
    const equals = line.indexOf("=");
    if (equals !== -1 && line.slice(0, equals).trim().toLowerCase() === "techniques") techniques = line.slice(equals + 1);
  }

  const effects: string[] = [];
  for (const technique of (techniques ?? "").split(",")) {
    if (!technique.trim()) continue;
    const at = technique.indexOf("@");
    const file = at === -1 ? "" : technique.slice(at + 1).trim();
    if (!effectFile.test(file)) throw invalid(`"${technique.trim()}" isn't an effect from a file. Save the preset in Unishade and try again.`);
    if (!effects.some((known) => known.toLowerCase() === file.toLowerCase())) effects.push(file);
  }
  if (!effects.length) throw invalid("The preset has no effects turned on.");
  return effects;
}

// The look settings RenoDX's DLSS5 add-on keeps in [RENODX-DLSS-preset1]. Its other settings are about how it
// runs on that PC, such as RequireDlss, which would leave DLSS5 waiting forever in Unishade.
export const dlss5Keys = [
  "DirectNeuralRenderingStyle",
  "DirectNeuralRenderingIntensity",
  "DirectNeuralRenderingLocalToneStrength",
  "DirectNeuralRenderingLocalStructureStrength",
  "DirectNeuralRenderingSkinStructureStrength",
  "DirectNeuralRenderingGlobalToneStrength",
  "DirectNeuralRenderingAutoMask",
  "DirectNeuralRenderingPassCount",
];

export function dlss5Settings(json: string): Record<string, string> {
  let parsed: unknown;
  try {
    parsed = JSON.parse(json);
  } catch {
    throw invalid("The DLSS5 settings aren't valid JSON.");
  }
  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) throw invalid("The DLSS5 settings must be an object.");
  const settings: Record<string, string> = {};
  for (const [key, value] of Object.entries(parsed)) {
    if (!dlss5Keys.includes(key)) throw invalid(`${key} can't be shared.`);
    if (typeof value !== "string" || !/^-?\d+(\.\d+)?$/.test(value)) throw invalid(`${key} must be a number.`);
    settings[key] = value;
  }
  if (!Object.keys(settings).length) throw invalid("The DLSS5 settings are empty.");
  return settings;
}
