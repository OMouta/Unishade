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
  // For people who write add-ons: the header that is the whole interface, and an add-on to start from.
  addonHeader: `${github}/blob/main/src/addons/unishade_addon.h`,
  addonExample: `${github}/tree/main/src/addons/example`,
  // Unishade's own add-ons, as working code.
  addonSource: `${github}/tree/main/src/addons`,
  reshade: 'https://reshade.me',
  signpath: 'https://about.signpath.io/',
  signpathFoundation: 'https://signpath.org/',
};
