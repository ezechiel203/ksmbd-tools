#!/usr/bin/env bash

set -euo pipefail

PREFIX="${PREFIX:-/opt/usr}"
SYSCONFDIR="${SYSCONFDIR:-$PREFIX/etc}"
RUNDIR="${RUNDIR:-/run}"
SYSTEMD_UNIT_DIR="${SYSTEMD_UNIT_DIR:-$PREFIX/lib/systemd/system}"
PATH_DROPIN="${PATH_DROPIN:-/etc/profile.d/ksmbd-tools-optusr-path.sh}"
STATE_DIR="${STATE_DIR:-/var/lib/ksmbd-tools-optusr}"
UNINSTALL_BASENAME="${UNINSTALL_BASENAME:-ksmbd-tools-uninstall-optusr}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')}"

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
UNINSTALL_SOURCE="$SCRIPT_DIR/uninstall_ksmbd_tools_optusr.sh"
UNINSTALL_TARGET="$PREFIX/sbin/$UNINSTALL_BASENAME"

INSTALL_META="$STATE_DIR/install.meta"
NEW_FILES_LIST="$STATE_DIR/new_files.list"
REPLACED_FILES_LIST="$STATE_DIR/replaced_files.list"
NEW_DIRS_LIST="$STATE_DIR/new_dirs.list"
BACKUP_ROOT="$STATE_DIR/backups"

BUILD_DIR=""
STAGE_DIR=""

die() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

require_root() {
	if [[ "$(id -u)" -ne 0 ]]; then
		die "run as root (example: sudo $0)"
	fi
}

require_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

append_unique() {
	local value="$1"
	local list_file="$2"

	if [[ ! -f "$list_file" ]] || ! grep -Fxq -- "$value" "$list_file"; then
		printf '%s\n' "$value" >> "$list_file"
	fi
}

