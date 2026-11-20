// The newest release, read from GitHub once per build. Publishing a release rebuilds the site, so the download
// buttons link straight to the newest files. When GitHub can't be reached, or has no release with a Setup, the
// buttons link to the Releases page instead and the build goes on.
import { links, repository } from './links';

export interface Download {
  url: string;
  // The file's real name, which may still be RobloxShadeHost's. Undefined when the link is the Releases page.
  name?: string;
  size?: number;
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

function parse(version: string) {
  const match = /^(\d+)\.(\d+)\.(\d+)$/.exec(version);
  return match ? match.slice(1).map(Number) : undefined;
}

function newer(a: number[], b: number[]) {
  const index = a.findIndex((part, i) => part !== b[i]);
  return index >= 0 && a[index] > b[index];
}

const download = (asset: Asset | undefined): Download | undefined =>
  asset && { url: asset.browser_download_url, name: asset.name, size: asset.size };

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

  // The depth and DLSS5 files are releases too, so only vX.Y.Z tags count, like the host's update check.
  let best: { release: GitHubRelease; setup: Asset; version: number[] } | undefined;
  for (const release of releases) {
    const parts = release.tag_name?.startsWith('v') ? parse(release.tag_name.slice(1)) : undefined;
    const setup = release.assets?.find((asset) => /-Setup-\d+\.\d+\.\d+\.exe$/.test(asset.name));
    if (parts && setup && !release.draft && !release.prerelease && (!best || newer(parts, best.version)))
      best = { release, setup, version: parts };
  }
  if (!best) {
    console.warn(`${repository} has no release with a Setup EXE. The download buttons link to GitHub Releases.`);
    return fallback;
  }

  const { release, setup, version } = best;
  const asset = (pattern: RegExp) => download(release.assets.find((file) => pattern.test(file.name)));
  return {
    version: version.join('.'),
    date: new Date(release.published_at),
    notes: release.html_url,
    setup: download(setup)!,
    host: asset(/^(Unishade|RobloxShadeHost)\.exe$/),
    // Releases before macOS and Linux support have neither.
    macos: asset(/^Unishade-macOS\.zip$/),
    linux: asset(/^Unishade-linux-x64\.tar\.gz$/),
  };
}

export const release = await newest();

export const megabytes = (bytes: number) => `${(bytes / 1024 / 1024).toFixed(1)} MB`;
