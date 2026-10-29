#!/usr/bin/env bash
#
# Write the .flatpakref of NeverTube from a REAL OSTree repository.
#
# A .flatpakref is the one-file installer: `flatpak install --user <url>` adds
# the remote that carries the key and installs the app from it. It has to name
# the app, the BRANCH that exists in the repository, the URL and the key that
# signs the repository's summary; if any of those is a guess the file is a
# broken installer, so nothing here is hardcoded:
#
#   - The branch is READ from the refs of the repository that was just signed
#     (`app/<id>/<arch>/<branch>`). This repository publishes `master` today
#     because the manifest does not set `branch:`, but that is a fact to read,
#     not to assume: changing the manifest must not need a second edit here.
#   - The ref is also required to be in the SUMMARY, which is the file flatpak
#     actually downloads over HTTP. A ref in refs/heads that the summary does
#     not carry is invisible to the client.
#   - The key is the one exported by GPG_KEY, and its fingerprint is checked
#     again from the GPGKey that ends up inside the file.
#
# Every check fails the script (and, in CI, the publish job): publishing an
# installer that points at a branch nobody has is worse than publishing none.
#
# Usage:
#   GPG_KEY=<key id> packaging/linux/flatpak/make-flatpakref.sh REPO_DIR OUT_DIR
#
# Env (defaults are the official ones):
#   APP_ID, REPO_URL, RUNTIME_REPO, TITLE, COMMENT, HOMEPAGE, REMOTE_NAME

set -euo pipefail

die() {
  echo "make-flatpakref: $*" >&2
  exit 1
}

[ "$#" -eq 2 ] || {
  # The header of this file IS the usage text: comment lines up to the first
  # line of code, with the leading '# ' removed.
  awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
  exit 2
}
REPO="$1"
OUT_DIR="$2"

APP_ID="${APP_ID:-io.github.marlo4220mc.NeverTube}"
REPO_URL="${REPO_URL:-https://marlo4220mc.github.io/NeverTube/flatpak/repo}"
RUNTIME_REPO="${RUNTIME_REPO:-https://dl.flathub.org/repo/flathub.flatpakrepo}"
TITLE="${TITLE:-NeverTube}"
COMMENT="${COMMENT:-Lightweight YouTube client (Qt5 + libmpv)}"
HOMEPAGE="${HOMEPAGE:-https://github.com/marlo4220mc/NeverTube}"
REMOTE_NAME="${REMOTE_NAME:-nevertube}"
GPG_KEY="${GPG_KEY:-${NEVERTUBE_GPG_KEY:-}}"

[ -d "$REPO" ] || die "$REPO is not a directory"
[ -f "$REPO/config" ] || die "$REPO does not look like an OSTree repository (no config)"
[ -s "$REPO/summary" ] || die "$REPO has no summary (flatpak build-update-repo was not run?)"
[ -n "$GPG_KEY" ] || die "GPG_KEY is not set: the .flatpakref cannot carry a key nobody signed with"

for tool in gpg base64 awk sort python3; do
  command -v "$tool" >/dev/null || die "$tool is missing"
done

# The refs of the repository. `ostree refs` is the authoritative reader; the
# fallback walks refs/heads, which is the same information on disk (a local
# archive-z2 repo, which is what flatpak-builder leaves behind).
if command -v ostree >/dev/null 2>&1; then
  refs="$(ostree --repo="$REPO" refs)"
else
  echo "make-flatpakref: ostree is not installed, reading $REPO/refs/heads" >&2
  refs="$(cd "$REPO/refs/heads" && find . \( -type f -o -type l \) -print | sed 's|^\./||' | sort)"
fi

app_refs="$(printf '%s\n' "$refs" |
  awk -F/ -v id="$APP_ID" '$1 == "app" && $2 == id && NF == 4 {print}' | sort)"
[ -n "$app_refs" ] || die "no app/$APP_ID/<arch>/<branch> ref in $REPO (refs: $(printf '%s' "$refs" | tr '\n' ' '))"

# One branch: a .flatpakref names ONE branch and no architecture, so a repo
# that carries the app on two branches cannot be described by one file.
branches="$(printf '%s\n' "$app_refs" | awk -F/ '{print $4}' | sort -u)"
n_branches="$(printf '%s\n' "$branches" | grep -c . || true)"
if [ "$n_branches" -ne 1 ]; then
  die "$APP_ID is published on several branches ($(printf '%s' "$branches" | tr '\n' ' ')); a .flatpakref carries exactly one and guessing it would install the wrong one"
fi
branch="$branches"
arches="$(printf '%s\n' "$app_refs" | awk -F/ '{print $3}' | sort -u)"

