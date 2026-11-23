// Every developer page, the header and the example add-on in one file, for an agent that takes it all at once.
import type { APIRoute } from 'astro';
import { developerPages, markdownOf, referenceFiles, text } from '../developers';

export const GET: APIRoute = async ({ site }) => {
  const pages = (await developerPages()).map((page) => markdownOf(page, site!));
  const files = referenceFiles.map((file) => `# ${file.name}\n\n> ${file.about}\n\n\`\`\`${file.language}\n${file.text.trimEnd()}\n\`\`\`\n`);
  return text([...pages, ...files].join('\n---\n\n'));
};
