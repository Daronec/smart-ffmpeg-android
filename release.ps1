# Smart FFmpeg Android - Release Script (PowerShell)
# Usage: .\release.ps1 1.0.5
# Example: .\release.ps1 1.0.6

param(
    [Parameter(Mandatory=$true)]
    [string]$Version
)

$ErrorActionPreference = "Stop"

# Colors
function Write-Info { Write-Host "ℹ️  $args" -ForegroundColor Blue }
function Write-Success { Write-Host "✅ $args" -ForegroundColor Green }
function Write-Warning { Write-Host "⚠️  $args" -ForegroundColor Yellow }
function Write-Error { Write-Host "❌ $args" -ForegroundColor Red }

# Validate version format (semantic versioning)
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Error "Invalid version format: $Version"
    Write-Host "Version must follow semantic versioning: MAJOR.MINOR.PATCH"
    Write-Host "Example: 1.0.6"
    exit 1
}

Write-Info "Starting release process for version $Version"
Write-Host ""

# Check if git is clean
$gitStatus = git status --porcelain
if ($gitStatus) {
    Write-Warning "Working directory is not clean!"
    git status --short
    Write-Host ""
    $continue = Read-Host "Continue anyway? (y/N)"
    if ($continue -ne 'y' -and $continue -ne 'Y') {
        Write-Info "Release cancelled"
        exit 1
    }
}

# Check if tag already exists
try {
    git rev-parse $Version 2>$null
    Write-Error "Tag $Version already exists!"
    Write-Host "Use a different version or delete the existing tag:"
    Write-Host "  git tag -d $Version"
    Write-Host "  git push origin :refs/tags/$Version"
    exit 1
} catch {
    # Tag doesn't exist - good!
}

# Update version in build.gradle
Write-Info "Updating version in build.gradle..."
$buildGradle = Get-Content "build.gradle" -Raw
$buildGradle = $buildGradle -replace "version = '[^']*'", "version = '$Version'"
Set-Content "build.gradle" -Value $buildGradle -NoNewline

# Verify the change
if ((Get-Content "build.gradle" -Raw) -match "version = '$Version'") {
    Write-Success "Version updated in build.gradle"
} else {
    Write-Error "Failed to update version in build.gradle"
    exit 1
}

# Skip tests on Windows (will run in GitHub Actions)
Write-Warning "Skipping tests on Windows (will run in GitHub Actions)"
Write-Info "Tests will be executed automatically in CI/CD pipeline"

# Build library
Write-Info "Building library..."
try {
    .\gradlew.bat assembleRelease --quiet
    Write-Success "Library built successfully"
} catch {
    Write-Error "Build failed!"
    Write-Host $_.Exception.Message
    exit 1
}

# Show what will be committed
Write-Host ""
Write-Info "Changes to be committed:"
git diff build.gradle

Write-Host ""
$commit = Read-Host "Commit these changes? (y/N)"
if ($commit -ne 'y' -and $commit -ne 'Y') {
    Write-Info "Release cancelled"
    # Restore build.gradle
    git checkout build.gradle
    exit 1
}

# Commit changes
Write-Info "Committing changes..."
git add build.gradle
git commit -m "Release $Version"
Write-Success "Changes committed"

# Create tag
Write-Info "Creating tag $Version..."
git tag -a $Version -m "Release $Version"
Write-Success "Tag created"

# Push to remote
Write-Host ""
Write-Warning "Ready to push to remote!"
Write-Host "This will:"
Write-Host "  1. Push commit to main branch"
Write-Host "  2. Push tag $Version"
Write-Host "  3. Trigger GitHub Actions workflow"
Write-Host "  4. Publish to GitHub Packages"
Write-Host "  5. Create GitHub Release"
Write-Host "  6. Trigger JitPack build"
Write-Host ""
$push = Read-Host "Push to remote? (y/N)"

if ($push -ne 'y' -and $push -ne 'Y') {
    Write-Warning "Release not pushed to remote"
    Write-Host "To push manually:"
    Write-Host "  git push origin main"
    Write-Host "  git push origin $Version"
    exit 0
}

# Push commit
Write-Info "Pushing commit to main..."
git push origin main
Write-Success "Commit pushed"

# Push tag
Write-Info "Pushing tag $Version..."
git push origin $Version
Write-Success "Tag pushed"

Write-Host ""
Write-Success "🎉 Release $Version initiated!"
Write-Host ""
Write-Info "Next steps:"
Write-Host "  1. Check GitHub Actions: https://github.com/Daronec/smart-ffmpeg-android/actions"
Write-Host "  2. Wait for workflow to complete (~5 minutes)"
Write-Host "  3. Check GitHub Release: https://github.com/Daronec/smart-ffmpeg-android/releases/tag/$Version"
Write-Host "  4. Check JitPack: https://jitpack.io/#Daronec/smart-ffmpeg-android/$Version"
Write-Host ""
Write-Info "Installation:"
Write-Host "  implementation 'com.github.Daronec:smart-ffmpeg-android:$Version'"
Write-Host ""
