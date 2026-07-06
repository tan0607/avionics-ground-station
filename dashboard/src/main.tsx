import { StrictMode } from "react"
import { createRoot } from "react-dom/client"

// Self-hosted Geist (shadcn/Vercel family), variable weight axis — bundled by
// Vite, no CDN, fully offline. Geist Sans = chrome/labels, Geist Mono = data.
import "@fontsource-variable/geist/wght.css"
import "@fontsource-variable/geist-mono/wght.css"
// uPlot's own stylesheet (axis/legend/cursor). Local dependency, not a CDN.
import "uplot/dist/uPlot.min.css"
import "./index.css"

import App from "./App.tsx"

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