# The ref has to be in the summary: that is the file a flatpak client reads
# over HTTP, and a ref that is only in refs/heads does not exist for it.
while IFS= read -r ref; do
  [ -n "$ref" ] || continue
  grep -a -q -F "$ref" "$REPO/summary" ||
    die "$ref is not in $REPO/summary: flatpak would not see it"
done <<EOF
$app_refs
EOF

key_b64="$(gpg --batch --export "$GPG_KEY" | base64 -w0)"
[ -n "$key_b64" ] || die "gpg cannot export $GPG_KEY"
key_fpr="$(gpg --batch --with-colons --list-keys "$GPG_KEY" |
  awk -F: '/^fpr:/{print $10; exit}')"
[ -n "$key_fpr" ] || die "cannot read the fingerprint of $GPG_KEY"

mkdir -p "$OUT_DIR"
out="$OUT_DIR/$APP_ID.flatpakref"
# The key order is the one of the man page (flatpakref(5)); Version first so a
# human reading the file sees the format version before anything else.
{
  echo '[Flatpak Ref]'
  echo 'Version=1'
  echo "Name=$APP_ID"
  echo "Branch=$branch"
  echo "Title=$TITLE"
  echo "Comment=$COMMENT"
  echo "Homepage=$HOMEPAGE"
  echo "Url=$REPO_URL"
  echo "RuntimeRepo=$RUNTIME_REPO"
  echo 'IsRuntime=false'
  echo "GPGKey=$key_b64"
  echo "SuggestRemoteName=$REMOTE_NAME"
} >"$out"
chmod 0644 "$out"

# --- validation of what was just written ------------------------------------
# Syntax and fields, with the same .ini reading flatpak uses (GKeyFile). An
# unknown or misspelled key is an error here and not a field flatpak silently
# ignores.
python3 - "$out" "$APP_ID" "$branch" "$REPO_URL" "$RUNTIME_REPO" "$TITLE" \
  "$HOMEPAGE" "$REMOTE_NAME" <<'PY'
import configparser
import sys

path, app_id, branch, url, runtime_repo, title, homepage, remote = sys.argv[1:9]

parser = configparser.RawConfigParser(strict=True)
parser.optionxform = str  # GKeyFile keys are case-sensitive
with open(path, encoding="utf-8") as fh:
    parser.read_file(fh)

if parser.sections() != ["Flatpak Ref"]:
    sys.exit(f"{path}: expected exactly the [Flatpak Ref] group, found {parser.sections()}")

ref = parser["Flatpak Ref"]
allowed = {
    "Version", "Name", "Url", "Branch", "Title", "Comment", "Description",
    "Icon", "Homepage", "DeploySideloadCollectionID", "DeployCollectionID",
    "CollectionID", "IsRuntime", "GPGKey", "SignatureLookaside",
    "RuntimeRepo", "SuggestRemoteName",
}
unknown = sorted(set(ref) - allowed)
if unknown:
    sys.exit(f"{path}: keys flatpak does not know: {', '.join(unknown)}")

expected = {
    "Version": "1",
    "Name": app_id,
    "Branch": branch,
    "Url": url,
    "RuntimeRepo": runtime_repo,
    "IsRuntime": "false",
    "Title": title,
    "Homepage": homepage,
    "SuggestRemoteName": remote,
}
for key, value in expected.items():
    got = ref.get(key)
    if got != value:
        sys.exit(f"{path}: {key}={got!r}, expected {value!r}")

for key in ("GPGKey", "Comment"):
    if not ref.get(key, "").strip():
        sys.exit(f"{path}: {key} is empty")

if not ref["Homepage"].startswith("https://"):
    sys.exit(f"{path}: Homepage is not an https URL")
print(f"make-flatpakref: {path} is a valid [Flatpak Ref] (Branch={branch})")
PY

# The GPGKey in the file (base64, binary or armored) must decode to the key
# that signs the repository. This catches the copy/paste and the export that
# looks right but carries another key.
tmp_key="$(mktemp)"
trap 'rm -f "$tmp_key"' EXIT
if ! sed -n 's/^GPGKey=//p' "$out" | base64 -d >"$tmp_key" 2>/dev/null; then
  die "GPGKey in $out is not valid base64"
fi
[ -s "$tmp_key" ] || die "GPGKey in $out decodes to nothing"
file_fpr="$(gpg --batch --with-colons --show-keys "$tmp_key" |
  awk -F: '/^fpr:/{print $10; exit}')"
[ "$file_fpr" = "$key_fpr" ] ||
  die "GPGKey in $out is $file_fpr, the repository is signed with $key_fpr"

echo "make-flatpakref: $out"
echo "make-flatpakref: app $APP_ID, branch $branch, arch $(printf '%s' "$arches" | tr '\n' ' ')"
echo "make-flatpakref: key $key_fpr"
echo "make-flatpakref: install with: flatpak install --user <url>/$APP_ID.flatpakref"
