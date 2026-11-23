import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// The tests' config, apart from the app's vite.config.ts (#548): jsdom gives the hooks and panels a DOM, and each test
// sits beside the code it tests under src/, where tsc checks it with the app's own code
export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.{ts,tsx}'],
    setupFiles: ['src/test/setup.ts'],
  },
});
