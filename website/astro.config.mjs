// @ts-check
import { defineConfig } from 'astro/config';

// The Pages workflow passes --site and --base, so the same build works under
// https://<owner>.github.io/<repo>/ and under a custom domain. Internal links
// go through href() in src/site.ts, never a bare "/path".
export default defineConfig({
  server: { port: 4321 },
  devToolbar: { enabled: false },
});
