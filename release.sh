#!/bin/bash

# Smart FFmpeg Android - Release Script
# Usage: ./release.sh <version>
# Example: ./release.sh 1.0.6

set -e  # Exit on error

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Functions
print_info() {
    echo -e "${BLUE}ℹ️  $1${NC}"
}

print_success() {
    echo -e "${GREEN}✅ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠️  $1${NC}"
}

print_error() {
    echo -e "${RED}❌ $1${NC}"
}

# Check if version is provided
if [ -z "$1" ]; then
    print_error "Version not provided!"
    echo "Usage: ./release.sh <version>"
    echo "Example: ./release.sh 1.0.6"
    exit 1
fi

VERSION=$1

# Validate version format (semantic versioning)
if ! [[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    print_error "Invalid version format: $VERSION"
    echo "Version must follow semantic versioning: MAJOR.MINOR.PATCH"
    echo "Example: 1.0.6"
    exit 1
fi

print_info "Starting release process for version $VERSION"
echo ""

# Check if git is clean
if [ -n "$(git status --porcelain)" ]; then
    print_warning "Working directory is not clean!"
    git status --short
    echo ""
    read -p "Continue anyway? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_info "Release cancelled"
        exit 1
    fi
fi

# Check if tag already exists
if git rev-parse "$VERSION" >/dev/null 2>&1; then
    print_error "Tag $VERSION already exists!"
    echo "Use a different version or delete the existing tag:"
    echo "  git tag -d $VERSION"
    echo "  git push origin :refs/tags/$VERSION"
    exit 1
fi

# Update version in build.gradle
print_info "Updating version in build.gradle..."
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    sed -i '' "s/version = '[^']*'/version = '$VERSION'/" build.gradle
else
    # Linux
    sed -i "s/version = '[^']*'/version = '$VERSION'/" build.gradle
fi

# Verify the change
if grep -q "version = '$VERSION'" build.gradle; then
    print_success "Version updated in build.gradle"
else
    print_error "Failed to update version in build.gradle"
    exit 1
fi

# Run tests
print_info "Running tests..."
if ./gradlew test --quiet; then
    print_success "All tests passed"
else
    print_error "Tests failed!"
    echo "Fix the tests before releasing"
    exit 1
fi

# Build library
print_info "Building library..."
if ./gradlew assembleRelease --quiet; then
    print_success "Library built successfully"
else
    print_error "Build failed!"
    exit 1
fi

# Show what will be committed
echo ""
print_info "Changes to be committed:"
git diff build.gradle

echo ""
read -p "Commit these changes? (y/N): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    print_info "Release cancelled"
    # Restore build.gradle
    git checkout build.gradle
    exit 1
fi

# Commit changes
print_info "Committing changes..."
git add build.gradle
git commit -m "Release $VERSION"
print_success "Changes committed"

# Create tag
print_info "Creating tag $VERSION..."
git tag -a "$VERSION" -m "Release $VERSION"
print_success "Tag created"

# Push to remote
echo ""
print_warning "Ready to push to remote!"
echo "This will:"
echo "  1. Push commit to main branch"
echo "  2. Push tag $VERSION"
echo "  3. Trigger GitHub Actions workflow"
echo "  4. Publish to GitHub Packages"
echo "  5. Create GitHub Release"
echo "  6. Trigger JitPack build"
echo ""
read -p "Push to remote? (y/N): " -n 1 -r
echo

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    print_warning "Release not pushed to remote"
    echo "To push manually:"
    echo "  git push origin main"
    echo "  git push origin $VERSION"
    exit 0
fi

# Push commit
print_info "Pushing commit to main..."
git push origin main
print_success "Commit pushed"

# Push tag
print_info "Pushing tag $VERSION..."
git push origin "$VERSION"
print_success "Tag pushed"

echo ""
print_success "🎉 Release $VERSION initiated!"
echo ""
print_info "Next steps:"
echo "  1. Check GitHub Actions: https://github.com/Daronec/smart-ffmpeg-android/actions"
echo "  2. Wait for workflow to complete (~5 minutes)"
echo "  3. Check GitHub Release: https://github.com/Daronec/smart-ffmpeg-android/releases/tag/$VERSION"
echo "  4. Check JitPack: https://jitpack.io/#Daronec/smart-ffmpeg-android/$VERSION"
echo ""
print_info "Installation:"
echo "  implementation 'com.github.Daronec:smart-ffmpeg-android:$VERSION'"
echo ""
