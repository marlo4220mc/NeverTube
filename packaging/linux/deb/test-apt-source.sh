#!/usr/bin/env bash
#
# Isolated test of the two maintainer scripts of the .deb (postinst/postrm).
#
# It runs them against throwaway roots (NEVERTUBE_ROOT / NEVERTUBE_OS_RELEASE,
# the test hooks of the scripts) so the behaviour can be checked on ONE machine
# for every supported and unsupported system:
#
#   Ubuntu 22.04 -> jammy        Ubuntu 24.04 -> noble
#   Ubuntu 20.04, Debian 12, Fedora -> warning, no source written
#   Mint 21 (ID_LIKE=ubuntu, UBUNTU_CODENAME=jammy) -> jammy
#
# and the things that would be silent bugs: reinstall and upgrade writing the
# file twice, postrm deleting the source during an UPGRADE, a file of somebody
# else being overwritten or deleted, a stale source left behind on an
# unsupported system, a missing keyring, apt/network calls from a postinst.
#
# It is offline and needs no package built: `bash test-apt-source.sh`.
# `packaging/linux/deb/build.sh` runs it as part of the packaging gate.

set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
POSTINST="$here/postinst"
POSTRM="$here/postrm"

LIST=/etc/apt/sources.list.d/nevertube.list
KEYRING=/usr/share/keyrings/nevertube-archive-keyring.gpg
KEY_FPR=D22DB9BBE7F4EF8FDD13CC9805A7DDE36DCDD33F
URL=https://marlo4220mc.github.io/NeverTube/apt/
MARKER='# nevertube: written by the nevertube package; removed when it is uninstalled'
EXPECTED_DEB_LINE="deb [signed-by=$KEYRING] $URL SUITE main"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

ok=0
bad=0
pass() { ok=$((ok + 1)); printf 'ok   %s\n' "$*"; }
fail() {
  bad=$((bad + 1))
  printf 'FAIL %s\n' "$*"
}
expect_eq() { # name expected actual
  if [ "$2" = "$3" ]; then
    pass "$1"
  else
    fail "$1"
    printf '       expected: %s\n       actual:   %s\n' "$2" "$3"
  fi
}
expect_contains() { # name needle haystack
  case "$3" in
    *"$2"*) pass "$1" ;;
    *)
      fail "$1"
      printf '       missing: %s\n       in:      %s\n' "$2" "$3"
      ;;
  esac
}
expect_missing() { # name needle haystack
  case "$3" in
    *"$2"*) fail "$1 (found: $2)" ;;
    *) pass "$1" ;;
  esac
}
expect_absent() { # name path
  if [ -e "$2" ]; then fail "$1 (it exists: $2)"; else pass "$1"; fi
}
expect_present() { # name path
  if [ -e "$2" ]; then pass "$1"; else fail "$1 (it does not exist: $2)"; fi
}

# new_root NAME "os-release line" ["os-release line"...] -> prints the root
new_root() {
  name="$1"
  shift
  r="$TMP/$name"
  rm -rf "$r"
  mkdir -p "$r/etc/apt/sources.list.d" "$r/usr/share/keyrings"
  printf 'placeholder keyring\n' >"$r$KEYRING"
  : >"$r/os-release"
  for line in "$@"; do printf '%s\n' "$line" >>"$r/os-release"; done
  printf '%s' "$r"
}

# run ROOT SCRIPT [ACTION] -> RC, OUT, ERR
RC=0
OUT=""
ERR=""
run() {
  RC=0
  NEVERTUBE_ROOT="$1" NEVERTUBE_OS_RELEASE="$1/os-release" \
    sh "$2" "${3:-configure}" >"$TMP/.out" 2>"$TMP/.err" || RC=$?
  OUT="$(cat "$TMP/.out")"
  ERR="$(cat "$TMP/.err")"
}

expected_for() { # suite -> the exact file postinst must write
  printf '%s\n%s\n' "$MARKER" "${EXPECTED_DEB_LINE/SUITE/$1}"
}

# --- Ubuntu 22.04 -> jammy ---------------------------------------------------
r="$(new_root jammy 'ID=ubuntu' 'VERSION_ID="22.04"' 'VERSION_CODENAME=jammy' 'UBUNTU_CODENAME=jammy')"
run "$r" "$POSTINST"
expect_eq "jammy: exit code" 0 "$RC"
expect_present "jammy: sources file written" "$r$LIST"
expect_eq "jammy: file content" "$(expected_for jammy)" "$(cat "$r$LIST" 2>/dev/null)"
expect_eq "jammy: file is 0644" 644 "$(stat -c %a "$r$LIST" 2>/dev/null)"
expect_contains "jammy: says what it did" "jammy" "$OUT"
expect_eq "jammy: one deb line only" 1 "$(grep -c '^deb ' "$r$LIST" 2>/dev/null)"
expect_eq "jammy: no trusted=yes" 0 "$(grep -c 'trusted=yes' "$r$LIST" 2>/dev/null)"

