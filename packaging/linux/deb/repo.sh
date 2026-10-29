#!/usr/bin/env bash
#
# Build an APT repository (Debian/Ubuntu) out of the .deb files of a release.
#
# It is what makes `apt update` + `apt upgrade` update NeverTube like any other
# package: the repository is a tree of metadata (`dists/<suite>/…`) plus the
# packages in a `pool/`, signed with a GPG key. What gets published to GitHub
# Pages is exactly this tree (see .github/workflows/publish-repos.yml).
#
# Two suites, not one: the Ubuntu 22.04 package and the Ubuntu 24.04 one have
# the SAME name and version but different dependencies (24.04 renamed the Qt
# packages to the ...t64 ones), so they cannot share a single suite: apt would
# see two candidates with the same version and could install the wrong one.
# Each distribution reads its own suite (jammy / noble) and the `Depends` of
# its package is the one that was built and smoke-tested on it:
#
#   deb [signed-by=/etc/apt/keyrings/nevertube.gpg] \
#     https://<user>.github.io/NeverTube/apt/ jammy main
#
# Usage:
#   packaging/linux/deb/repo.sh OUTDIR SUITE=FILE.deb [SUITE=FILE.deb ...]
#
#   packaging/linux/deb/repo.sh repo \
#     jammy=dist/nevertube_1.0.0_amd64.deb noble=dist/nevertube_1.0.0_amd64.deb
#
# Signing: the Release of every suite is signed with the key of
# NEVERTUBE_APT_GPG_KEY (a key id or fingerprint present in the keyring), which
# produces `InRelease` and `Release.gpg`. WITHOUT a key the script REFUSES to
# write the repository: publishing metadata that says "signed" without a
# signature is worse than not publishing. Use --unsigned-test only for a local
# dry run, and note that what it writes says so in `SUITE-unsigned.txt`.
#
# The public key has to be exported for the users:
#   gpg --armor --export <key> > nevertube.gpg   # what `signed-by=` points at
set -euo pipefail

if [ "$#" -lt 2 ]; then
  sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
fi

OUT="$1"
shift

UNSIGNED_TEST=0
declare -a SUITES=()
for arg in "$@"; do
  if [ "$arg" = "--unsigned-test" ]; then
    UNSIGNED_TEST=1
    continue
  fi
  SUITES+=("$arg")
done

if [ "${#SUITES[@]}" -eq 0 ]; then
  echo "ERROR: no suite was given (SUITE=FILE.deb)" >&2
  exit 2
fi

KEY="${NEVERTUBE_APT_GPG_KEY:-}"
if [ -z "$KEY" ] && [ "$UNSIGNED_TEST" -eq 0 ]; then
  echo "ERROR: NEVERTUBE_APT_GPG_KEY is not set." >&2
  echo "       The metadata of an APT repository IS what apt verifies, and a" >&2
  echo "       repository published unsigned cannot be called official." >&2
  echo "       Export the key of the project and import it in the job:" >&2
  echo "         gpg --armor --export-secret-keys <key> > private.gpg   # SECRET," >&2
  echo "         # stored as the GPG_PRIVATE_KEY secret of the repository." >&2
  echo "       For a LOCAL, throwaway test only: --unsigned-test" >&2
  exit 1
fi

for tool in dpkg-scanpackages apt-ftparchive; do
  command -v "$tool" >/dev/null || {
    echo "ERROR: $tool is missing (apt install dpkg-dev apt-utils)" >&2
    exit 1
  }
done
if [ -n "$KEY" ]; then
  command -v gpg >/dev/null || {
    echo "ERROR: gpg is missing (apt install gnupg)" >&2
    exit 1
  }
fi

rm -rf "$OUT"
mkdir -p "$OUT"
OUT_ABS="$(cd "$OUT" && pwd)"

for entry in "${SUITES[@]}"; do
  suite="${entry%%=*}"
  deb="${entry#*=}"
  if [ "$suite" = "$entry" ] || [ -z "$deb" ]; then
    echo "ERROR: '$entry' is not SUITE=FILE.deb" >&2
    exit 2
  fi
  if [ ! -f "$deb" ]; then
    echo "ERROR: $deb does not exist" >&2
    exit 1
  fi

  pkg="$(dpkg-deb -f "$deb" Package)"
  ver="$(dpkg-deb -f "$deb" Version)"
  arch="$(dpkg-deb -f "$deb" Architecture)"
  if [ -z "$pkg" ] || [ -z "$ver" ] || [ -z "$arch" ]; then
    echo "ERROR: $deb has no Package/Version/Architecture in its control file" >&2
    exit 1
  fi

  # pool/<suite>/main/<letra>/<paquete>/<fichero>.deb, the layout apt expects.
  letter="$(printf '%s' "$pkg" | cut -c1)"
  pool="$OUT_ABS/pool/$suite/main/$letter/$pkg"
  mkdir -p "$pool"
  cp "$deb" "$pool/"

  # The Packages index. Running it from $OUT keeps the Filename field relative
  # to the root of the repository, which is what apt resolves.
  bin="$OUT_ABS/dists/$suite/main/binary-$arch"
  mkdir -p "$bin"
  ( cd "$OUT_ABS" && dpkg-scanpackages "pool/$suite" ) > "$bin/Packages"
  gzip -9nc "$bin/Packages" > "$bin/Packages.gz"

  echo "== $suite: $pkg $ver ($arch) =="
  grep -E '^(Package|Version|Filename|SHA256):' "$bin/Packages"

  # The release file of the suite: Date, Architectures and the hashes of every
  # index are computed by apt-ftparchive from the tree itself.
  conf="$(mktemp)"
  cat > "$conf" <<EOF
APT::FTPArchive::Release {
  Origin "NeverTube";
  Label "NeverTube";
  Suite "$suite";
  Codename "$suite";
  Architectures "$arch";
  Components "main";
  Description "NeverTube packages for $suite";
};
EOF
  (
    cd "$OUT_ABS"
    apt-ftparchive -c "$conf" release "dists/$suite"
  ) > "$OUT_ABS/dists/$suite/Release"
  rm -f "$conf"

  if [ -n "$KEY" ]; then
    gpg --batch --yes --local-user "$KEY" --clearsign \
        -o "$OUT_ABS/dists/$suite/InRelease" "$OUT_ABS/dists/$suite/Release"
    gpg --batch --yes --local-user "$KEY" --detach-sign --armor \
        -o "$OUT_ABS/dists/$suite/Release.gpg" "$OUT_ABS/dists/$suite/Release"
    # A signature that does not verify is a broken repository: it is checked
    # here, where the failure is one line, and not in the user's apt.
    gpg --verify "$OUT_ABS/dists/$suite/InRelease"
  else
    echo "WARNING: TEST repository, unsigned ($suite)" \
      > "$OUT_ABS/dists/$suite/$suite-unsigned.txt"
    echo "WARNING: unsigned test repository ($suite): it must not be published" >&2
  fi
done

echo
echo "OK: APT repository in $OUT_ABS"
echo "    suites: $(cd "$OUT_ABS/dists" && echo *)"
if [ -n "$KEY" ]; then
  echo "    signed with: $KEY"
  echo "    users: deb [signed-by=/etc/apt/keyrings/nevertube.gpg] <url>/apt/ <suite> main"
fi
