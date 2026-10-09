// The GitHub repository. GitHub Actions sets GITHUB_REPOSITORY, so a fork's build links to the fork.
export const repository = process.env.GITHUB_REPOSITORY || 'OMouta/Unishade';

const github = `https://github.com/${repository}`;

// Markdown pages use these too, written as [Discord](links:discord).
export const links = {
  discord: 'https://discord.gg/wVbVUdENas',
  kofi: 'https://ko-fi.com/omouta',
  github,
  releases: `${github}/releases`,
  license: `${github}/blob/main/LICENSE`,
  // How to build the macOS and Linux version.
  build: `${github}/tree/main/src/posix#building`,
  reshade: 'https://reshade.me',
  signpath: 'https://about.signpath.io/',
  signpathFoundation: 'https://signpath.org/',
};
