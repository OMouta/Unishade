// The developer docs as Markdown, for people who paste them into a chat and for agents that read them: each page at
// its own address ending in .md, /llms.txt as the list of them, and /llms-full.txt with all of them in one file.
import { getCollection, type CollectionEntry } from 'astro:content';
import header from '../../addons/unishade_addon.h?raw';
import exampleSource from '../../addons/example/example.c?raw';
import exampleManifest from '../../addons/example/addon.ini?raw';
import exampleBuild from '../../addons/example/CMakeLists.txt?raw';
import { links } from './links';

type Page = CollectionEntry<'developers'>;

export const developerPages = async () => (await getCollection('developers')).sort((a, b) => a.data.order - b.data.order);

// Where a page's Markdown is, on the site.
export const markdownPath = (page: Page) => `/developers/${page.id}.md`;

// The files an add-on is written against, which the site hands out as they are in the repository.
export const referenceFiles = [
  { path: '/developers/unishade_addon.h', name: 'unishade_addon.h', language: 'c', text: header, about: 'The whole add-on interface, a C header whose comments are the reference.' },
  { path: '/developers/example/example.c', name: 'example.c', language: 'c', text: exampleSource, about: 'The example add-on: settings, controls, drawing over the game, events and Unishade\'s functions, with no graphics code.' },
  { path: '/developers/example/addon.ini', name: 'addon.ini', language: 'ini', text: exampleManifest, about: 'The example add-on\'s addon.ini.' },
  { path: '/developers/example/CMakeLists.txt', name: 'CMakeLists.txt', language: 'cmake', text: exampleBuild, about: 'Builds the example add-on with CMake.' },
];

// A page as Markdown that stands on its own: its title and description first, and every link a whole address. Links
// to other developer pages lead to their Markdown, and [Discord](links:discord) to what links.ts has.
export const markdownOf = (page: Page, site: URL) => {
  const body = (page.body ?? '')
    .replace(/\]\(links:([A-Za-z]+)\)/g, (whole, name: string) => (Object.hasOwn(links, name) ? `](${links[name as keyof typeof links]})` : whole))
    .replace(/\]\(\/developers\/([a-z-]*)\/?(#[^)]*)?\)/g, (_, id: string, anchor = '') => `](${new URL(`/developers/${id || 'overview'}.md`, site)}${anchor})`)
    .replace(/\]\((\/[^)]*)\)/g, (_, path: string) => `](${new URL(path, site)})`);
  return `# ${page.data.title}\n\n> ${page.data.description}\n\n${body.trim()}\n`;
};

export const text = (body: string, type = 'text/plain') => new Response(body, { headers: { 'Content-Type': `${type}; charset=utf-8` } });
