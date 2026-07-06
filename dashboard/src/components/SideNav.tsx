/**
 * SideNav — the thin left view rail. Live is the only active view in this
 * milestone; Map / Log / Settings are disabled placeholders for later
 * milestones (the map comes from Window 3). Icons are functional lucide marks,
 * label + glyph, no decoration.
 */
import { Activity, Map, ScrollText, Settings, type LucideIcon } from "lucide-react"
import { cn } from "@/lib/utils"

interface NavItem {
  label: string
  icon: LucideIcon
  active?: boolean
  disabled?: boolean
}

const ITEMS: NavItem[] = [
  { label: "Live", icon: Activity, active: true },
  { label: "Map", icon: Map, disabled: true },
  { label: "Log", icon: ScrollText, disabled: true },
  { label: "Set", icon: Settings, disabled: true },
]

export function SideNav() {
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
          return (
            <li key={item.label}>
              <button
                type="button"
                disabled={item.disabled}
                aria-current={item.active ? "page" : undefined}
                title={item.disabled ? `${item.label} — later milestone` : item.label}
                className={cn(
                  "flex w-full flex-col items-center gap-1 py-3 transition-colors",
                  item.active
                    ? "bg-surface-2 text-ink"
                    : item.disabled
                      ? "cursor-not-allowed text-ink-mute/50"
                      : "text-ink-mute hover:text-ink-dim",
                )}
              >
                <Icon
                  aria-hidden
                  strokeWidth={1.75}
                  className={cn("size-[18px]", item.active && "text-data")}
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
