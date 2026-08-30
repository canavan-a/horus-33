# release/

Local signing material and the guided release tool for the Android app.

Tracked in git: `release.sh`, `README.md`, `.gitignore`.
**Never** committed: `release.keystore`, `keystore.env`, `HORUS_KEYSTORE_BASE64.txt`.

## Cutting a release

1. Fill the three passwords into `keystore.env` (from the GitHub Actions secrets
   `HORUS_KEYSTORE_PASSWORD` / `HORUS_KEY_ALIAS` / `HORUS_KEY_PASSWORD`). The
   keystore file itself is already on disk.
2. Run it:

   ```
   ./release/release.sh
   ```

The script: checks the toolchain, sources `keystore.env`, lists existing
`mobile-v*` tags, prompts for the new tag (e.g. `mobile-v0.3.2`) and a
`versionCode`, runs the real `./gradlew assembleRelease` with full output, then —
after a single confirmation — creates and pushes the tag and publishes a GitHub
Release with the signed APK attached.

Assumes `git`, `java`, the Android SDK/build-tools, and `gh` (authenticated) are
installed. Mirrors `.github/workflows/mobile-release.yml`; the only difference is
`versionCode` is entered by hand instead of using the CI run number.

The signed APK is also left at `release/horus-<tag>.apk` (gitignored).
