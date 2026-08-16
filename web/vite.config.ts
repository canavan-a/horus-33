import path from 'path'
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// Dev-only proxying so the browser never needs CORS handling and the app
// always talks to relative paths — exactly what it will do in production
// behind nginx (see the plan's M10). Swap the targets here if horus-server or
// MediaMTX run on different ports locally.
export default defineConfig({
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: {
      '@': path.resolve(import.meta.dirname, './src'),
    },
  },
  server: {
    proxy: {
      '/api': { target: 'http://127.0.0.1:8090', ws: true },
      // Public shape is /whep/<path> (matches nginx's eventual mapping in
      // M10); MediaMTX itself expects /<path>/whep, so this rewrites between
      // the two rather than making every caller know MediaMTX's convention.
      '/whep': {
        target: 'http://127.0.0.1:8889',
        changeOrigin: true,
        rewrite: (path) => {
          // "/whep/eye" -> ["", "whep", "eye"]; skip the leading empty
          // segment *and* the "whep" prefix itself to get at the real name.
          const [, , name, ...rest] = path.split('/')
          return `/${name}/whep${rest.length ? '/' + rest.join('/') : ''}`
        },
      },
    },
  },
})
