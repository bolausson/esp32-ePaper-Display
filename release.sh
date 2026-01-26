#!/bin/bash
#
# release.sh - Automated release script for ESP32-S3-Display
#
# Usage:
#   ./release.sh <version>        # e.g., ./release.sh 0.2.1
#   ./release.sh --bump patch     # Auto-bump patch version
#   ./release.sh --bump minor     # Auto-bump minor version
#   ./release.sh --bump major     # Auto-bump major version
#
# Options:
#   --dry-run    Show what would be done without making changes
#   --no-push    Create tag but don't push or create GitHub release
#   --force      Skip confirmation prompts
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
PROJECT_NAME="esp32-s3-display"
BUILD_DIR=".pio/build/freenove_esp32_s3_wroom"
PLATFORMIO_CMD="${PLATFORMIO_CMD:-platformio}"
ASSETS=(
    "${BUILD_DIR}/firmware.bin"
    "${BUILD_DIR}/bootloader.bin"
    "${BUILD_DIR}/partitions.bin"
    "flash_instructions.txt"
    "LICENSE"
    "README.md"
)

# Flags
DRY_RUN=false
NO_PUSH=false
FORCE=false
BUMP_TYPE=""
VERSION=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --dry-run) DRY_RUN=true; shift ;;
        --no-push) NO_PUSH=true; shift ;;
        --force) FORCE=true; shift ;;
        --bump) BUMP_TYPE="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS] <version>"
            echo ""
            echo "Arguments:"
            echo "  version          Version to release (e.g., 0.2.1)"
            echo ""
            echo "Options:"
            echo "  --bump TYPE      Auto-bump version (major|minor|patch)"
            echo "  --dry-run        Show what would be done without changes"
            echo "  --no-push        Create tag locally but don't push"
            echo "  --force          Skip confirmation prompts"
            echo "  -h, --help       Show this help"
            exit 0
            ;;
        *) VERSION="$1"; shift ;;
    esac
done

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
    if [[ -n "$2" ]]; then
        echo -e "${RED}        Details:${NC}"
        echo "$2" | sed 's/^/        /'
    fi
    exit 1
}

get_current_version() {
    # Get latest version tag, strip 'v' prefix
    git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || echo "0.0.0"
}

bump_version() {
    local current="$1" type="$2"
    IFS='.' read -r major minor patch <<< "$current"
    case $type in
        major) echo "$((major + 1)).0.0" ;;
        minor) echo "${major}.$((minor + 1)).0" ;;
        patch) echo "${major}.${minor}.$((patch + 1))" ;;
        *) log_error "Invalid bump type: $type (use major|minor|patch)" ;;
    esac
}

