#!/usr/bin/env bash
# Manage the single zstreamer source version in VERSION.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="${ROOT_DIR}/VERSION"

usage() {
    cat <<'EOF'
Usage: ./scripts/version.sh <command> [value]

Commands:
  get                     Print the current MAJOR.MINOR.PATCH version.
  tag                     Print the release tag for the current version.
  set <MAJOR.MINOR.PATCH> Replace VERSION with an explicit release version.
  bump <major|minor|patch> Increment one version component.
  check-tag <vX.Y.Z>      Fail unless the tag matches VERSION exactly.
EOF
}

read_version() {
    local version
    version="$(tr -d '[:space:]' < "${VERSION_FILE}")"
    if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "Invalid VERSION '${version}'; expected MAJOR.MINOR.PATCH" >&2
        exit 1
    fi
    printf '%s\n' "${version}"
}

write_version() {
    local version="$1"
    if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "Invalid version '${version}'; expected MAJOR.MINOR.PATCH" >&2
        exit 1
    fi
    printf '%s\n' "${version}" > "${VERSION_FILE}"
}

case "${1:-}" in
    get)
        [[ "$#" -eq 1 ]] || { usage >&2; exit 1; }
        read_version
        ;;
    tag)
        [[ "$#" -eq 1 ]] || { usage >&2; exit 1; }
        printf 'v%s\n' "$(read_version)"
        ;;
    set)
        [[ "$#" -eq 2 ]] || { usage >&2; exit 1; }
        write_version "$2"
        ;;
    bump)
        [[ "$#" -eq 2 ]] || { usage >&2; exit 1; }
        IFS=. read -r major minor patch <<< "$(read_version)"
        case "$2" in
            major) major=$((major + 1)); minor=0; patch=0 ;;
            minor) minor=$((minor + 1)); patch=0 ;;
            patch) patch=$((patch + 1)) ;;
            *) echo "Unknown bump type '$2'; expected major, minor, or patch" >&2; exit 1 ;;
        esac
        write_version "${major}.${minor}.${patch}"
        read_version
        ;;
    check-tag)
        [[ "$#" -eq 2 ]] || { usage >&2; exit 1; }
        expected="v$(read_version)"
        if [[ "$2" != "${expected}" ]]; then
            echo "Release tag '$2' does not match VERSION '${expected}'" >&2
            exit 1
        fi
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
