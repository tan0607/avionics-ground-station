/**
 * SideNav — the thin left view rail. All four views (Live, Map, Log, Set) are
 * active. The rail is controlled: the parent owns the selected view and passes
 * it down. Icons are functional lucide marks, label + glyph, no decoration.
 */
import { Activity, Map, ScrollText, Settings, type LucideIcon } from "lucide-react"
import { cn } from "@/lib/utils"

/** The dashboard's top-level views. */
export type ViewId = "live" | "map" | "log" | "settings"

interface NavItem {
  id: ViewId | null // null = not yet wired (disabled placeholder)
  label: string
  icon: LucideIcon
  disabled?: boolean
}

const ITEMS: NavItem[] = [
  { id: "live", label: "Live", icon: Activity },
  { id: "map", label: "Map", icon: Map },
  { id: "log", label: "Log", icon: ScrollText },
  { id: "settings", label: "Set", icon: Settings },
]

interface SideNavProps {
  active: ViewId
  onSelect: (view: ViewId) => void
}

export function SideNav({ active, onSelect }: SideNavProps) {
  return (
    <nav
      aria-label="Views"
      className="flex w-16 shrink-0 flex-col border-r border-hairline bg-surface"
    >
      {/* brand mark — the ascending trace + apogee, matching the favicon */}
      <div className="flex h-11 items-center justify-center border-b border-hairline">
        <svg viewBox="0 0 32 32" className="size-5" aria-hidden>
          <path
            d="M4 27 L13 9 L20 5 L28 20"
            fill="none"
            stroke="var(--data)"
            strokeWidth="2.6"
            strokeLinecap="round"
            strokeLinejoin="round"
          />
          <circle cx="20" cy="5" r="2.4" fill="var(--caution)" />
        </svg>
      </div>

      <ul className="flex flex-col">
        {ITEMS.map((item) => {
          const Icon = item.icon
          const isActive = item.id != null && item.id === active
          return (
            <li key={item.label}>
              <button
                type="button"
                disabled={item.disabled}
                aria-current={isActive ? "page" : undefined}
                title={item.disabled ? `${item.label} — later milestone` : item.label}
                onClick={item.id ? () => onSelect(item.id as ViewId) : undefined}
                className={cn(
                  "flex w-full flex-col items-center gap-1 py-3 transition-colors",
                  isActive
                    ? "bg-surface-2 text-ink"
                    : item.disabled
                      ? "cursor-not-allowed text-ink-mute/50"
                      : "text-ink-mute hover:text-ink-dim",
                )}
              >
                <Icon
                  aria-hidden
                  strokeWidth={1.75}
                  className={cn("size-[18px]", isActive && "text-data")}
                />
                <span className="text-[0.5625rem] uppercase tracking-wide">{item.label}</span>
              </button>
            </li>
          )
        })}
      </ul>
    </nav>
  )
}
