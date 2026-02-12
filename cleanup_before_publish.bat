@echo off
echo Cleaning temporary files before publishing...
echo.

REM Delete temporary documentation files
if exist "nul" del /q "nul"
if exist "COMPLETION_REPORT.md" del /q "COMPLETION_REPORT.md"
if exist "CURRENT_SITUATION.txt" del /q "CURRENT_SITUATION.txt"
if exist "FIX_NDK_ERROR.txt" del /q "FIX_NDK_ERROR.txt"
if exist "FIX_TOKEN_LEAK.md" del /q "FIX_TOKEN_LEAK.md"
if exist "PROJECT_STRUCTURE.md" del /q "PROJECT_STRUCTURE.md"
if exist "QUICK_FIX.txt" del /q "QUICK_FIX.txt"
if exist "QUICK_REFERENCE.md" del /q "QUICK_REFERENCE.md"
if exist "QUICKSTART.md" del /q "QUICKSTART.md"
if exist "START_HERE_WINDOWS.txt" del /q "START_HERE_WINDOWS.txt"
if exist "SUMMARY.md" del /q "SUMMARY.md"
if exist "WINDOWS_QUICKSTART.md" del /q "WINDOWS_QUICKSTART.md"

REM Keep these important files:
REM - START_HERE.md (main entry point)
REM - FINAL_CHECKLIST.md (publishing guide)
REM - PRE_PUBLISH_CHECKLIST.md (pre-publish checks)
REM - PUBLISH.md (publishing instructions)
REM - README.md (project documentation)
REM - README_GITHUB.md (GitHub version)
REM - CHANGELOG.md (version history)
REM - SECURITY.md (security policy)
REM - LICENSE (LGPL 2.1)

echo.
echo ✅ Cleanup complete!
echo.
echo Next steps:
echo 1. Run: gradlew clean assembleRelease
echo 2. Create GitHub repository
echo 3. Run: git init
echo 4. Run: git add .
echo 5. Run: git commit -m "Initial commit: Smart FFmpeg Android v1.0.0"
echo 6. Run: git remote add origin https://github.com/Daronec/smart-ffmpeg-android.git
echo 7. Run: git push -u origin main
echo.
pause
