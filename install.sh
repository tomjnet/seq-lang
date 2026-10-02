#!/usr/bin/env bash
# seqc installer: installs the Seq compiler, its runtime library and its
# project templates for the current user, with no root access needed.
#
#   curl -fsSL https://raw.githubusercontent.com/tomjnet/seq-lang/HEAD/install.sh | bash
#
# By default it downloads the pre-built package of the latest GitHub release
# and verifies its SHA-256 checksum. When no package fits (an older C library,
# a branch instead of a release, no release yet) it falls back to cloning the
# repository and building from source.
#
# Options can be passed through bash:
#
#   curl -fsSL https://raw.githubusercontent.com/tomjnet/seq-lang/HEAD/install.sh \
#     | bash -s -- --ref v0.1.0
#
# Run with --help for all options.
#
# The installer never downloads a model and never installs system packages.
# What compiling and running workflows still needs is reported at the end by
# `seqc doctor`.
#
# The whole script is wrapped in main(), which is only called on the last
# line, so a partially downloaded script never runs.

if [ -z "${BASH_VERSION:-}" ]; then
  echo "seqc installer: this script needs bash. Run it with: ... | bash" >&2
  exit 1
fi

set -Eeuo pipefail
umask 022

readonly INSTALLER_VERSION="1.0.0"
readonly DEFAULT_REPO="https://github.com/tomjnet/seq-lang.git"
# Empty means the latest release, or the default branch when building.
readonly DEFAULT_REF=""
readonly MIN_CMAKE_VERSION="3.28"
# The release package is built on Ubuntu 24.04 (.github/workflows/release.yml).
# Its runtime library is linked into generated programs with the host's C
# library, so the host needs at least the glibc of the build machine.
readonly MIN_GLIBC_VERSION="2.39"
readonly RELEASE_TAG_PATTERN='^v[0-9]+\.[0-9]+\.[0-9]+$'

# Settings; environment variables provide defaults, options override them.
# They are named SEQ_INSTALL_* to stay clear of seqc's own SEQC_<SECTION>_<KEY>
# settings.
REPO="${SEQ_INSTALL_REPO:-$DEFAULT_REPO}"
REF="${SEQ_INSTALL_REF:-$DEFAULT_REF}"
PREFIX="${SEQ_INSTALL_PREFIX:-${HOME:-}/.local}"
RELEASES_URL="${SEQ_INSTALL_RELEASES_URL:-}"
MODE="auto" # auto: binary, else source | binary: binary only | source
SOURCE_DIR=""
JOBS=""
RUN_TESTS=0
KEEP_WORKDIR=0
VERBOSE=0
CHECK_ONLY=0
UNINSTALL=0

# Filled in while running.
WORKDIR=""
LOG_FILE=""
CXX_BIN=""
CC_BIN=""
GENERATOR=""
ARCH=""
USE_BINARY=0
SRC=""
BUILD_DIR=""
CURRENT_STEP="starting"

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

setup_colors() {
  if [ -t 2 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != "dumb" ]; then
    BOLD=$'\033[1m' DIM=$'\033[2m' RED=$'\033[31m' GREEN=$'\033[32m'
    YELLOW=$'\033[33m' BLUE=$'\033[34m' RESET=$'\033[0m'
  else
    BOLD="" DIM="" RED="" GREEN="" YELLOW="" BLUE="" RESET=""
  fi
}

step() {
  CURRENT_STEP="$*"
  printf '%s==>%s %s%s%s\n' "$BLUE" "$RESET" "$BOLD" "$*" "$RESET" >&2
}
info() { printf '    %s\n' "$*" >&2; }
ok() { printf '    %s✓%s %s\n' "$GREEN" "$RESET" "$*" >&2; }
warn() { printf '    %s!%s %s\n' "$YELLOW" "$RESET" "$*" >&2; }
fail() { printf '    %s✗%s %s\n' "$RED" "$RESET" "$*" >&2; }
die() {
  printf '%serror:%s %s\n' "$RED$BOLD" "$RESET" "$*" >&2
  exit 1
}