ensure_safe_abs_path() {
	local path="$1"
	if [[ "$path" != /* || "$path" == "/" ]]; then
		die "unsafe path: $path"
	fi
}

record_new_dir_if_missing() {
	local dir_path="$1"
	ensure_safe_abs_path "$dir_path"

	if [[ -e "$dir_path" && ! -d "$dir_path" ]]; then
		die "path exists but is not a directory: $dir_path"
	fi

	if [[ ! -d "$dir_path" ]]; then
		append_unique "$dir_path" "$NEW_DIRS_LIST"
	fi
}

record_file_backup_or_new() {
	local target_path="$1"
	local rel_path backup_path

	ensure_safe_abs_path "$target_path"
	if [[ -d "$target_path" && ! -L "$target_path" ]]; then
		die "path exists as a directory, cannot overwrite with file: $target_path"
	fi
	rel_path="${target_path#/}"
	backup_path="$BACKUP_ROOT/$rel_path"

	if [[ -e "$target_path" || -L "$target_path" ]]; then
		mkdir -p -- "$(dirname -- "$backup_path")"
		cp -a -- "$target_path" "$backup_path"
		append_unique "$target_path" "$REPLACED_FILES_LIST"
	else
		append_unique "$target_path" "$NEW_FILES_LIST"
	fi
}

cleanup_tmp() {
	if [[ -n "$BUILD_DIR" && -d "$BUILD_DIR" ]]; then
		rm -rf -- "$BUILD_DIR"
	fi
	if [[ -n "$STAGE_DIR" && -d "$STAGE_DIR" ]]; then
		rm -rf -- "$STAGE_DIR"
	fi
}

prepare_build_tree() {
	require_cmd make
	require_cmd install
	require_cmd cp
	require_cmd find
	require_cmd grep
	require_cmd sort
	require_cmd awk

	if [[ ! -x "$UNINSTALL_SOURCE" ]]; then
		die "uninstall script is missing or not executable: $UNINSTALL_SOURCE"
	fi

	if [[ ! -x "$SOURCE_ROOT/configure" ]]; then
		require_cmd autoreconf
		(
			cd "$SOURCE_ROOT"
			./autogen.sh
		)
	fi

	# Support repositories that were previously configured in-tree.
	if [[ -f "$SOURCE_ROOT/config.status" ]]; then
		(
			cd "$SOURCE_ROOT"
			make distclean >/dev/null 2>&1 || true
			rm -f config.status config.log config.cache
		)
	fi

	BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ksmbd-tools-build.XXXXXX")"
	STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ksmbd-tools-stage.XXXXXX")"
}

build_and_stage_install() {
	(
		cd "$BUILD_DIR"
		"$SOURCE_ROOT/configure" \
			--prefix="$PREFIX" \
			--sysconfdir="$SYSCONFDIR" \
			--with-rundir="$RUNDIR" \
			--with-systemdsystemunitdir="$SYSTEMD_UNIT_DIR"
		make -j"$JOBS"
		make DESTDIR="$STAGE_DIR" install
	)
}

prepare_state_dir() {
	if [[ -e "$STATE_DIR" ]]; then
		die "existing state directory detected in $STATE_DIR; run uninstall first"
	fi

	mkdir -p -- "$BACKUP_ROOT"
	: > "$NEW_FILES_LIST"
	: > "$REPLACED_FILES_LIST"
	: > "$NEW_DIRS_LIST"
}

record_stage_paths() {
	local rel target

	while IFS= read -r -d '' rel; do
		target="/$rel"
		record_new_dir_if_missing "$target"
	done < <(find "$STAGE_DIR" -mindepth 1 -type d -printf '%P\0')

	while IFS= read -r -d '' rel; do
		target="/$rel"
		record_file_backup_or_new "$target"
	done < <(find "$STAGE_DIR" -mindepth 1 \( -type f -o -type l \) -printf '%P\0')

	record_new_dir_if_missing "$(dirname -- "$PATH_DROPIN")"
	record_file_backup_or_new "$PATH_DROPIN"

	record_new_dir_if_missing "$PREFIX"
	record_new_dir_if_missing "$PREFIX/sbin"
	record_file_backup_or_new "$UNINSTALL_TARGET"
}

install_payload() {
	cp -a -- "$STAGE_DIR"/. /

	install -D -m 0755 "$UNINSTALL_SOURCE" "$UNINSTALL_TARGET"

	cat > "$PATH_DROPIN" <<EOF
# Added by ksmbd-tools managed install script.
# Remove by running: sudo $UNINSTALL_TARGET
if [ -d "$PREFIX/sbin" ]; then
	case ":\${PATH:-}:" in
		*:"$PREFIX/sbin":*) ;;
		*) PATH="$PREFIX/sbin:\${PATH:-}" ;;
	esac
fi

if [ -d "$PREFIX/bin" ]; then
	case ":\${PATH:-}:" in
		*:"$PREFIX/bin":*) ;;
		*) PATH="$PREFIX/bin:\${PATH:-}" ;;
	esac
fi

export PATH
EOF
	chmod 0644 "$PATH_DROPIN"

	sort -u -o "$NEW_FILES_LIST" "$NEW_FILES_LIST"
	sort -u -o "$REPLACED_FILES_LIST" "$REPLACED_FILES_LIST"
	sort -u -o "$NEW_DIRS_LIST" "$NEW_DIRS_LIST"

	cat > "$INSTALL_META" <<EOF
PREFIX=$PREFIX
SYSCONFDIR=$SYSCONFDIR
RUNDIR=$RUNDIR
SYSTEMD_UNIT_DIR=$SYSTEMD_UNIT_DIR
PATH_DROPIN=$PATH_DROPIN
UNINSTALL_TARGET=$UNINSTALL_TARGET
INSTALLED_AT=$(date -u +'%Y-%m-%dT%H:%M:%SZ')
EOF
	chmod 0600 "$INSTALL_META"
}

main() {
	require_root
	prepare_build_tree
	trap cleanup_tmp EXIT

	build_and_stage_install
	prepare_state_dir
	record_stage_paths
	install_payload

	printf 'ksmbd-tools installed under %s\n' "$PREFIX"
	printf 'PATH drop-in installed at %s\n' "$PATH_DROPIN"
	printf 'Uninstall with: sudo %s\n' "$UNINSTALL_TARGET"
	printf 'Open a new login shell (or source %s) to use updated PATH.\n' "$PATH_DROPIN"
}

main "$@"
