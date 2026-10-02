# The Seq website

The website of the Seq programming language, built with [Astro](https://astro.build). It is a static site: three pages, no client framework, and fonts served from the site itself.

```sh
npm install
npm run dev
```

`npm run dev` serves the site at `http://localhost:4321`. `npm run build` writes it to `dist/`. Node 22.12 or newer is needed.

## Layout

| Path | Content |
| --- | --- |
| `src/pages/` | `index.astro` (home), `why.astro`, `learn.astro`, `404.astro`. |
| `src/components/` | The hero, the "Try Seq" playground, the navigation, and the smaller parts. |
| `src/data/examples.ts` | The workflows and console sessions that the hero and the playground show. |
| `src/data/logo.json` | The wordmark as vector paths, traced from `logo/logo-seq.png`. |
| `src/assets/sparrow.png` | The mascot, cut out of `logo/logo-seq-cyber-Sparrow.png`. |
| `src/styles/global.css` | Colours, type, and buttons. The colours are the logo's. |
| `src/site.ts` | Repository links, the navigation, and the hero video setting. |

## The hero video

The hero shows an editor window and a terminal window until there is a video. To publish one, put the files in `public/media/` and set `HERO_VIDEO` in `src/site.ts`:

```ts
export const HERO_VIDEO = {
  src: 'media/hero.mp4',
  poster: 'media/hero.jpg',
};
```

The video then plays muted and looped behind the headline and covers the whole hero; the windows are not rendered. The headline sits on the left, so keep the subject of the video to the right.

## The playground

`seqc` needs GCC and a local model, so nothing is compiled in the browser. "Try Seq" replays the sessions in `src/data/examples.ts`. They follow the console format of `src/cli/build.cpp`; timings, sizes, and identifiers are illustrative. If the compiler's output changes, change them with it.

## Publishing

`.github/workflows/pages.yml` builds and deploys the site to GitHub Pages on every push to `main` that touches `website/`. The workflow passes the site address and base path to the build, so links work both under `https://<owner>.github.io/<repo>/` and under a custom domain. Internal links go through `href()` in `src/site.ts` for the same reason.
