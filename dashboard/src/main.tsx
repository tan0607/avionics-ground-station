import { StrictMode } from "react"
import { createRoot } from "react-dom/client"

// Self-hosted JetBrains Mono (bundled by Vite — no CDN, works fully offline).
import "@fontsource/jetbrains-mono/400.css"
import "@fontsource/jetbrains-mono/500.css"
import "@fontsource/jetbrains-mono/600.css"
import "@fontsource/jetbrains-mono/700.css"
// uPlot's own stylesheet (axis/legend/cursor). Local dependency, not a CDN.
import "uplot/dist/uPlot.min.css"
import "./index.css"

import App from "./App.tsx"

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
