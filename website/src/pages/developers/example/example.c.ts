// The example add-on's example.c, as it is in the repository.
import type { APIRoute } from 'astro';
import { referenceFiles, text } from '../../../developers';

export const GET: APIRoute = () => text(referenceFiles.find((file) => file.name === 'example.c')!.text);
