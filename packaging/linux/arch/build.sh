#!/usr/bin/env bash
#
# Build the Arch package (pacman) for nevertube.
#
# MUST run inside Arch (e.g. `docker run --rm -v "$PWD:/work" -w /work
# archlinux:latest bash /work/packaging/linux/arch/build.sh`). The binary is
# compiled INSIDE Arch (through makepkg), so it links Arch's own sonames.
#
# Two different recipes are involved, and they are not the same thing:
#
#   1. `packaging/linux/arch/PKGBUILD` is the PUBLISHED recipe (what goes to
#      the AUR): a real pkgver, a real source URL and a real sha256sums. It is
#      VALIDATED here (syntax, version, the pinned archive and its hash) but it
#      is not what this job builds: it builds a pinned commit, not the working
#      tree, and the job has to build the working tree.
#   2. the recipe this job hands to makepkg is GENERATED below, because its
#      source is a tarball produced here with `git archive` of the checkout.
#      For a locally generated tarball `sha256sums=('SKIP')` is legitimate
#      (the file never travels and it is our own tree), and it is why the
#      published recipe is NOT the one used: in the published one SKIP would be
#      a hole in the verification of what the user downloads.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
cd "$repo"

echo "== installing Arch build dependencies =="
pacman -Syu --noconfirm
pacman -S --noconfirm --needed \
  base-devel git cmake shadow curl coreutils \
  qt5-base mpv ffmpeg libpulse libglvnd openssl hicolor-icon-theme ttf-dejavu

# Release builds pass the version of the tag (NEVERTUBE_PKG_VERSION, see
# .github/workflows/release.yml); a development build reads CMakeLists.txt.
# It arrives normalized for pacman, which forbids '-' in pkgver (it is the
# pkgver/pkgrel separator): a pre-release is "1.0.0.alpha.1" here.
ver="${NEVERTUBE_PKG_VERSION:-$(grep -oP 'project\(nevertube VERSION \K[0-9.]+' CMakeLists.txt | head -1)}"
cmake_ver="$(grep -oP 'project\(nevertube VERSION \K[0-9.]+' CMakeLists.txt | head -1)"
echo "== packaging nevertube $ver (x86_64) =="

# git refuses to operate on a repo owned by another user (the mounted checkout
# is owned by the runner UID and this runs as root).
git config --global --add safe.directory "$repo"

# ---------------------------------------------------------------------------
# 1) Validation of the PUBLISHED recipe (packaging/linux/arch/PKGBUILD).
#
# It is checked here so that an AUR package cannot lie: the recipe must carry
# a real pkgver (not a placeholder), that version has to be the one of this
# tree, and its source has to be one this job can PROVE:
#
#   - a VCS source (git+...) is pinned by what its tag resolves to: makepkg
#     verifies no hash for it, so the recipe writes sha256sums=('SKIP') and what
#     is checked here is that the pin IS this pkgver (a moving branch would
#     build whatever the branch says today, which is not the release);
#   - a plain tarball URL carries a real SHA-256: the archive is downloaded here
#     and the hash compared, so a recipe whose sum matches nothing cannot be
#     published.
#
# In both cases the directory build() enters has to be the one the source
# produces: that mismatch is invisible until an AUR user hits it.
#
# Deliberately no `| head` and no `| grep -q` below: with pipefail an early
# reader makes the writer die of SIGPIPE and the status becomes a false failure.
# ---------------------------------------------------------------------------
say() { echo; echo "== $* =="; }
say "validating the published PKGBUILD"

pub="$here/PKGBUILD"
bash -n "$pub"

pub_ver="$(sed -n 's/^pkgver=//p' "$pub")"
if [ -z "$pub_ver" ]; then
  echo "ERROR: the published PKGBUILD has no pkgver" >&2
  exit 1
fi
case "$pub_ver" in
  *__*|*@*|*'$'*)
    echo "ERROR: the published PKGBUILD still carries a placeholder in pkgver: '$pub_ver'" >&2
    exit 1 ;;
esac
if [ "$pub_ver" != "$cmake_ver" ]; then
  echo "ERROR: the published PKGBUILD says $pub_ver and CMakeLists.txt $cmake_ver." >&2
  echo "       Bump both together or the AUR package would build another version." >&2
  exit 1
fi

# The recipe is a bash script, so its arrays are read by SOURCING it in a
# subshell -- which is how makepkg itself sees them. String surgery over the
# text (tr/sed fighting quotes, parentheses and spaces) mis-parses a recipe, and
# it did once: it is not used here.
vals="$(mktemp)"
if ! ( set +eu; source "$pub" >/dev/null 2>&1;
        echo "${source[0]:-}"; echo "${sha256sums[0]:-}" ) > "$vals"; then
  echo "ERROR: the published PKGBUILD does not source cleanly" >&2
  rm -f "$vals"
  exit 1
