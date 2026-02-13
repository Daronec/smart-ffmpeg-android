# Release Process Visualization

## 🔄 Complete Release Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                     DEVELOPER ACTIONS                            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌──────────────────┐
                    │  Update Version  │
                    │  in build.gradle │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │   Run Tests      │
                    │   ./gradlew test │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │  Create Commit   │
                    │  & Tag           │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │  Push to GitHub  │
                    │  git push tag    │
                    └────────┬─────────┘
                             │
┌────────────────────────────┴────────────────────────────┐
│                   GITHUB ACTIONS                         │
└──────────────────────────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────┐
        │  Workflow: release.yml                 │
        ├────────────────────────────────────────┤
        │  1. Checkout code                      │
        │  2. Setup JDK 17                       │
        │  3. Setup Android SDK                  │
        │  4. Extract version from tag           │
        │  5. Update build.gradle                │
        │  6. Run tests                          │
        │  7. Build library (AAR)                │
        │  8. Generate sources JAR               │
        └────────────────┬───────────────────────┘
                         │
         ┌───────────────┼───────────────┬───────────────┐
         │               │               │               │
         ▼               ▼               ▼               ▼
┌────────────────┐ ┌────────────┐ ┌────────────┐ ┌────────────┐
│ GitHub         │ │ GitHub     │ │ JitPack    │ │ Notify     │
│ Packages       │ │ Release    │ │ Build      │ │ Users      │
└────────────────┘ └────────────┘ └────────────┘ └────────────┘
         │               │               │               │
         ▼               ▼               ▼               ▼
┌────────────────┐ ┌────────────┐ ┌────────────┐ ┌────────────┐
│ Publish:       │ │ Create:    │ │ Trigger:   │ │ Comment:   │
│ • AAR          │ │ • Release  │ │ • API call │ │ • Install  │
│ • POM          │ │ • Upload   │ │ • Build    │ │   guide    │
│ • Sources JAR  │ │   AAR      │ │   library  │ │ • Links    │
│                │ │ • Upload   │ │            │ │            │
│ Auth: Token    │ │   sources  │ │ No auth    │ │            │
│                │ │ • Changelog│ │            │ │            │
└────────────────┘ └────────────┘ └────────────┘ └────────────┘
         │               │               │               │
         └───────────────┴───────────────┴───────────────┘
                         │
                         ▼
                ┌────────────────┐
                │  ✅ SUCCESS    │
                │  Notification  │
                └────────────────┘
                         │
                         ▼
┌────────────────────────────────────────────────────────────────┐
│                     USERS CAN INSTALL                           │
└────────────────────────────────────────────────────────────────┘
```

## 📦 Three Distribution Channels

### 1. JitPack (Recommended)

```
✅ No credentials required
✅ Automatic builds
✅ Maven Central compatible
✅ Fast CDN delivery

implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.5'
```

### 2. GitHub Packages

```
⚠️ Requires GitHub token
✅ Official GitHub hosting
✅ Private packages support
✅ Organization control

implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.5'
```

### 3. GitHub Releases

```
✅ Direct download
✅ Manual installation
✅ Backup option
✅ Changelog included

Download: smart-ffmpeg-android-1.0.5.aar
```

## ⏱️ Timeline

```
T+0s    Developer pushes tag
T+5s    GitHub Actions triggered
T+10s   Checkout & setup complete
T+30s   Tests running
T+60s   Build complete
T+90s   Publishing to GitHub Packages
T+120s  GitHub Release created
T+125s  JitPack API called
T+130s  Workflow complete ✅

T+5m    JitPack build complete ✅
```

## 🎯 Success Criteria

After workflow completes, verify:

- [ ] GitHub Actions shows green ✅
- [ ] GitHub Packages has new version
- [ ] GitHub Release is created with artifacts
- [ ] JitPack shows green build status
- [ ] Comment added to commit with instructions

## 🔐 Security & Permissions

### GitHub Actions Permissions

```yaml
permissions:
  contents: write # For creating releases
  packages: write # For publishing to GitHub Packages
```

### Secrets Used

- `GITHUB_TOKEN` - Automatically provided by GitHub
  - No manual configuration needed
  - Scoped to repository
  - Expires after workflow

### User Credentials

- **JitPack**: None required ✅
- **GitHub Packages**: GitHub token required ⚠️
- **GitHub Releases**: None required ✅

## 📊 Comparison Matrix

| Feature          | JitPack | GitHub Packages | GitHub Releases |
| ---------------- | ------- | --------------- | --------------- |
| Auto-build       | ✅      | ✅              | ❌              |
| No credentials   | ✅      | ❌              | ✅              |
| Maven compatible | ✅      | ✅              | ❌              |
| Direct download  | ❌      | ❌              | ✅              |
| Private repos    | 💰      | ✅              | ✅              |
| CDN delivery     | ✅      | ✅              | ✅              |
| Build logs       | ✅      | ✅              | N/A             |
| Recommended      | ⭐⭐⭐  | ⭐⭐            | ⭐              |

## 🚀 Quick Commands

### Create Release

```bash
./release.sh 1.0.6
```

### Manual Release

```bash
git tag 1.0.6
git push origin 1.0.6
```

### Check Status

```bash
# GitHub Actions
open https://github.com/Daronec/smart-ffmpeg-android/actions

# JitPack
open https://jitpack.io/#Daronec/smart-ffmpeg-android/1.0.6

# GitHub Release
open https://github.com/Daronec/smart-ffmpeg-android/releases
```

### Rollback

```bash
# Delete tag
git tag -d 1.0.6
git push origin :refs/tags/1.0.6

# Delete release manually on GitHub
```

## 📝 Changelog Generation

Automatic changelog from git commits:

```
## Release 1.0.6

- Add new metadata fields (a1b2c3d)
- Fix thumbnail extraction bug (d4e5f6g)
- Update documentation (g7h8i9j)
- Improve error handling (j0k1l2m)
```

## 🎉 Post-Release

After successful release:

1. ✅ Update README with new version
2. ✅ Announce on social media
3. ✅ Update documentation
4. ✅ Close related issues
5. ✅ Plan next release

---

**Last Updated:** 2026-02-13  
**Version:** 1.0  
**Status:** ✅ Active
