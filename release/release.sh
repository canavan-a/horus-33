#!/usr/bin/env bash
#
# release.sh — guided mobile APK release.
#
# Sources the local signing material in release/keystore.env, shows the existing
# mobile-v* git tags, prompts for a new tag + versionCode, runs the real Android
# release build (full toolchain output), then optionally tags, pushes, and
# publishes a GitHub Release with the signed APK attached.
#
# Mirrors .github/workflows/mobile-release.yml step-for-step; the only
# intentional difference is versionCode comes from a prompt, not the CI run
# number.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MOBILE_DIR="$REPO_ROOT/mobile"
MOBILE_ANDROID="$MOBILE_DIR/android"
ENV_FILE="$SCRIPT_DIR/keystore.env"
KEYSTORE_FILE="$SCRIPT_DIR/release.keystore"
APK_BUILT="$MOBILE_ANDROID/app/build/outputs/apk/release/app-release.apk"

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m%s\033[0m\n' "$*"; }
warn()  { printf '\033[33m%s\033[0m\n' "$*" >&2; }
die()   { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

confirm() {
	local prompt="$1" reply
	read -r -p "$prompt [y/N] " reply
	[[ "$reply" == [yY] || "$reply" == [yY][eE][sS] ]]
}

# ---------------------------------------------------------------------------
# 1. Preflight
# ---------------------------------------------------------------------------
bold "==> Preflight"

for bin in git java gh; do
	command -v "$bin" >/dev/null 2>&1 || die "'$bin' not found on PATH"
done
[[ -x "$MOBILE_ANDROID/gradlew" ]] || die "missing $MOBILE_ANDROID/gradlew"
[[ -f "$ENV_FILE" ]]              || die "missing $ENV_FILE (see release/keystore.env comments)"
[[ -f "$KEYSTORE_FILE" ]]         || die "missing $KEYSTORE_FILE"
git -C "$REPO_ROOT" rev-parse --git-dir >/dev/null 2>&1 || die "not a git repo: $REPO_ROOT"

info "toolchain ok  ($(java -version 2>&1 | head -1))"

# ---------------------------------------------------------------------------
# 2. Load signing env
# ---------------------------------------------------------------------------
bold "==> Signing material"

set -a
# shellcheck disable=SC1090
. "$ENV_FILE"
set +a

# The env file carries an absolute path that may be stale on another machine.
export HORUS_KEYSTORE_FILE="$KEYSTORE_FILE"

placeholder_re='^(|\.\.\.|CHANGME|CHANGEME|fill.*|<.*>)$'
for var in HORUS_KEYSTORE_PASSWORD HORUS_KEY_ALIAS HORUS_KEY_PASSWORD; do
	val="${!var:-}"
	if [[ -z "$val" || "$val" =~ $placeholder_re ]]; then
		die "$var is unset/placeholder in $ENV_FILE — fill it in from the GitHub Actions secrets (HORUS_KEYSTORE_PASSWORD / HORUS_KEY_ALIAS / HORUS_KEY_PASSWORD)"
	fi
done
info "keystore: $HORUS_KEYSTORE_FILE  alias: $HORUS_KEY_ALIAS"

# ---------------------------------------------------------------------------
# 3. Show existing tags
# ---------------------------------------------------------------------------
bold "==> Existing mobile-v* tags"

git -C "$REPO_ROOT" fetch --tags --quiet 2>/dev/null || warn "could not fetch tags (offline?) — showing local only"

mapfile -t TAGS < <(git -C "$REPO_ROOT" tag --sort=-v:refname -l 'mobile-v*')
if [[ ${#TAGS[@]} -eq 0 ]]; then
	warn "no mobile-v* tags yet"
	LATEST_TAG=""
else
	LATEST_TAG="${TAGS[0]}"
	for i in "${!TAGS[@]}"; do
		if [[ $i -eq 0 ]]; then
			printf '  %s  \033[32m<- latest\033[0m\n' "${TAGS[$i]}"
		else
			printf '  %s\n' "${TAGS[$i]}"
		fi
	done
	if command -v fzf >/dev/null 2>&1; then
		if confirm "Browse tags in fzf?"; then
			printf '%s\n' "${TAGS[@]}" | fzf --prompt='tag> ' --height=40% --reverse || true
		fi
	fi
fi

# ---------------------------------------------------------------------------
# 4. Prompt for new tag + versionCode
# ---------------------------------------------------------------------------
bold "==> New release"

suggest_tag=""
if [[ -n "$LATEST_TAG" && "$LATEST_TAG" =~ ^mobile-v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
	suggest_tag="mobile-v${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.$((BASH_REMATCH[3] + 1))"
fi

while :; do
	read -r -p "New release tag${suggest_tag:+ [$suggest_tag]}: " TAG
	TAG="${TAG:-$suggest_tag}"
	[[ -n "$TAG" ]] || { warn "tag required"; continue; }
	if [[ ! "$TAG" =~ ^mobile-v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
		warn "tag must look like mobile-v1.2.3"; continue
	fi
	if git -C "$REPO_ROOT" rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
		warn "tag $TAG already exists locally"; continue
	fi
	if git -C "$REPO_ROOT" ls-remote --exit-code --tags origin "$TAG" >/dev/null 2>&1; then
		warn "tag $TAG already exists on origin"; continue
	fi
	break
done

versionName="${TAG#mobile-v}"
suggest_code=$(( ${#TAGS[@]} + 1 ))

while :; do
	read -r -p "versionCode [$suggest_code]: " versionCode
	versionCode="${versionCode:-$suggest_code}"
	[[ "$versionCode" =~ ^[0-9]+$ ]] && break
	warn "versionCode must be an integer"
done

# Signing cert fingerprint for the summary (best effort).
cert_sha1="$(keytool -list -v \
	-keystore "$HORUS_KEYSTORE_FILE" \
	-storepass "$HORUS_KEYSTORE_PASSWORD" \
	-alias "$HORUS_KEY_ALIAS" 2>/dev/null | grep -m1 'SHA1:' | sed 's/.*SHA1: //')"

print_summary() {
	bold "-------- release summary --------"
	printf '  tag          %s\n' "$TAG"
	printf '  versionName  %s\n' "$versionName"
	printf '  versionCode  %s\n' "$versionCode"
	printf '  keystore     %s\n' "$HORUS_KEYSTORE_FILE"
	printf '  cert SHA1    %s\n' "${cert_sha1:-<unavailable>}"
	printf '  HEAD         %s\n' "$(git -C "$REPO_ROOT" rev-parse --short HEAD)"
	bold "--------------------------------"
}
print_summary

confirm "Proceed with build?" || { info "aborted."; exit 0; }

# ---------------------------------------------------------------------------
# 5. Build
# ---------------------------------------------------------------------------
bold "==> Building signed release APK"

if [[ ! -d "$MOBILE_DIR/node_modules" ]]; then
	info "node_modules missing — running npm ci"
	( cd "$MOBILE_DIR" && npm ci )
fi

(
	cd "$MOBILE_ANDROID"
	./gradlew --no-daemon assembleRelease \
		-PversionName="$versionName" \
		-PversionCode="$versionCode"
)

# ---------------------------------------------------------------------------
# 6. Verify artifact
# ---------------------------------------------------------------------------
bold "==> Artifact"

[[ -f "$APK_BUILT" ]] || die "expected APK not found at $APK_BUILT"

APK_OUT="$SCRIPT_DIR/horus-$TAG.apk"
cp "$APK_BUILT" "$APK_OUT"
info "copied to $APK_OUT ($(du -h "$APK_OUT" | cut -f1))"

if command -v apksigner >/dev/null 2>&1; then
	apksigner verify --print-certs "$APK_OUT" || warn "apksigner verify reported problems"
else
	warn "apksigner not on PATH — skipping signature verification"
	unzip -l "$APK_OUT" | tail -n +1 | grep -qE 'META-INF/.*\.(RSA|EC|DSA)|META-INF/MANIFEST.MF' \
		&& info "APK contains a signature block" \
		|| warn "no signature block visible in APK"
fi

# ---------------------------------------------------------------------------
# 7. Tag + publish
# ---------------------------------------------------------------------------
bold "==> Publish"
print_summary

if ! confirm "Tag $TAG, push it to origin, and publish the GitHub Release?"; then
	info "not published. Signed APK is at: $APK_OUT"
	exit 0
fi

git -C "$REPO_ROOT" tag -a "$TAG" -m "Mobile release $versionName"
git -C "$REPO_ROOT" push origin "$TAG"

if ! gh release create "$TAG" "$APK_OUT" \
	--repo "$(git -C "$REPO_ROOT" remote get-url origin)" \
	--title "$TAG" \
	--notes "Mobile release $versionName (versionCode $versionCode)"; then
	warn "gh release create failed — the tag $TAG is pushed; retry with:"
	warn "  gh release create $TAG \"$APK_OUT\" --title \"$TAG\" --notes \"Mobile release $versionName (versionCode $versionCode)\""
	exit 1
fi

url="$(gh release view "$TAG" --json url -q .url 2>/dev/null || true)"
bold "==> Done"
info "${url:-release published}"
