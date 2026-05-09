#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${1:-}" == "-check" || "${1:-}" == "--check" ]]; then
  header_version=$(awk -F'"' '/^#define TEALET_VERSION "[0-9]+\.[0-9]+\.[0-9]+"/ {print $2; exit}' src/tealet.h)
  if [[ -z "$header_version" ]]; then
    echo "ERROR: Could not parse TEALET_VERSION from src/tealet.h"
    exit 1
  fi

  make_version=$(awk -F'[[:space:]]*=[[:space:]]*' '/^VERSION[[:space:]]*=/ {print $2; exit}' Makefile)
  readme_version=$(sed -n 's/^\*\*Version \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)\*\*$/\1/p' README.md | head -n 1)
  doxy_version=$(awk -F'[[:space:]]*=[[:space:]]*' '/^PROJECT_NUMBER[[:space:]]*=/ {print $2; exit}' Doxyfile)

  if [[ "$make_version" != "$header_version" ]]; then
    echo "ERROR: Makefile VERSION ($make_version) != src/tealet.h TEALET_VERSION ($header_version)"
    exit 1
  fi
  if [[ "$readme_version" != "$header_version" ]]; then
    echo "ERROR: README.md version ($readme_version) != src/tealet.h TEALET_VERSION ($header_version)"
    exit 1
  fi
  if [[ "$doxy_version" != "$header_version" ]]; then
    echo "ERROR: Doxyfile PROJECT_NUMBER ($doxy_version) != src/tealet.h TEALET_VERSION ($header_version)"
    exit 1
  fi

  echo "*** Version sync OK: $header_version ***"
  exit 0
fi

version=$(awk -F'"' '/^#define TEALET_VERSION "[0-9]+\.[0-9]+\.[0-9]+"/ {print $2; exit}' src/tealet.h)
if [[ -z "$version" ]]; then
  echo "ERROR: Could not parse TEALET_VERSION from src/tealet.h"
  exit 1
fi

awk -v v="$version" 'BEGIN{done=0} { if (!done && $0 ~ /^VERSION[[:space:]]*=/) { print "VERSION = " v; done=1; next } print }' Makefile > Makefile.new
mv Makefile.new Makefile

awk -v v="$version" 'BEGIN{done=0} { if (!done && $0 ~ /^\*\*Version [0-9]+\.[0-9]+\.[0-9]+\*\*$/) { print "**Version " v "**"; done=1; next } print }' README.md > README.md.new
mv README.md.new README.md

awk -v v="$version" 'BEGIN{done=0} { if (!done && $0 ~ /^PROJECT_NUMBER[[:space:]]*=/) { print "PROJECT_NUMBER         = " v; done=1; next } print }' Doxyfile > Doxyfile.new
mv Doxyfile.new Doxyfile

"$repo_root/scripts/sync-version.sh" -check
