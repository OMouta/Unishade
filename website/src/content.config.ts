import { defineCollection } from 'astro:content';
import { glob } from 'astro/loaders';
import { z } from 'astro/zod';

// Each file is a page under /docs/, listed in the sidebar by order. install.md is /docs/ itself.
const docs = defineCollection({
  loader: glob({ pattern: '*.md', base: './src/content/docs' }),
  schema: z.object({
    title: z.string(),
    description: z.string(),
    order: z.number(),
  }),
});

// The same for people who write add-ons, under /developers/. overview.md is /developers/ itself.
const developers = defineCollection({
  loader: glob({ pattern: '*.md', base: './src/content/developers' }),
  schema: z.object({
    title: z.string(),
    description: z.string(),
    order: z.number(),
  }),
});

export const collections = { docs, developers };
