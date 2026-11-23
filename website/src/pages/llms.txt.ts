// What the site has for agents, as https://llmstxt.org describes: the developer docs as Markdown, the files an add-on
// is written against, and the docs for people who use Unishade.
import type { APIRoute } from 'astro';
import { getCollection } from 'astro:content';
import { developerPages, markdownPath, referenceFiles, text } from '../developers';

export const GET: APIRoute = async ({ site }) => {
  const at = (path: string) => new URL(path, site).href;
  const developers = await developerPages();
  const docs = (await getCollection('docs')).sort((a, b) => a.data.order - b.data.order);
  return text(
    [
      '# Unishade',
      '',
      '> Unishade draws ReShade effects over a game without injecting anything into it, on Windows, macOS and Linux. Developers extend it with add-ons: a folder with an addon.ini and a native library that implements one C header.',
      '',
      'An add-on can add settings and controls to Unishade\'s menu, draw over the game, change effects and presets, react to events, and work on the game\'s picture on the graphics card. To write one, read "Your first add-on", then the page for each part you use. unishade_addon.h is the reference, and the example add-on is a complete one to start from.',
      '',
      '## Developer docs',
      '',
      ...developers.map((page) => `- [${page.data.title}](${at(markdownPath(page))}): ${page.data.description}`),
      '',
      '## Reference',
      '',
      ...referenceFiles.map((file) => `- [${file.name}](${at(file.path)}): ${file.about}`),
      `- [Everything in one file](${at('/llms-full.txt')}): Every developer page, the header and the example add-on.`,
      '',
      '## Optional',
      '',
      ...docs.map((page) => `- [${page.data.title}](${at(page.id === 'install' ? '/docs/' : `/docs/${page.id}/`)}): ${page.data.description}`),
      '',
    ].join('\n'),
  );
};
