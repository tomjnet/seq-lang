// Site-wide settings and the few helpers every page needs.

export const REPO = 'https://github.com/tomjnet/seq-lang';
export const DOCS = `${REPO}/blob/main/docs`;
export const INSTALL_URL = 'https://raw.githubusercontent.com/tomjnet/seq-lang/HEAD/install.sh';
export const VERSION = '0.1.0';

export const SITE = {
  name: 'Seq',
  title: 'The Seq Programming Language',
  slogan: 'Plain words in. Native code out.',
  description:
    'The Seq programming language turns ordered, plain-language requests into a native program: ' +
    'written by a language model at compile time, built by GCC, and run in a sandbox.',
};

// The hero video. While `src` is empty the hero shows the editor and terminal
// windows instead. To publish a video, put the files in public/media/ and set:
//
//   src: 'media/hero.mp4', poster: 'media/hero.jpg'
//
// It plays muted and looped behind the headline, covering the whole hero.
export const HERO_VIDEO = {
  src: '',
  poster: '',
};

// Prefixes a site path with the base the site is served under.
export function href(path = ''): string {
  const base = import.meta.env.BASE_URL.replace(/\/$/, '');
  return `${base}/${path.replace(/^\//, '')}`;
}

export const NAV = [
  { label: 'Why Seq', path: 'why/' },
  { label: 'Learn', path: 'learn/' },
  { label: 'Try Seq', path: '#try' },
  { label: 'Install', path: 'learn/#install' },
];