fi
pub_src="$(sed -n 1p "$vals")"
pub_sum="$(sed -n 2p "$vals")"
rm -f "$vals"
if [ -z "$pub_src" ] || [ -z "$pub_sum" ]; then
  echo "ERROR: the published PKGBUILD has no source URL or no sha256sums" >&2
  exit 1
fi

# "name::url" renames the checkout directory; without it the name comes from
# the URL. build() enters that one.
src_name=""
src_url="$pub_src"
case "$pub_src" in
  *::*) src_name="${pub_src%%::*}"; src_url="${pub_src#*::}" ;;
esac
echo "-- source: $src_url"
echo "-- sha256: $pub_sum"

# The directory build() enters, read from the recipe itself (the last path
# element of its `cd "$srcdir/..."`; awk exits at the first match).
cd_dir="$(awk 'match($0, /cd "[$]srcdir[/][^"]*/) { s = substr($0, RSTART, RLENGTH); sub(/.*[/]/, "", s); print s; exit }' "$pub")"

case "$src_url" in
  git+*|hg+*|svn+*|bzr+*) is_vcs=1 ;;
  *) is_vcs=0 ;;
esac

if [ "$is_vcs" -eq 1 ]; then
  # VCS: there is no archive to hash.
  if [ "$pub_sum" != "SKIP" ]; then
    echo "ERROR: a VCS source is pinned by its tag, not by a hash: sha256sums" >&2
    echo "       is ('SKIP') in this case and the recipe says ('$pub_sum')." >&2
    exit 1
  fi
  case "$src_url" in
    *'#tag=v$pkgver'|*"#tag=v$pub_ver"|*'#commit='*) : ;;
    *)
      echo "ERROR: the published recipe does not pin a tag/commit for $pub_ver." >&2
      exit 1 ;;
  esac
  if [ -z "$src_name" ]; then
    echo "ERROR: a VCS source needs 'name::' (build() enters srcdir/<name>)" >&2
    exit 1
  fi
  if [ "$cd_dir" != "$src_name" ]; then
    echo "ERROR: build() enters srcdir/$cd_dir and this source is checked out" >&2
    echo "       into srcdir/$src_name." >&2
    exit 1
  fi
  echo "OK: the published recipe is a VCS source pinned to v$pub_ver"
else
  case "$pub_sum" in
    SKIP|''|*[!0-9a-fA-F]*)
      echo "ERROR: sha256sums of the published PKGBUILD is not a hash: '$pub_sum'" >&2
      echo "       SKIP is only acceptable for a VCS source, or for a tarball" >&2
      echo "       generated locally (which is what this job builds, not what is" >&2
      echo "       published)." >&2
      exit 1 ;;
  esac
  pub_tar="$(mktemp)"
  curl -fsSL --retry 3 -o "$pub_tar" "$src_url"
  got="$(sha256sum "$pub_tar")"
  got="${got%% *}"
  if [ "$got" != "$pub_sum" ]; then
    echo "ERROR: the published sha256sums does not match the archive it points at." >&2
    echo "       expected $pub_sum" >&2
    echo "       got      $got" >&2
    echo "       Refresh it with updpkgsums (or recompute it against the URL)." >&2
    rm -f "$pub_tar"
    exit 1
  fi
  # Listing the archive says whether it is a .tar.gz at all, and its first
  # member is the directory build() has to enter.
  list="$(mktemp)"
  if ! tar -tzf "$pub_tar" > "$list" 2>/dev/null; then
    echo "ERROR: the archive of the published recipe cannot be listed as a .tar.gz." >&2
    echo "       If its source is another format, this check has to be extended" >&2
    echo "       instead of skipped." >&2
    rm -f "$pub_tar" "$list"
    exit 1
  fi
  first=""
  read -r first < "$list" || true
  top="${first%%/*}"
  rm -f "$pub_tar" "$list"
  if [ -n "$cd_dir" ] && [ "$cd_dir" != "$top" ]; then
    echo "ERROR: build() enters srcdir/$cd_dir and the archive extracts '$top'" >&2
    exit 1
  fi
  echo "OK: the published recipe is coherent (version, hash and directory)"
fi

# 2) The package of THIS checkout.
# ---------------------------------------------------------------------------
id -u builder >/dev/null 2>&1 || useradd -m builder

rm -rf archbuild
mkdir -p archbuild

