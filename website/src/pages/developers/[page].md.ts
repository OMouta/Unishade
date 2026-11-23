// Each developer page as Markdown, such as /developers/settings.md.
import type { APIRoute, GetStaticPaths } from 'astro';
import { developerPages, markdownOf, text } from '../../developers';

export const getStaticPaths: GetStaticPaths = async () => (await developerPages()).map((page) => ({ params: { page: page.id }, props: { page } }));

export const GET: APIRoute = ({ props, site }) => text(markdownOf(props.page, site!), 'text/markdown');
