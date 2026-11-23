// The add-on interface's header, as it is in the repository.
import type { APIRoute } from 'astro';
import { referenceFiles, text } from '../../developers';

export const GET: APIRoute = () => text(referenceFiles[0].text);
