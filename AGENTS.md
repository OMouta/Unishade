# Guidelines for agents

## Rules

- Unishade stays out of the game. Never inject code, read a game's memory or put files in a game's folder.
- Windows, macOS and Linux build the same sources. A change to the platform layer covers all three, or says which one lacks it and why.
- Effects and presets behave as they do in ReShade. When unsure, do what ReShade does.
- Every download uses HTTPS, has a size limit and is checked against a hash or a list of allowed file types.
- Third-party code is downloaded at a pinned version against a hash, never copied into the repository.
- Never lose the user's settings, presets or saved games.
- Leave the version and the beta number alone.

## Working

- Build and run the tests before calling a change done. Say which systems you could not test.
- Match the code around your change. Fix warnings, don't silence them.
- Tests are plain programs with no framework. Add a case when you change what one covers.
- Prefer checks that need no window. Ask before a live run, and give it a home folder and a build folder of its own.
- Something never released can be changed outright, with no migration or fallback. What has been released keeps working.

## Writing

- Comments say why, in full plain sentences. No history, no TODOs, no em dashes, and "such as" where others write "e.g.".
- Text the user reads says what happened and what to do about it.
- A change to what the user sees changes the docs in the same piece of work.

## Commits

- Commit on the branch you are on, main included. Don't branch or push unless asked.
- Make small commits, one change each.
- The subject is one plain imperative sentence with no prefix and no period. The body says why. Release notes are made from the subjects.