usage() {
  cat <<EOF
seqc installer ${INSTALLER_VERSION}

Installs seqc, the Seq compiler, for the current user (no root needed). By
default the pre-built package of the latest release is downloaded and its
checksum verified; when no package fits, seqc is built from source.

Usage:
  install.sh [options]
  curl -fsSL https://raw.githubusercontent.com/tomjnet/seq-lang/HEAD/install.sh | bash -s -- [options]

Options:
  --prefix DIR     Install under DIR (default: \$HOME/.local)
                   The compiler goes to DIR/bin, its runtime library and
                   templates to DIR/lib/seqc.
  --ref REF        Release tag (e.g. v0.1.0) or, when building from source,
                   a branch (default: the latest release / default branch)
  --binary         Only install a pre-built release package; never build
  --from-source    Always clone and build from source
  --repo URL       Git repository (default: ${DEFAULT_REPO})
  --source DIR     Build from an existing local checkout (implies --from-source)
  --jobs N         Parallel build jobs (default: number of CPUs)
  --with-tests     Build and run the test suite (implies --from-source;
                   needs python3 and curl)
  --check          Only check what is needed, do not install
  --uninstall      Remove a previous installation from the prefix
  --keep           Keep the temporary directory for inspection
  --verbose        Show the full git, CMake and compiler output
  -h, --help       Show this help and exit
  --version        Show the installer version and exit

Environment:
  SEQ_INSTALL_PREFIX, SEQ_INSTALL_REF, SEQ_INSTALL_REPO
                             Defaults for --prefix, --ref, --repo
  SEQ_INSTALL_RELEASES_URL   Base URL of the release downloads
                             (default: <repo>/releases)
  CXX                        C++ compiler to try first
  NO_COLOR                   Disable colored output
EOF
}

# ---------------------------------------------------------------------------
# Error handling and cleanup
# ---------------------------------------------------------------------------

cleanup() {
  local status=$?
  if [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ]; then
    if [ "$KEEP_WORKDIR" -eq 1 ]; then
      info "Temporary directory kept at: $WORKDIR"
    else
      rm -rf -- "$WORKDIR"
    fi
  fi
  exit "$status"
}

on_error() {
  local status=$? line=$1
  trap - ERR
  printf '\n%serror:%s installation failed while: %s (line %s, exit %s)\n' \
    "$RED$BOLD" "$RESET" "$CURRENT_STEP" "$line" "$status" >&2
  if [ -n "$LOG_FILE" ] && [ -s "$LOG_FILE" ] && [ "$VERBOSE" -eq 0 ]; then
    printf '%s--- last lines of the log ---%s\n' "$DIM" "$RESET" >&2
    tail -n 25 "$LOG_FILE" >&2 || true
    printf '%s-----------------------------%s\n' "$DIM" "$RESET" >&2
  fi
  printf 'Re-run with --verbose for full output, or --keep to inspect the temporary directory.\n' >&2
  exit "$status"
}

