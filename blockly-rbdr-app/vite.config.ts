import { defineConfig } from 'vite';

export default defineConfig({
  server: {
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8000',
        ws: true,
      },
      '/poll': 'http://127.0.0.1:8000',
      '/events': 'http://127.0.0.1:8000',
    },
  },
});