# Reinstalling the same package must not change anything (and must not fail).
before="$(sha256sum <"$r$LIST")"
run "$r" "$POSTINST" configure
expect_eq "jammy: reinstall exit code" 0 "$RC"
expect_eq "jammy: reinstall keeps the file identical" "$before" "$(sha256sum <"$r$LIST")"

# --- Ubuntu 24.04 -> noble, and jammy -> noble on the same machine -----------
r="$(new_root noble 'ID=ubuntu' 'VERSION_ID="24.04"' 'VERSION_CODENAME=noble' 'UBUNTU_CODENAME=noble')"
run "$r" "$POSTINST"
expect_eq "noble: exit code" 0 "$RC"
expect_eq "noble: file content" "$(expected_for noble)" "$(cat "$r$LIST" 2>/dev/null)"

# The file a 22.04 install left behind, then the OS updated to 24.04: the
# package rewrites its own source with the new suite (never a second entry).
printf '%s\n%s\n' "$MARKER" "${EXPECTED_DEB_LINE/SUITE/jammy}" >"$r$LIST"
sed -i 's/^VERSION_ID=.*/VERSION_ID="24.04"/' "$r/os-release"
run "$r" "$POSTINST"
expect_eq "jammy->noble: file content" "$(expected_for noble)" "$(cat "$r$LIST" 2>/dev/null)"
expect_eq "jammy->noble: still one deb line" 1 "$(grep -c '^deb ' "$r$LIST" 2>/dev/null)"

# --- Unsupported Ubuntu release: warning, no source, install not blocked -----
for rel in '20.04|focal' '23.10|mantic' '26.04|zesty'; do
  vid="${rel%%|*}"
  cod="${rel##*|}"
  r="$(new_root "ubuntu-$cod" 'ID=ubuntu' "VERSION_ID=\"$vid\"" "VERSION_CODENAME=$cod" "UBUNTU_CODENAME=$cod")"
  run "$r" "$POSTINST"
  expect_eq "ubuntu $vid: exit code (install is not blocked)" 0 "$RC"
  expect_absent "ubuntu $vid: no sources file" "$r$LIST"
  expect_contains "ubuntu $vid: warns about the release" "$cod" "$ERR"
  expect_contains "ubuntu $vid: says the app still works" "installed and works" "$ERR"
done

# --- Other distributions: warning, no source, no guessed suite --------------
for d in "debian|12|bookworm" "fedora|41|" "arch|rolling|"; do
  id="${d%%|*}"
  rest="${d#*|}"
  vid="${rest%%|*}"
  cod="${rest##*|}"
  args=("ID=$id" "VERSION_ID=\"$vid\"")
  [ -n "$cod" ] && args+=("VERSION_CODENAME=$cod")
  r="$(new_root "$id" "${args[@]}")"
  run "$r" "$POSTINST"
  expect_eq "$id: exit code" 0 "$RC"
  expect_absent "$id: no sources file" "$r$LIST"
  expect_contains "$id: says it is not Ubuntu" "not Ubuntu" "$ERR"
  expect_contains "$id: names the system" "$id" "$ERR"
done

# --- A derivative that declares Ubuntu gets the Ubuntu suite ----------------
r="$(new_root mint 'ID=linuxmint' 'ID_LIKE="ubuntu debian"' 'VERSION_ID="21.3"' 'VERSION_CODENAME=virginia' 'UBUNTU_CODENAME=jammy')"
run "$r" "$POSTINST"
expect_eq "mint 21: exit code" 0 "$RC"
expect_eq "mint 21: uses jammy (from UBUNTU_CODENAME)" "$(expected_for jammy)" "$(cat "$r$LIST" 2>/dev/null)"

# --- A file of somebody else is never overwritten ---------------------------
r="$(new_root foreign 'ID=ubuntu' 'VERSION_ID="22.04"' 'VERSION_CODENAME=jammy' 'UBUNTU_CODENAME=jammy')"
printf 'deb https://example.invalid/ hand-made main\n' >"$r$LIST"
run "$r" "$POSTINST"
expect_eq "foreign file: exit code" 0 "$RC"
expect_eq "foreign file: untouched" "deb https://example.invalid/ hand-made main" "$(cat "$r$LIST")"
expect_contains "foreign file: says it left it alone" "left untouched" "$ERR"

# postrm must not delete it either.
run "$r" "$POSTRM" remove
expect_eq "foreign file: postrm exit code" 0 "$RC"
expect_present "foreign file: survives postrm remove" "$r$LIST"

# --- Removal, purge, upgrade -------------------------------------------------
r="$(new_root removal 'ID=ubuntu' 'VERSION_ID="22.04"' 'VERSION_CODENAME=jammy' 'UBUNTU_CODENAME=jammy')"
run "$r" "$POSTINST"
expect_present "removal: source installed" "$r$LIST"
run "$r" "$POSTRM" upgrade
expect_present "upgrade: postrm keeps the source" "$r$LIST"
expect_eq "upgrade: exit code" 0 "$RC"
run "$r" "$POSTRM" remove
expect_eq "remove: exit code" 0 "$RC"
expect_absent "remove: source deleted" "$r$LIST"
expect_contains "remove: says so" "removed the NeverTube APT source" "$OUT"

