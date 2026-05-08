#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${1:-}" == "-check" || "${1:-}" == "--check" ]]; then
  if ! grep -q '^## \[Unreleased\]$' CHANGELOG.md; then
    echo "ERROR: CHANGELOG.md missing '## [Unreleased]' section"
    exit 1
  fi

  if ! grep -q '^\[Unreleased\]: .*compare/v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\.\.\.HEAD$' CHANGELOG.md; then
    echo "ERROR: CHANGELOG.md missing canonical [Unreleased] compare footer link"
    exit 1
  fi

  echo "*** Changelog roll prerequisites OK ***"
  exit 0
fi

roll_version="${1:-}"
roll_date="${2:-}"

if [[ -z "$roll_version" ]]; then
  roll_version=$(awk -F'[[:space:]]*=[[:space:]]*' '/^VERSION[[:space:]]*=/ {print $2; exit}' Makefile)
fi
if [[ -z "$roll_date" ]]; then
  roll_date=$(date +%F)
fi

if ! [[ "$roll_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "ERROR: ROLL_VERSION must be x.y.z, got '$roll_version'"
  exit 1
fi
if ! [[ "$roll_date" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
  echo "ERROR: ROLL_DATE must be YYYY-MM-DD, got '$roll_date'"
  exit 1
fi

if grep -q "^## \[$roll_version\]" CHANGELOG.md; then
  echo "ERROR: CHANGELOG.md already has section for $roll_version"
  exit 1
fi
if grep -q "^\[$roll_version\]: " CHANGELOG.md; then
  echo "ERROR: CHANGELOG.md already has footer link for $roll_version"
  exit 1
fi

prev=$(sed -n 's@^\[Unreleased\]: .*compare/v\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)\.\.\.HEAD$@\1@p' CHANGELOG.md | head -n 1)
if [[ -z "$prev" ]]; then
  echo "ERROR: Could not parse previous release version from [Unreleased] footer link"
  exit 1
fi

awk -v v="$roll_version" -v d="$roll_date" -v p="$prev" '
BEGIN { inserted = 0; links = 0 }
{
  if (!inserted && $0 == "## [Unreleased]") {
    print $0;
    print "";
    print "## [" v "] - " d;
    inserted = 1;
    next;
  }
  if ($0 ~ /^\[Unreleased\]: /) {
    print "[Unreleased]: https://github.com/kristjanvalur/libtealet/compare/v" v "...HEAD";
    print "[" v "]: https://github.com/kristjanvalur/libtealet/compare/v" p "...v" v;
    links = 1;
    next;
  }
  print $0;
}
END {
  if (!inserted) {
    print "ERROR: Unreleased section insertion point not found" > "/dev/stderr";
    exit 1;
  }
  if (!links) {
    print "ERROR: Unreleased footer link insertion point not found" > "/dev/stderr";
    exit 1;
  }
}' CHANGELOG.md > CHANGELOG.md.new
mv CHANGELOG.md.new CHANGELOG.md

echo "*** Rolled CHANGELOG.md to $roll_version ($roll_date), previous base $prev ***"
