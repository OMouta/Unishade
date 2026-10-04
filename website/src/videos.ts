// YouTube videos on the home page, the first one larger. Each thumbnail is saved as src/assets/videos/<id>.jpg, from
// https://i.ytimg.com/vi/<id>/maxresdefault.jpg, so the page loads nothing from YouTube.
import type { ImageMetadata } from 'astro';

const thumbnails = import.meta.glob<ImageMetadata>('./assets/videos/*.jpg', { eager: true, import: 'default' });

const list = [
  { id: '0i35823ve88', title: 'I tested DLSS 5 on ROBLOX', channel: 'OMB Gaming' },
  { id: '2DedjEkzW8o', title: 'Testing DLSS 5 on ROBLOX Car Games!', channel: 'Acssendh' },
  { id: '0mOYo3YKA8g', title: "I Tried DLSS 5 In Train Sim... AND IT'S INSANE!", channel: 'Bunny Originals' },
  { id: 'Pmvnnb-2Fk4', title: 'Roblox with DLSS 5 is insane!', channel: 'Dema' },
  { id: 'YWjBbvQguJ8', title: 'I tested DLSS 5 on Driving Empire!', channel: 'Acssendh' },
  { id: 'x3u9adboTMs', title: 'THIS MAKES ROBLOX LOOK LIKE REAL LIFE...', channel: 'th3c0nnman' },
  { id: 'aAO8q3l4Ja4', title: 'Roblox Has NEVER Been This Realistic Before! (DLSS 5)', channel: 'RP master' },
  { id: 'g-CNS8FwvlM', title: "I tried DLSS 5 on Roblox (It's Insane)", channel: 'In_na' },
  { id: 'JzCwvp49uWE', title: 'DLSS 5 on Roblox Greenville - Hmm… it’s weird! (Greenville Roblox)', channel: 'Car Boi' },
  { id: 'Dt6zG__hZJI', title: 'I Tested DLSS 5 on Greenville Roblox (And It Looks Insane...)', channel: 'Blubber' },
  { id: 'NA6BK3sc-VE', title: 'This Is What DLSS 5 Looks On ROBLOX!', channel: 'JoJewyd' },
];

export const videos = list.map((video) => {
  const thumbnail = thumbnails[`./assets/videos/${video.id}.jpg`];
  if (!thumbnail) throw new Error(`src/assets/videos/${video.id}.jpg is missing. Save the video's thumbnail there.`);
  return { ...video, url: `https://www.youtube.com/watch?v=${video.id}`, thumbnail };
});