# Runs a command, sending its output to the log unless --verbose is set.
# Never lets it read stdin: with `curl | bash` stdin is the script itself.
run() {
  if [ "$VERBOSE" -eq 1 ]; then
    "$@" </dev/null
  else
    "$@" </dev/null >>"$LOG_FILE" 2>&1
  fi
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

need_value() {
  if [ $# -lt 2 ] || [ -z "$2" ] || [[ "$2" == --* ]]; then
    die "option $1 requires a value (see --help)"
  fi
}

parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --prefix) need_value "$@"; PREFIX=$2; shift 2 ;;
      --prefix=*) PREFIX=${1#*=}; shift ;;
      --ref) need_value "$@"; REF=$2; shift 2 ;;
      --ref=*) REF=${1#*=}; shift ;;
      --repo) need_value "$@"; REPO=$2; shift 2 ;;
      --repo=*) REPO=${1#*=}; shift ;;
      --source) need_value "$@"; SOURCE_DIR=$2; shift 2 ;;
      --source=*) SOURCE_DIR=${1#*=}; shift ;;
      --jobs) need_value "$@"; JOBS=$2; shift 2 ;;
      --jobs=*) JOBS=${1#*=}; shift ;;
      --binary) MODE="binary"; shift ;;
      --from-source) MODE="source"; shift ;;
      --with-tests) RUN_TESTS=1; shift ;;
      --check) CHECK_ONLY=1; shift ;;
      --uninstall) UNINSTALL=1; shift ;;
      --keep) KEEP_WORKDIR=1; shift ;;
      --verbose) VERBOSE=1; shift ;;
      -h | --help) usage; exit 0 ;;
      --version) echo "seqc installer ${INSTALLER_VERSION}"; exit 0 ;;
      *) die "unknown option: $1 (see --help)" ;;
    esac
  done

  [ -n "$PREFIX" ] || die "the install prefix is empty; set --prefix or HOME"
  # Expand a leading ~ and make the prefix absolute.
  PREFIX=${PREFIX/#\~/${HOME:-~}}
  [[ "$PREFIX" == /* ]] || PREFIX="$PWD/$PREFIX"
  PREFIX=${PREFIX%/}
  [ -n "$PREFIX" ] || die "refusing to install into /"

  if [ -n "$JOBS" ] && ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    die "--jobs expects a positive number, got: $JOBS"
  fi
  if [ -n "$SOURCE_DIR" ]; then
    [ -d "$SOURCE_DIR" ] || die "--source directory does not exist: $SOURCE_DIR"
    SOURCE_DIR=$(cd "$SOURCE_DIR" && pwd)
  fi
  if [[ "$REF" == -* ]] || [[ "$REF" =~ [[:space:]] ]]; then
    die "invalid --ref: $REF"
  fi
  if [ "$MODE" = "binary" ] && { [ -n "$SOURCE_DIR" ] || [ "$RUN_TESTS" -eq 1 ]; }; then
    die "--binary cannot be combined with --source or --with-tests"
  fi
  if [ -n "$SOURCE_DIR" ] || [ "$RUN_TESTS" -eq 1 ]; then
    MODE="source"
  fi
}

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

have() { command -v "$1" >/dev/null 2>&1; }

# True when version $1 >= version $2.
version_ge() {
  [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1)" = "$2" ]
}

distro_id() {
  if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    (. /etc/os-release && echo "${ID:-} ${ID_LIKE:-}")
  fi
}

# Version of the GNU C library, or empty on another C library such as musl.
glibc_version() {
  getconf GNU_LIBC_VERSION 2>/dev/null | awk '{ print $2 }' || true
}

# Prints how to install the build dependencies on this distribution.
print_package_hint() {
  local ids
  ids=" $(distro_id) "
  info ""
  info "Install the build dependencies with your package manager, e.g.:"
  case "$ids" in
    *" ubuntu "* | *" debian "*)
      info "  sudo apt-get update && sudo apt-get install -y git cmake ninja-build build-essential"
      info "  (Ubuntu 22.04 / Debian 12 ship an older GCC and CMake; seqc needs"
      info "   GCC >= 13 and CMake >= ${MIN_CMAKE_VERSION}. Ubuntu 24.04 has both.)"
      ;;
    *" fedora "* | *" rhel "* | *" centos "*)
      info "  sudo dnf install -y git cmake ninja-build gcc gcc-c++"
      ;;
    *" arch "*)
      info "  sudo pacman -S --needed git cmake ninja gcc"
      ;;
    *" opensuse"* | *" suse "*)
      info "  sudo zypper install -y git cmake ninja gcc13 gcc13-c++"
      ;;
    *)
      info "  git, CMake >= ${MIN_CMAKE_VERSION}, ninja or make, and GCC >= 13 (C and C++)"
      ;;
  esac
}

check_platform() {
  step "Checking platform"
  local os machine
  os=$(uname -s)
  machine=$(uname -m)
  [ "$os" = "Linux" ] ||
    die "seqc targets Linux x86_64 (this system is $os). On Windows, use WSL2."
  case "$machine" in
    x86_64 | amd64) ARCH="x86_64" ;;
    *) die "seqc targets Linux x86_64 (this machine is $machine)" ;;
  esac
  ok "Linux $(uname -r) ($machine)"
  if [ "$(id -u)" -eq 0 ]; then
    warn "Running as root. seqc does not need root; it will be installed into $PREFIX"
  fi
}

# Base URL of the GitHub release downloads, or empty when unknown.
releases_base_url() {
  if [ -n "$RELEASES_URL" ]; then
    echo "${RELEASES_URL%/}"
    return
  fi
  local repo=${REPO%/}
  repo=${repo%.git}
  if [[ "$repo" =~ ^https://github\.com/[^/]+/[^/]+$ ]]; then
    echo "$repo/releases"
  fi
}

# Decides between the pre-built package and a source build.
choose_method() {
  step "Choosing installation method"
  local reason="" libc
  libc=$(glibc_version)
  if [ "$MODE" = "source" ]; then
    reason="building from source as requested"
  elif [ -n "$REF" ] && ! [[ "$REF" =~ $RELEASE_TAG_PATTERN ]]; then
    reason="--ref $REF is not a release tag (vX.Y.Z)"
  elif [ -z "$libc" ]; then
    reason="the pre-built package needs the GNU C library"
  elif ! version_ge "$libc" "$MIN_GLIBC_VERSION"; then
    reason="the pre-built package needs glibc >= $MIN_GLIBC_VERSION (this system has $libc)"
  elif [ -z "$(releases_base_url)" ]; then
    reason="$REPO is not a GitHub repository; set SEQ_INSTALL_RELEASES_URL for packages"
  fi

  if [ -z "$reason" ]; then
    USE_BINARY=1
    ok "pre-built package: seqc-linux-$ARCH (${REF:-latest release})"
  elif [ "$MODE" = "binary" ]; then
    die "cannot use a pre-built package: $reason"
  else
    USE_BINARY=0
    ok "source build: $reason"
  fi
}

# Tools needed to download and verify a release package. Returns 1 if any is
# missing.
check_download_tools() {
  step "Checking download tools"
  local missing=0 tool
  if have curl; then
    ok "curl"
  elif have wget; then
    ok "wget"
  else
    fail "curl or wget not found"
    missing=1
  fi
  for tool in tar gzip sha256sum install; do
    if have "$tool"; then
      ok "$tool"
    else
      fail "$tool not found"
      missing=1
    fi
  done
  return "$missing"
}

# Picks the first compiler that builds a C++20 program using what seqc uses.
find_compiler() {
  local test_dir="$WORKDIR/compiler-check" candidate path
  mkdir -p "$test_dir"
  cat >"$test_dir/check.cc" <<'EOF'
#include <concepts>
#include <filesystem>
#include <span>
#include <string_view>
template <std::integral T> T twice(T v) { return v + v; }
int main() {
  const std::string_view text = "seq";
  const std::span<const char> view(text.data(), text.size());
  return std::filesystem::path("a").empty() || !text.starts_with("s") ||
                 twice(view.size()) != 6
             ? 1
             : 0;
}
EOF
  local candidates=("${CXX:-}" c++ g++ g++-15 g++-14 g++-13 clang++
    clang++-20 clang++-19 clang++-18 clang++-17)
  local tried=()
  for candidate in "${candidates[@]}"; do
    if [ -z "$candidate" ] || ! have "$candidate"; then
      continue
    fi
    path=$(command -v "$candidate")
    # Skip duplicates such as c++ -> g++.
    if [[ " ${tried[*]:-} " == *" $path "* ]]; then
      continue
    fi
    tried+=("$path")
    if "$candidate" -std=c++20 -o "$test_dir/check" "$test_dir/check.cc" \
      </dev/null >/dev/null 2>&1 && "$test_dir/check"; then
      CXX_BIN=$path
      return 0
    fi
  done
  return 1
}

# The C compiler that belongs to $CXX_BIN (g++-14 -> gcc-14), for the runtime
# library. Left empty when there is no obvious match; CMake then picks one.
find_c_compiler() {
  local dir name
  dir=$(dirname "$CXX_BIN")
  name=$(basename "$CXX_BIN")
  case "$name" in
    g++*) name="gcc${name#g++}" ;;
    clang++*) name="clang${name#clang++}" ;;
    *) name="" ;;
  esac
  if [ -n "$name" ] && [ -x "$dir/$name" ]; then
    CC_BIN="$dir/$name"
  fi
}

# Tools needed to build from source. Returns 1 if any is missing.
check_build_tools() {
  step "Checking build tools"
  local missing=0

  if [ -z "$SOURCE_DIR" ]; then
    if have git; then
      ok "git $(git --version | awk '{print $3}')"
    else
      fail "git not found"
      missing=1
    fi
  fi

  if have cmake; then
    local cmake_version
    cmake_version=$(cmake --version | awk 'NR == 1 {print $3}')
    if version_ge "$cmake_version" "$MIN_CMAKE_VERSION"; then
      ok "cmake $cmake_version"
    else
      fail "cmake $cmake_version is too old (need >= $MIN_CMAKE_VERSION)"
      missing=1
    fi
  else
    fail "cmake not found (need >= $MIN_CMAKE_VERSION)"
    missing=1
  fi

  if have ninja; then
    GENERATOR="Ninja"
    ok "ninja $(ninja --version)"
  elif have make; then
    GENERATOR="Unix Makefiles"
    ok "make $(make --version | awk 'NR == 1 {print $NF}')"
  else
    fail "no build tool found (need ninja or make)"
    missing=1
  fi

  if find_compiler; then
    ok "C++20 compiler: $CXX_BIN ($("$CXX_BIN" --version | head -n 1))"
    find_c_compiler
    if [ -n "$CC_BIN" ]; then
      ok "C compiler: $CC_BIN"
    elif have cc || have gcc || have clang; then
      ok "C compiler: chosen by CMake"
    else
      fail "no C compiler found (need gcc or clang for the runtime library)"
      missing=1
    fi
  else
    fail "no C++20 compiler found (need GCC >= 13 or a recent Clang)"
    local found
    for found in g++ clang++; do
      if have "$found"; then
        info "  found: $("$found" --version | head -n 1)"
      fi
    done
    missing=1
  fi

  if [ "$missing" -ne 0 ]; then
    print_package_hint
  fi
  return "$missing"
}

check_prefix_writable() {
  step "Checking install location"
  local dir=$PREFIX
  # Walk up to the first existing directory and check we can write there.
  while [ ! -d "$dir" ]; do dir=$(dirname "$dir"); done
  [ -w "$dir" ] || die "cannot write to $dir; choose another --prefix (no sudo needed for \$HOME/.local)"
  ok "$PREFIX is writable"
  if [ -x "$PREFIX/bin/seqc" ]; then
    info "Existing installation found: $("$PREFIX/bin/seqc" --version 2>/dev/null | head -n 1 || echo unknown) (will be replaced)"
  fi
  local other
  other=$(command -v seqc 2>/dev/null || true)
  if [ -n "$other" ] && [ "$other" != "$PREFIX/bin/seqc" ]; then
    warn "another seqc is on your PATH at $other and may take precedence"
  fi
}

# ---------------------------------------------------------------------------
# Pre-built package
# ---------------------------------------------------------------------------

# Downloads $1 to $2. Returns non-zero on any HTTP or network error.
download() {
  local url=$1 dest=$2
  if have curl; then
    local secure=()
    # Never follow a redirect from https to plain http.
    if [[ "$url" == https://* ]]; then
      secure=(--proto '=https' --tlsv1.2)
    fi
    curl -fsSL "${secure[@]}" --retry 3 --retry-delay 2 --connect-timeout 20 \
      -o "$dest" "$url" </dev/null 2>>"$LOG_FILE"
  else
    wget -q --tries=3 --timeout=20 -O "$dest" "$url" </dev/null 2>>"$LOG_FILE"
  fi
}

# Installs the release package. Returns 1 when no usable package is available
# (the caller may then build from source). Integrity failures are fatal: a
# download that does not match its published checksum is never installed.
install_binary() {
  local base asset url_dir dir="$WORKDIR/release"
  base=$(releases_base_url)
  asset="seqc-linux-$ARCH.tar.gz"
  if [ -n "$REF" ]; then
    url_dir="$base/download/$REF"
  else
    url_dir="$base/latest/download"
  fi
  mkdir -p "$dir/extract"

  step "Downloading $asset (${REF:-latest release})"
  if ! download "$url_dir/SHA256SUMS" "$dir/SHA256SUMS"; then
    warn "no release found at $url_dir"
    return 1
  fi
  if ! download "$url_dir/$asset" "$dir/$asset"; then
    warn "the release has no $asset"
    return 1
  fi
  ok "downloaded from $url_dir"

  step "Verifying checksum"
  local expected actual
  expected=$(awk -v f="$asset" '$2 == f || $2 == "*" f { print $1; exit }' \
    "$dir/SHA256SUMS")
  [ -n "$expected" ] || die "SHA256SUMS has no entry for $asset; refusing to install"
  actual=$(sha256sum "$dir/$asset" | awk '{ print $1 }')
  if [ "$expected" != "$actual" ]; then
    die "checksum mismatch for $asset (expected $expected, got $actual); refusing to install"
  fi
  ok "sha256 $actual"

  local package="$dir/extract/seqc-linux-$ARCH" required
  tar -xzf "$dir/$asset" -C "$dir/extract" </dev/null ||
    die "cannot extract $asset"
  for required in bin/seqc lib/seqc/lib/libseqrt.a lib/seqc/include/seq_runtime.h \
    lib/seqc/templates/gitignore.template lib/seqc/templates/Makefile.template; do
    [ -f "$package/$required" ] || die "$asset does not contain $required"
  done
  if ! "$package/bin/seqc" --version </dev/null >/dev/null 2>&1; then
    warn "the pre-built binary does not run on this system"
    return 1
  fi

  step "Installing into $PREFIX"
  # install(1) replaces each file, so a running seqc does not block upgrades.
  local files=() file mode manifest=""
  mapfile -t files < <(cd "$package" && find bin lib share -type f | LC_ALL=C sort)
  for file in "${files[@]}"; do
    mode=644
    [[ "$file" == bin/* ]] && mode=755
    install -D -m "$mode" "$package/$file" "$PREFIX/$file" ||
      die "cannot write $PREFIX/$file"
    manifest+="$PREFIX/$file"$'\n'
  done
  mkdir -p "$PREFIX/share/seqc"
  printf '%s' "$manifest" >"$PREFIX/share/seqc/install_manifest.txt"
  ok "$PREFIX/bin/seqc"
  ok "$PREFIX/lib/seqc"
  return 0
}

# ---------------------------------------------------------------------------
# Source build
# ---------------------------------------------------------------------------

fetch_source() {
  if [ -n "$SOURCE_DIR" ]; then
    step "Using local source"
    SRC="$SOURCE_DIR"
  else
    step "Cloning $REPO (${REF:-default branch})"
    SRC="$WORKDIR/src"
    local branch_args=()
    if [ -n "$REF" ]; then
      branch_args=(--branch "$REF")
    fi
    GIT_TERMINAL_PROMPT=0 run git clone --depth 1 "${branch_args[@]}" -- "$REPO" "$SRC"
    ok "cloned $(git -C "$SRC" rev-parse --short HEAD)"
  fi
  if [ ! -f "$SRC/CMakeLists.txt" ] || ! grep -q 'project(seq-lang' "$SRC/CMakeLists.txt"; then
    die "$SRC does not look like the seq-lang source tree"
  fi
  ok "source: $SRC"
}

build() {
  step "Configuring"
  BUILD_DIR="$WORKDIR/build"
  local tests=OFF compilers=("-DCMAKE_CXX_COMPILER=$CXX_BIN")
  if [ "$RUN_TESTS" -eq 1 ]; then
    tests=ON
  fi
  if [ -n "$CC_BIN" ]; then
    compilers+=("-DCMAKE_C_COMPILER=$CC_BIN")
  fi
  run cmake -S "$SRC" -B "$BUILD_DIR" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    "${compilers[@]}" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_TESTING="$tests"
  ok "configured (Release, $GENERATOR)"

  step "Building with $JOBS parallel jobs"
  run cmake --build "$BUILD_DIR" --parallel "$JOBS"
  ok "built $BUILD_DIR/bin/seqc"

  if [ "$RUN_TESTS" -eq 1 ]; then
    step "Running tests"
    run ctest --test-dir "$BUILD_DIR" --output-on-failure --parallel "$JOBS"
    ok "all tests passed"
  fi
}

install_built_files() {
  step "Installing into $PREFIX"
  run cmake --install "$BUILD_DIR" --strip
  # Keep the list of installed files so --uninstall removes exactly them.
  mkdir -p "$PREFIX/share/seqc"
  cp "$BUILD_DIR/install_manifest.txt" "$PREFIX/share/seqc/install_manifest.txt"
  # CMake does not end the manifest with a newline.
  echo >>"$PREFIX/share/seqc/install_manifest.txt"
  ok "$PREFIX/bin/seqc"
  ok "$PREFIX/lib/seqc"
}

# ---------------------------------------------------------------------------
# After installing
# ---------------------------------------------------------------------------

# Runs the installed compiler: its version, and a scaffold-and-check round
# trip that proves it finds its templates next to itself.
verify_install() {
  step "Verifying"
  local seqc="$PREFIX/bin/seqc" version probe="$WORKDIR/verify"
  version=$("$seqc" --version </dev/null | head -n 1) ||
    die "the installed binary does not run: $seqc"
  ok "$version"
  mkdir -p "$probe"
  # SEQC_HOME would point seqc at another installation.
  (cd "$probe" && unset SEQC_HOME SEQC_PATHS_HOME &&
    run "$seqc" new probe && run "$seqc" check probe/src/main.seq) ||
    die "the installed seqc cannot create and check a project (see --verbose)"
  ok "seqc new and seqc check work"
}

# seqc is installed at this point. What is still needed to compile and run
# workflows (GCC, the static C library, Google Test, llama.cpp, a recent
# kernel) is seqc's own knowledge, so ask it.
report_prerequisites() {
  step "Checking what workflows need (seqc doctor)"
  local output status=0 line
  output=$("$PREFIX/bin/seqc" doctor </dev/null 2>&1) || status=$?
  while IFS= read -r line; do
    info "$line"
  done <<<"$output"
  if [ "$status" -eq 0 ]; then
    ok "this host can compile and run workflows"
    return
  fi
  warn "seqc is installed, but this host cannot compile and run workflows yet"
  case " $(distro_id) " in
    *" ubuntu "* | *" debian "*)
      info "  sudo apt-get install -y build-essential libgtest-dev curl"
      ;;
  esac
  info "  llama-server comes from https://github.com/ggml-org/llama.cpp"
  info "  Then run: seqc doctor"
}

print_next_steps() {
  local bin_dir="$PREFIX/bin"
  printf '\n%sseqc is installed.%s\n\n' "$GREEN$BOLD" "$RESET" >&2

  case ":${PATH:-}:" in
    *":$bin_dir:"*) ;;
    *)
      local rc="\$HOME/.profile" line="export PATH=\"$bin_dir:\$PATH\""
      case "${SHELL:-}" in
        */bash) rc="\$HOME/.bashrc" ;;
        */zsh) rc="\$HOME/.zshrc" ;;
        */fish)
          rc="\$HOME/.config/fish/config.fish"
          line="fish_add_path $bin_dir"
          ;;
      esac
      printf '  %s%s is not on your PATH.%s Add it by running:\n\n' "$YELLOW" "$bin_dir" "$RESET" >&2
      printf "    echo '%s' >> \"%s\"\n\n" "$line" "$rc" >&2
      printf '  then open a new terminal.\n\n' >&2
      ;;
  esac

  printf '  Start a project:\n\n' >&2
  printf '    seqc new hello && cd hello\n' >&2
  printf '    seqc model pull      # downloads the model, about 1.1 GB\n' >&2
  printf '    seqc src/main.seq\n' >&2

  local prefix_arg=""
  if [ "$PREFIX" != "${HOME:-}/.local" ]; then
    prefix_arg=" --prefix $PREFIX"
  fi
  printf '\n  Uninstall with: install.sh --uninstall%s\n\n' "$prefix_arg" >&2
}