# `sha256sums=('SKIP')`: the source below is generated HERE with `git archive`
# of the checkout, so there is nothing to verify against (and nothing is
# downloaded). The published recipe, which does download, carries the real
# hash and is the one validated above.
cat > archbuild/PKGBUILD <<PKGBUILD_EOF
# Maintainer: marlo4220mc <https://github.com/marlo4220mc>
#
# GENERATED by packaging/linux/arch/build.sh for the CI: it builds the working
# tree (git archive of the checkout) instead of the pinned release of the
# published PKGBUILD. The dependencies, the check() list and the layout are
# the ones of the published recipe.
pkgname=nevertube
pkgver=${ver}
pkgrel=1
pkgdesc='NeverTube - lightweight YouTube client (Qt5 + libmpv)'
arch=('x86_64')
url='https://github.com/marlo4220mc/NeverTube'
license=('GPL-3.0-or-later')
depends=('qt5-base' 'mpv' 'ffmpeg' 'libpulse' 'libglvnd' 'openssl' 'hicolor-icon-theme')
optdepends=('qt5-wayland: native Wayland instead of XWayland')
makedepends=('cmake' 'gcc' 'make' 'pkgconf' 'qt5-base' 'mpv' 'ffmpeg' 'libpulse')
source=('nevertube-${ver}.tar.gz')
sha256sums=('SKIP')
options=('!debug')

build() {
  cd "\$srcdir/nevertube"
  # NEVERTUBE_VERSION: what the app shows (Ajustes) and what its updater
  # compares against a release. The display spelling, which is not the pacman
  # one when there is a suffix (1.0.0-alpha.1 vs 1.0.0.alpha.1).
  extra=()
  if [ -n "\${NEVERTUBE_VERSION:-}" ]; then
    extra+=("-DNEVERTUBE_VERSION=\$NEVERTUBE_VERSION")
  fi
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr "\${extra[@]}"
  cmake --build build --parallel "\$(nproc)"
}

# Offline regression tests (no network, no GPU): the same "viability" gate the
# other distro builds run.
check() {
  cd "\$srcdir/nevertube"
  export QT_QPA_PLATFORM=offscreen
  for t in --selftest-db --selftest-discover --selftest-ssl \\
           --selftest-grid --selftest-theme --selftest-glyphs \\
           --selftest-i18n --selftest-dates --selftest-av1hls \\
           --selftest-variants --selftest-reels --selftest-download \\
           --selftest-download-button --selftest-download-folder \\
           --selftest-download-settings --selftest-download-transport \\
           --selftest-download-resume --selftest-single-instance \\
           --selftest-video-url --selftest-search-url --selftest-update \\
           --selftest-mpris \\
           --selftest-settings-page; do
    echo "SELFTEST \$t"
    ./build/nevertube "\$t"
  done
}

package() {
  cd "\$srcdir/nevertube"
  DESTDIR="\$pkgdir" cmake --install build --prefix /usr
}
PKGBUILD_EOF

# The "source" of the generated recipe is a git archive of the checkout, so
# what is built is exactly what is committed.
git archive --format=tar.gz --prefix=nevertube/ \
  -o "archbuild/nevertube-$ver.tar.gz" HEAD

chown -R builder:builder archbuild
su builder -c "cd '$repo/archbuild' && makepkg -f --noconfirm"

mkdir -p dist
pkg="dist/nevertube-${ver}-x86_64.pkg.tar.zst"
cp archbuild/*.pkg.tar.zst "$pkg"
# NOTE: grep without -q on purpose: with pipefail, -q closes the pipe early,
# tar gets SIGPIPE and the script exits 141 even though the package is fine.
tar -tvf "$pkg" | grep 'usr/bin/nevertube' >/dev/null
tar -tvf "$pkg" | grep 'usr/share/applications/nevertube.desktop' >/dev/null
tar -xOf "$pkg" .PKGINFO > "$repo/archbuild/PKGINFO"
# Every runtime package the binary needs is a hard `depend`: makepkg does not
# derive them from the sonames, so the PKGBUILD lists them by hand and this gate
# checks none is missing.
for dep in qt5-base mpv ffmpeg libpulse libglvnd openssl hicolor-icon-theme; do
  grep -qx "depend = $dep" "$repo/archbuild/PKGINFO"
done
# Wayland is optional (optdepend): the app runs via xcb/XWayland without it.
grep -q '^optdepend = qt5-wayland' "$repo/archbuild/PKGINFO"

# Installability: install the package and run offline selftests from the
# INSTALLED path (not the build tree), like the .deb job does on the runner.
pacman -R --noconfirm nevertube >/dev/null 2>&1 || true
pacman -U --noconfirm "$pkg"
# ldd catches a package whose depends do not cover a NEEDED library. grep WITHOUT
# -q: with pipefail, -q closes the pipe early and ldd dies with SIGPIPE (141).
if ldd /usr/bin/nevertube | grep 'not found' >/dev/null; then
  echo "ERROR: missing shared libraries after install:" >&2
  ldd /usr/bin/nevertube | grep 'not found' >&2
  exit 1
fi
QT_QPA_PLATFORM=offscreen /usr/bin/nevertube --selftest-db
QT_QPA_PLATFORM=offscreen /usr/bin/nevertube --selftest-i18n

echo "OK: $pkg ($(du -h "$pkg" | cut -f1))"
