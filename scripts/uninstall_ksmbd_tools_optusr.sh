#!/usr/bin/env bash

set -euo pipefail

STATE_DIR="${STATE_DIR:-/var/lib/ksmbd-tools-optusr}"
NEW_FILES_LIST="$STATE_DIR/new_files.list"
REPLACED_FILES_LIST="$STATE_DIR/replaced_files.list"
NEW_DIRS_LIST="$STATE_DIR/new_dirs.list"
BACKUP_ROOT="$STATE_DIR/backups"
INSTALL_META="$STATE_DIR/install.meta"

die() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

require_root() {
	if [[ "$(id -u)" -ne 0 ]]; then
		die "run as root (example: sudo $0)"
	fi
}

ensure_safe_abs_path() {
	local path="$1"
	if [[ "$path" != /* || "$path" == "/" ]]; then
		die "unsafe path: $path"
	fi
}

remove_new_files() {
	local path

	[[ -f "$NEW_FILES_LIST" ]] || return 0

	while IFS= read -r path; do
		[[ -n "$path" ]] || continue
		ensure_safe_abs_path "$path"
		if [[ -e "$path" || -L "$path" ]]; then
			rm -f -- "$path"
		fi
	done < "$NEW_FILES_LIST"
}

restore_replaced_files() {
	local path rel backup_path

	[[ -f "$REPLACED_FILES_LIST" ]] || return 0

	while IFS= read -r path; do
		[[ -n "$path" ]] || continue
		ensure_safe_abs_path "$path"
		rel="${path#/}"
		backup_path="$BACKUP_ROOT/$rel"

		if [[ ! -e "$backup_path" && ! -L "$backup_path" ]]; then
			printf 'WARNING: missing backup for %s\n' "$path" >&2
			continue
		fi

		mkdir -p -- "$(dirname -- "$path")"
		rm -rf -- "$path"
		cp -a -- "$backup_path" "$path"
	done < "$REPLACED_FILES_LIST"
}

remove_new_dirs() {
	local path

	[[ -f "$NEW_DIRS_LIST" ]] || return 0

	awk -F/ '{ print NF ":" $0 }' "$NEW_DIRS_LIST" | sort -t: -k1,1nr | cut -d: -f2- |
	while IFS= read -r path; do
		[[ -n "$path" ]] || continue
		ensure_safe_abs_path "$path"
		rmdir --ignore-fail-on-non-empty "$path" 2>/dev/null || true
	done
}

main() {
	require_root

	if [[ ! -f "$INSTALL_META" && ! -f "$NEW_FILES_LIST" && ! -f "$REPLACED_FILES_LIST" ]]; then
		printf 'No managed ksmbd-tools install state found at %s\n' "$STATE_DIR"
		exit 0
	fi

	remove_new_files
	restore_replaced_files
	remove_new_dirs

	rm -rf -- "$STATE_DIR"

	printf 'ksmbd-tools managed install has been removed.\n'
	printf 'Any files replaced at install time were restored from backup.\n'
	printf 'Open a new login shell to refresh PATH.\n'
}

main "$@"