uninstall() {
  step "Uninstalling seqc from $PREFIX"
  local manifest="$PREFIX/share/seqc/install_manifest.txt"
  local files=()
  if [ -f "$manifest" ]; then
    mapfile -t files <"$manifest"
  else
    files=("$PREFIX/bin/seqc"
      "$PREFIX/lib/seqc/lib/libseqrt.a"
      "$PREFIX/lib/seqc/include/seq_runtime.h"
      "$PREFIX/lib/seqc/templates/gitignore.template"
      "$PREFIX/lib/seqc/templates/Makefile.template"
      "$PREFIX/share/doc/seqc/LICENSE"
      "$PREFIX/share/doc/seqc/THIRD_PARTY_NOTICES.md"
      "$PREFIX/share/doc/seqc/README.md")
  fi
  local removed=0 file
  for file in "${files[@]}"; do
    [ -n "$file" ] || continue
    # Only ever delete regular files inside the prefix.
    if [[ "$file" != "$PREFIX"/* ]] || [[ "$file" == *"/../"* ]]; then
      warn "skipping $file (outside $PREFIX)"
      continue
    fi
    if [ -f "$file" ] || [ -L "$file" ]; then
      rm -f -- "$file"
      ok "removed $file"
      removed=1
    fi
  done
  rm -f -- "$manifest"
  # Directories that only seqc uses; each goes only if it is empty.
  local dir
  for dir in lib/seqc/lib lib/seqc/include lib/seqc/templates lib/seqc \
    share/doc/seqc share/seqc; do
    rmdir -- "$PREFIX/$dir" 2>/dev/null || true
  done
  if [ "$removed" -eq 0 ]; then
    warn "nothing to remove; seqc is not installed in $PREFIX"
  else
    printf '\n%sseqc has been uninstalled.%s\n' "$GREEN$BOLD" "$RESET" >&2
    info "Downloaded models (~/.cache/seqc) and settings (~/.config/seqc) were left in place."
  fi
}

# ---------------------------------------------------------------------------

main() {
  setup_colors
  parse_args "$@"

  if [ "$UNINSTALL" -eq 1 ]; then
    uninstall
    return
  fi

  printf '%sseqc installer%s %s\n\n' "$BOLD" "$RESET" "$INSTALLER_VERSION" >&2
  WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/seqc-install.XXXXXX") ||
    die "cannot create a temporary directory"
  LOG_FILE="$WORKDIR/install.log"
  : >"$LOG_FILE"
  trap cleanup EXIT
  trap 'on_error $LINENO' ERR
  trap 'exit 130' INT TERM

  if [ -z "$JOBS" ]; then
    JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
  fi

  check_platform
  choose_method

  if [ "$CHECK_ONLY" -eq 1 ]; then
    local ready=1
    if [ "$USE_BINARY" -eq 1 ] && ! check_download_tools; then
      ready=0
    fi
    if [ "$MODE" != "binary" ] && ! check_build_tools; then
      # Building is only a fallback when the package route is ready.
      if [ "$USE_BINARY" -eq 0 ]; then
        ready=0
      fi
    fi
    [ "$ready" -eq 1 ] || die "missing required tools; see above"
    printf '\n%sEverything needed to install seqc is available.%s\n' "$GREEN$BOLD" "$RESET" >&2
    return
  fi

  check_prefix_writable
  if [ "$USE_BINARY" -eq 1 ]; then
    if check_download_tools && install_binary; then
      verify_install
      report_prerequisites
      print_next_steps
      return
    fi
    if [ "$MODE" = "binary" ]; then
      die "could not install a pre-built package (--binary was given)"
    fi
    warn "falling back to building from source"
  fi

  check_build_tools || die "missing build tools; install them and run the installer again"
  fetch_source
  build
  install_built_files
  verify_install
  report_prerequisites
  print_next_steps
}

main "$@"
