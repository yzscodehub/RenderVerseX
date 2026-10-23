#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
cd "$script_dir"

if ! command -v pwsh >/dev/null 2>&1; then
  echo "PowerShell 7 (pwsh) is required by the RenderVerseX harness." >&2
  exit 1
fi

exec pwsh -NoProfile -ExecutionPolicy Bypass -File "$script_dir/init.ps1" "$@"
