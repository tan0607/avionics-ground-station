#!/usr/bin/env bash
# Compatibility entry point for existing macOS commands and Finder shortcuts.
exec "$(dirname "$0")/mac/start.command" "$@"