validate_version() {
    local version="$1"
    [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || \
        log_error "Invalid version format: $version (expected X.Y.Z)"
}

check_prerequisites() {
    log_info "Checking prerequisites..."

    # Check required commands
    for cmd in git gh; do
        if ! command -v $cmd &> /dev/null; then
            log_error "$cmd is required but not installed" \
                "Install $cmd and ensure it's in your PATH"
        fi
    done

    # Check PlatformIO
    if ! command -v $PLATFORMIO_CMD &> /dev/null; then
        # Try Windows paths (MINGW64 uses different user variable)
        local win_user="${USERNAME:-$USER}"
        if [[ -f "/c/Users/${win_user}/.platformio/penv/Scripts/platformio.exe" ]]; then
            PLATFORMIO_CMD="/c/Users/${win_user}/.platformio/penv/Scripts/platformio.exe"
        elif [[ -f "$HOME/.platformio/penv/Scripts/platformio.exe" ]]; then
            PLATFORMIO_CMD="$HOME/.platformio/penv/Scripts/platformio.exe"
        elif [[ -f "$USERPROFILE/.platformio/penv/Scripts/platformio.exe" ]]; then
            PLATFORMIO_CMD="$USERPROFILE/.platformio/penv/Scripts/platformio.exe"
        else
            log_error "PlatformIO is required but not found" \
                "Install PlatformIO or set PLATFORMIO_CMD environment variable"
        fi
    fi

    # Check we're in project root
    if [[ ! -f "platformio.ini" ]]; then
        log_error "Must be run from project root (platformio.ini not found)" \
            "Current directory: $(pwd)"
    fi

    # Check for uncommitted changes
    local dirty_files=$(git status --porcelain)
    if [[ -n "$dirty_files" ]]; then
        log_error "Working directory is not clean. Commit or stash changes first." \
            "$dirty_files"
    fi

    # Check branch
    local branch=$(git branch --show-current)
    if [[ "$branch" != "main" && "$branch" != "master" ]]; then
        log_warn "Not on main/master branch (currently on: $branch)"
        if [[ "$FORCE" != "true" ]]; then
            read -p "Continue anyway? [y/N] " -n 1 -r; echo
            [[ $REPLY =~ ^[Yy]$ ]] || exit 1
        fi
    fi
    log_success "Prerequisites check passed"
}

check_tag_exists() {
    local tag="v$1"
    if git rev-parse "$tag" >/dev/null 2>&1; then
        local tag_commit=$(git rev-parse "$tag")
        local tag_date=$(git log -1 --format=%ci "$tag" 2>/dev/null || echo "unknown")
        log_error "Tag $tag already exists" \
            "Commit: $tag_commit
Created: $tag_date
Use a different version number or delete the existing tag with:
  git tag -d $tag && git push origin :refs/tags/$tag"
    fi
}

build_release() {
    log_info "Building release with PlatformIO..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[DRY-RUN] Would run: $PLATFORMIO_CMD run"; return
    fi
    $PLATFORMIO_CMD run
    log_success "Build completed"
}

verify_assets() {
    log_info "Verifying release assets..."
    for asset in "${ASSETS[@]}"; do
        if [[ ! -f "$asset" ]]; then
            log_error "Required asset not found: $asset" \
                "Expected at: $(pwd)/$asset
Run '$PLATFORMIO_CMD run' to build, or check that all required files exist."
        fi
        log_info "  ✓ $asset"
    done
    log_success "All assets verified"
}

# Get the release folder name for a version
get_release_folder() {
    local version="$1"
    local timestamp=$(date +%Y%m%d%H%M)
    echo "${PROJECT_NAME}_release-${timestamp}"
}

# Get the archive filename
get_archive_name() {
    local folder="$1"
    echo "${folder}.tar"
}

create_archive() {
    local version="$1"
    local release_folder=$(get_release_folder "$version")
    local archive_name=$(get_archive_name "$release_folder")

    log_info "Creating release archive: $archive_name"
    log_info "  Release folder: $release_folder/"

    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[DRY-RUN] Would create archive: $archive_name"
        log_info "[DRY-RUN] Contents:"
        for asset in "${ASSETS[@]}"; do
            log_info "  - $(basename "$asset")"
        done
        # Return the folder name for later use
        echo "$release_folder"
        return
    fi

    # Create release folder
    rm -rf "$release_folder"
    mkdir -p "$release_folder"

    # Copy assets to release folder
    for asset in "${ASSETS[@]}"; do
        cp "$asset" "$release_folder/"
    done

    # Create tar archive
    tar -cvf "$archive_name" "$release_folder"

    # Cleanup release folder (keep archive)
    rm -rf "$release_folder"

    log_success "Created archive: $archive_name"

    # Return the folder name for later use
    echo "$release_folder"
}

create_tag() {
    local version="$1"
    local tag="v$version"
    log_info "Creating git tag $tag..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[DRY-RUN] Would create tag $tag"
        return
    fi
    git tag -a "$tag" -m "Version $version"
    log_success "Created tag $tag"
}

push_to_remote() {
    local tag="v$1"
    if [[ "$NO_PUSH" == "true" ]]; then
        log_warn "Skipping push (--no-push specified)"; return
    fi
    log_info "Pushing to remote..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[DRY-RUN] Would push tag $tag"; return
    fi
    git push origin "$tag"
    log_success "Pushed to remote"
}

create_github_release() {
    local version="$1"
    local release_folder="$2"
    local tag="v$version"
    local archive_name=$(get_archive_name "$release_folder")

    if [[ "$NO_PUSH" == "true" ]]; then
        log_warn "Skipping GitHub release (--no-push specified)"; return
    fi
    log_info "Creating GitHub release $tag..."

    # Generate release notes from commits since last tag
    local last_tag=$(git describe --tags --abbrev=0 HEAD^ 2>/dev/null || echo "")
    local release_notes=""
    if [[ -n "$last_tag" ]]; then
        release_notes=$(git log "$last_tag"..HEAD --pretty=format:"- %s" --no-merges)
    else
        release_notes="Initial release"
    fi

    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[DRY-RUN] Would create GitHub release with:"
        log_info "  Tag: $tag"
        log_info "  Archive: $archive_name"
        log_info "  Release notes:"
        echo "$release_notes"
        return
    fi

    # Verify archive exists
    if [[ ! -f "$archive_name" ]]; then
        log_error "Archive not found: $archive_name" \
            "Expected at: $(pwd)/$archive_name
The create_archive step may have failed. Check for errors above."
    fi

    gh release create "$tag" \
        --title "$tag" \
        --notes "$release_notes" \
        --latest \
        "$archive_name"

    log_success "GitHub release created: $tag"
}

show_summary() {
    local version="$1"
    echo ""
    echo "=========================================="
    echo "  Release Summary"
    echo "=========================================="
    echo "  Version:  $version"
    echo "  Tag:      v$version"
    echo "  Assets:"
    for asset in "${ASSETS[@]}"; do
        echo "    - $asset"
    done
    echo "=========================================="
    echo ""
}

main() {
    echo ""
    echo "=========================================="
    echo "  ESP32-S3-Display Release Script"
    echo "=========================================="
    echo ""

    # Determine version
    local current_version=$(get_current_version)
    log_info "Current version: $current_version"

    if [[ -n "$BUMP_TYPE" ]]; then
        VERSION=$(bump_version "$current_version" "$BUMP_TYPE")
        log_info "Bumping $BUMP_TYPE version: $current_version -> $VERSION"
    fi

    if [[ -z "$VERSION" ]]; then
        log_error "No version specified. Use: $0 <version> or $0 --bump <type>"
    fi

    validate_version "$VERSION"

    # Show what we're going to do
    show_summary "$VERSION"

    # Confirmation
    if [[ "$FORCE" != "true" && "$DRY_RUN" != "true" ]]; then
        read -p "Proceed with release? [y/N] " -n 1 -r
        echo
        [[ $REPLY =~ ^[Yy]$ ]] || exit 1
    fi

    # Execute release steps
    check_prerequisites
    check_tag_exists "$VERSION"
    build_release
    verify_assets
    local release_folder=$(create_archive "$VERSION")
    create_tag "$VERSION"
    push_to_remote "$VERSION"
    create_github_release "$VERSION" "$release_folder"

    echo ""
    log_success "Release v$VERSION completed successfully!"
    echo ""
}

main "$@"

