import { defineConfig } from 'vitest/config';
import { fileURLToPath } from 'node:url';

export default defineConfig({
  resolve: {
    alias: {
      // Resolve workspace packages to their sources so tests run against the
      // code being edited without a build step in between.
      '@cad/core': fileURLToPath(new URL('./packages/core/src/index.ts', import.meta.url)),
      '@cad/io': fileURLToPath(new URL('./packages/io/src/index.ts', import.meta.url)),
      '@cad/render': fileURLToPath(new URL('./packages/render/src/index.ts', import.meta.url)),
    },
  },
  test: {
    include: ['packages/*/src/**/*.test.ts'],
    environment: 'node',
    // Geometry assertions compare floating-point values; failures are far
    // easier to diagnose with the full received value printed.
    chaiConfig: { truncateThreshold: 0 },
  },
});
