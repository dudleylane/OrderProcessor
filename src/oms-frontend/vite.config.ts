import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// The app connects to /ws on the host and port it was loaded from (#157). The dev and preview servers pass that on to
// the server on its default port, as the image's nginx passes it to the server's container.
const wsProxy = {
  '/ws': { target: 'ws://localhost:8080', ws: true },
};

export default defineConfig({
  plugins: [react()],
  server: {
    port: 3000,
    proxy: wsProxy,
  },
  preview: {
    port: 3000,
    proxy: wsProxy,
  },
});