r="$(new_root purge 'ID=ubuntu' 'VERSION_ID="22.04"' 'VERSION_CODENAME=jammy' 'UBUNTU_CODENAME=jammy')"
run "$r" "$POSTINST"
run "$r" "$POSTRM" purge
expect_eq "purge: exit code" 0 "$RC"
expect_absent "purge: source deleted" "$r$LIST"

r="$(new_root nofile 'ID=ubuntu' 'VERSION_ID="22.04"' 'VERSION_CODENAME=jammy' 'UBUNTU_CODENAME=jammy')"
run "$r" "$POSTRM" remove
expect_eq "postrm without a file: exit code" 0 "$RC"
expect_eq "postrm without a file: nothing to say" "" "$ERR"

# --- A stale source on a system that is not supported any more --------------
r="$(new_root stale 'ID=ubuntu' 'VERSION_ID="22.04"' 'VERSION_CODENAME=jammy' 'UBUNTU_CODENAME=jammy')"
run "$r" "$POSTINST"
printf 'ID=debian\nVERSION_ID="12"\nVERSION_CODENAME=bookworm\n' >"$r/os-release"
run "$r" "$POSTINST"
expect_eq "stale source: exit code" 0 "$RC"
expect_absent "stale source: removed from an unsupported system" "$r$LIST"
expect_contains "stale source: says it removed it" "removed the NeverTube APT source" "$OUT"

# --- Missing keyring: do not point apt at a file that is not there ----------
r="$(new_root nokeyring 'ID=ubuntu' 'VERSION_ID="22.04"' 'VERSION_CODENAME=jammy' 'UBUNTU_CODENAME=jammy')"
rm -f "$r$KEYRING"
run "$r" "$POSTINST"
expect_eq "missing keyring: exit code" 0 "$RC"
expect_absent "missing keyring: no sources file" "$r$LIST"
expect_contains "missing keyring: warns" "missing" "$ERR"

# --- Missing os-release -----------------------------------------------------
r="$(new_root noosrelease 'ID=ubuntu')"
rm -f "$r/os-release"
run "$r" "$POSTINST"
expect_eq "missing os-release: exit code" 0 "$RC"
expect_absent "missing os-release: no sources file" "$r$LIST"

# --- Other postinst actions do nothing --------------------------------------
r="$(new_root abort 'ID=ubuntu' 'VERSION_ID="22.04"' 'VERSION_CODENAME=jammy' 'UBUNTU_CODENAME=jammy')"
for action in abort-upgrade abort-remove abort-deconfigure triggered; do
  run "$r" "$POSTINST" "$action"
  expect_eq "postinst $action: exit code" 0 "$RC"
  expect_absent "postinst $action: touches nothing" "$r$LIST"
done

# --- The scripts themselves -------------------------------------------------
for s in "$POSTINST" "$POSTRM"; do
  if sh -n "$s" 2>"$TMP/.err"; then
    pass "$(basename "$s"): sh -n"
  else
    fail "$(basename "$s"): sh -n"
    sed 's/^/       /' "$TMP/.err"
  fi
done

# The postinst must not need the network, must not call apt, must not trust
# globally. Comments are stripped first (they explain these very rules), and
# what is looked for is a COMMAND, not the mention of one: the script is
# allowed to TELL the person to run `apt update`.
strip_comments() { sed -e 's/[[:space:]]#.*$//' -e 's/^#.*$//' "$1"; }
for s in "$POSTINST" "$POSTRM"; do
  code="$(strip_comments "$s")"
  for word in apt apt-get apt-key curl wget gpg; do
    if printf '%s\n' "$code" |
      grep -Eq "^[[:space:]]*(sudo[[:space:]]+)?$word([[:space:]]|$)"; then
      fail "$(basename "$s"): runs no '$word' command"
    else
      pass "$(basename "$s"): runs no '$word' command"
    fi
  done
  expect_missing "$(basename "$s"): does not use trusted=yes" "trusted=yes" "$code"
  expect_missing "$(basename "$s"): does not use apt-key" "apt-key" "$code"
done
expect_contains "postinst: points signed-by at the packaged keyring" "$KEYRING" "$(strip_comments "$POSTINST")"
expect_contains "postinst: uses the official repository URL" "$URL" "$(strip_comments "$POSTINST")"
expect_contains "postinst: marker shared with postrm" "$MARKER" "$(strip_comments "$POSTINST")"
expect_contains "postrm: same marker" "$MARKER" "$(strip_comments "$POSTRM")"

# --- The key that is packaged ----------------------------------------------
key_asc="$here/nevertube-archive-keyring.asc"
expect_present "public key present in the source tree" "$key_asc"
if [ -f "$key_asc" ]; then
  fpr="$(gpg --batch --with-colons --show-keys "$key_asc" 2>/dev/null | awk -F: '/^fpr:/{print $10; exit}')"
  expect_eq "public key fingerprint (the one that signs the published repo)" "$KEY_FPR" "$fpr"
fi

echo
printf '%d passed, %d failed\n' "$ok" "$bad"
[ "$bad" -eq 0 ]
