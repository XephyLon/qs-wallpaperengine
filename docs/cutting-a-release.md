# Cutting a qs-wallpaperengine release

The immaterial-impulse installer prefers a **prebuilt** release tarball for its
Wallpaper Engine option and only compiles from source when no matching prebuilt
is available. Cutting a release is what produces that tarball.

1. Ensure `main` builds locally:
   ```bash
   REPO_ROOT="$PWD" bash scripts/build-we.sh    # prints QS_BIN=/WE_LIB_DIR= on stdout
   ```
   Run the built binary with the WE libs on `LD_LIBRARY_PATH` to sanity-check it
   (see `launch-shell.sh`).

2. Tag and push:
   ```bash
   git tag vX.Y.Z && git push origin vX.Y.Z
   ```

3. `.github/workflows/release.yml` fires on the `v*` tag. In an Arch container it:
   - installs deps (yay, `linux-wallpaperengine-git` for the `/opt` runtime, and
     Quickshell's makedepends extracted from its PKGBUILD),
   - runs `scripts/build-we.sh`,
   - runs `scripts/package-we.sh` → `qs-wallpaperengine-vX.Y.Z-x86_64.tar.zst` +
     `manifest.json` + `SHA256SUMS`,
   - smoke-tests the packaged binary (`quickshell --version`),
   - publishes the GitHub Release with all three assets.

   You can dry-run it first via **workflow_dispatch** (Actions → release → Run
   workflow → enter the tag).

4. Point the installer at the new tag: set the `WE_REF` default to `vX.Y.Z` in
   imi-unify's `sdata/subcmd-install/4.wallpaperengine.sh`. Installs now fetch
   the prebuilt; any checksum / arch / Qt-too-old / smoke-test failure falls back
   to a local source compile — no install is ever worse off than before.

## Artifact contract

- `manifest.json`: `{ schema, version, commit, qt_min, arch, built_at, files[] }`.
  `qt_min` is the build host's `qt6-base` version; the installer refuses the
  prebuilt when the target host's Qt is older. `arch` must match the target.
- `SHA256SUMS` covers the tarball + `manifest.json`; the installer runs
  `sha256sum -c` **before** extracting anything.
- Tarball layout: `bin/quickshell` + `lib/*.so*` (bundled CEF/EGL/swiftshader +
  `liblinux-wallpaperengine-lib.so`). The system `linux-wallpaperengine-git`
  package (`/opt/linux-wallpaperengine`) is still a runtime prerequisite.
