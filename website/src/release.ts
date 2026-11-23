// The newest release, read from GitHub once per build. Publishing a release rebuilds the site, so the download
// buttons link straight to the newest files. When GitHub can't be reached, or has no release with a Setup, the
// buttons link to the Releases page instead and the build goes on. A beta of the version after it, when one is out,
// comes along for the download page's banner.
import cmake from '../../CMakeLists.txt?raw';
import { links, repository } from './links';

export interface Download {
  url: string;
  // The file's real name, which may still be RobloxShadeHost's. Undefined when the link is the Releases page.
  name?: string;
  size?: number;
}

// A beta of the version after the newest release, such as 0.7.0-beta.2.
export interface Beta {
  version: string;
  notes: string;
  setup: Download;
  macos?: Download;
  linux?: Download;
}

export interface Release {
  // Undefined when there's no release to link to.
  version?: string;
  date?: Date;
  // The release's page, or the Releases page.
  notes: string;
  setup: Download;
  host?: Download;
  macos?: Download;
  linux?: Download;
  // The newest beta, unless the version it leads to is out.
  beta?: Beta;
  // The version the site describes, from CMakeLists.txt, while it's newer than the release. The docs say so.
  upcoming?: string;
}

interface Asset {
  name: string;
  size: number;
  browser_download_url: string;
}

interface GitHubRelease {
  tag_name: string;
  draft: boolean;
  prerelease: boolean;
  published_at: string;
  html_url: string;
  assets: Asset[];
}

// GitHub Actions sets GITHUB_API_URL. Pointing it at a host that doesn't answer shows the site as it builds offline.
const api = process.env.GITHUB_API_URL || 'https://api.github.com';

// A version's three numbers, then which beta of it this is, as in 0.7.0-beta.2. A version itself counts as
// Infinity there, so it comes after its betas.
function parse(version: string) {
  const match = /^(\d+)\.(\d+)\.(\d+)(?:-beta\.([1-9]\d*))?$/.exec(version);
  return match ? [Number(match[1]), Number(match[2]), Number(match[3]), match[4] ? Number(match[4]) : Infinity] : undefined;
}

function newer(a: number[], b: number[]) {
  const index = a.findIndex((part, i) => part !== b[i]);
  return index >= 0 && a[index] > b[index];
}

const download = (asset: Asset | undefined): Download | undefined =>
  asset && { url: asset.browser_download_url, name: asset.name, size: asset.size };

// The version being built, from project(Unishade VERSION x.y.z).
const project = /project\(Unishade VERSION (\d+\.\d+\.\d+)/.exec(cmake)?.[1];

async function list() {
  const headers: Record<string, string> = { Accept: 'application/vnd.github+json' };
  // Without a token GitHub allows 60 requests an hour, which local builds rarely reach.
  if (process.env.GITHUB_TOKEN) headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
  const response = await fetch(`${api}/repos/${repository}/releases?per_page=30`, {
    headers,
    signal: AbortSignal.timeout(15_000),
  });
  if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
  const releases = await response.json();
  if (!Array.isArray(releases)) throw new Error('the answer is not a list of releases');
  return releases as GitHubRelease[];
}

async function newest(): Promise<Release> {
  const fallback: Release = { notes: links.releases, setup: { url: links.releases } };
  let releases: GitHubRelease[];
  try {
    releases = await list();
  } catch (error) {
    const cause = error instanceof Error && error.cause instanceof Error ? `: ${error.cause.message}` : '';
    const reason = `${error instanceof Error ? error.message : error}${cause}`;
    console.warn(`Could not read the releases of ${repository} (${reason}). The download buttons link to GitHub Releases.`);
    return fallback;
  }

  // The depth and DLSS5 files are releases too, so only vX.Y.Z and vX.Y.Z-beta.N tags count, like the host's update
  // check. A tag that names a beta makes the release one, whatever its prerelease flag says.
  type Found = { release: GitHubRelease; setup: Asset; version: number[] };
  let best: Found | undefined;
  let beta: Found | undefined;
  for (const release of releases) {
    const parts = release.tag_name?.startsWith('v') ? parse(release.tag_name.slice(1)) : undefined;
    const setup = release.assets?.find((asset) => /-Setup-\d+\.\d+\.\d+(-beta\.\d+)?\.exe$/.test(asset.name));
    if (!parts || !setup || release.draft) continue;
    if (parts[3] !== Infinity) {
      if (!beta || newer(parts, beta.version)) beta = { release, setup, version: parts };
    } else if (!release.prerelease && (!best || newer(parts, best.version))) best = { release, setup, version: parts };
  }
  if (!best) {
    console.warn(`${repository} has no release with a Setup EXE. The download buttons link to GitHub Releases.`);
    return fallback;
  }

  const files = (release: GitHubRelease) => {
    const asset = (pattern: RegExp) => download(release.assets.find((file) => pattern.test(file.name)));
    // Releases before macOS and Linux support have neither.
    return { macos: asset(/^Unishade-macOS\.zip$/), linux: asset(/^Unishade-linux-x64\.tar\.gz$/) };
  };
  const { release, setup, version } = best;
  const target = project ? parse(project) : undefined;
  return {
    version: version.slice(0, 3).join('.'),
    date: new Date(release.published_at),
    notes: release.html_url,
    setup: download(setup)!,
    host: download(release.assets.find((file) => /^(Unishade|RobloxShadeHost)\.exe$/.test(file.name))),
    ...files(release),
    // Once its version is out, a beta is older than the newest release.
    beta:
      beta && newer(beta.version, version)
        ? { version: beta.release.tag_name.slice(1), notes: beta.release.html_url, setup: download(beta.setup)!, ...files(beta.release) }
        : undefined,
    upcoming: target && newer(target, version) ? project : undefined,
  };
}

export const release = await newest();

// A beta of the version the site describes is out, so its pictures of the app are that beta's.
export const inBeta = Boolean(release.upcoming && release.beta?.version.startsWith(`${release.upcoming}-beta.`));

export const megabytes = (bytes: number) => `${(bytes / 1024 / 1024).toFixed(1)} MB`;
