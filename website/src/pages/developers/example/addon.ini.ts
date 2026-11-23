// The example add-on's addon.ini, as it is in the repository.
import type { APIRoute } from 'astro';
import { referenceFiles, text } from '../../../developers';

export const GET: APIRoute = () => text(referenceFiles.find((file) => file.name === 'addon.ini')!.text);
