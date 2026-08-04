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
   - checks the repo out **first**, before any network step, so a bad checkout
     costs seconds rather than the whole dependency install,
   - installs deps (yay, `linux-wallpaperengine-git` for the `/opt` runtime, and
     Quickshell's makedepends extracted from its PKGBUILD) — every network call
     wrapped in `ci-retry`, so one 5xx from a mirror or the AUR does not throw
     away the build,
   - runs `scripts/build-we.sh`,
   - runs `scripts/package-we.sh` → `qs-wallpaperengine-vX.Y.Z-x86_64.tar.zst` +
     `manifest.json` + `SHA256SUMS`, and records `build-env.txt`. Packaging also
     rewrites the shipped binary's RUNPATH to `$ORIGIN/../lib` and **refuses to
     produce a tarball if it is anything else** — the build bakes in the
     builder's own directory (`/__w/...` in CI), which exists on no user's
     machine, and that shipped in every release up to 0.2.2 before anything
     noticed. `patchelf` is a hard dependency of this job for that reason,
   - **uploads `out/` as a workflow artifact** before anything that can still
     fail, so the build survives a failed smoke test or a failed publish. The
     upload is `continue-on-error`: it is a safety net, not a deliverable, so if
     the artifact service itself is down the release still publishes — you just
     do not get the second copy,
   - smoke-tests the packaged binary (`quickshell --version`),
   - creates the release as a **draft**, uploads all four assets with
     `--clobber`, and only then flips it to published — so the release is never
     visible with assets missing.

   You can also start it by hand via **workflow_dispatch** (Actions → release →
   Run workflow → enter the tag). Note this is *not* a dry run: it builds,
   packages **and publishes** the tag you type. The tag must be `v` followed by
   `[A-Za-z0-9._-]` — the same character class `package-we.sh` enforces, checked
   in the job's first step rather than at minute 35.

4. Point the installer at the new tag: set the `WE_REF` default to `vX.Y.Z` in
   imi-unify's `sdata/subcmd-install/4.wallpaperengine.sh`. Installs now fetch
   the prebuilt; any checksum / arch / Qt-too-old / smoke-test failure falls back
   to a local source compile — no install is ever worse off than before.

## When a release run fails

The job is built so that **re-running it is always the right first move**, and so
that a rebuild is never needed just to finish a publish.

- **Failed after Package** (smoke test, or any publish step): the tarball,
  `manifest.json`, `SHA256SUMS` and `build-env.txt` are already saved as the
  workflow artifact `qs-wallpaperengine-vX.Y.Z` (14-day retention). Download it
  from the run's summary page; `sha256sum -c SHA256SUMS` works directly inside
  it. If the upload step itself is the one showing a warning, there is no saved
  copy for that run — re-run the job.
- **Re-run the failed job.** Every step converges: `gh release create` is guarded
  by `gh release view`, so it does not die on "release already exists";
  `gh release upload --clobber` overwrites the assets that made it up on the
  first attempt; and the final `gh release edit --draft=false` is a no-op on an
  already-published release. You no longer have to delete the release by hand
  first.
- **Publish by hand from the saved artifact**, if you would rather not spend
  another 35 minutes:
  ```bash
  gh release create vX.Y.Z --draft --title vX.Y.Z --notes "..."   # only if absent
  gh release upload vX.Y.Z --clobber qs-wallpaperengine-vX.Y.Z-x86_64.tar.zst \
      manifest.json SHA256SUMS build-env.txt
  gh release edit vX.Y.Z --draft=false
  ```
- **A `::warning::` that `linux-wallpaperengine-git` does not contain
  `WE_COMMIT`** means the AUR package has moved ahead of the commit
  `bootstrap.sh` pins. The release still cuts, but the bundled WE lib and the
  `/opt` runtime it loads against no longer come from the same commit — an ABI
  mismatch that surfaces as a crash on a user's machine, not in CI. Bump
  `WE_COMMIT` in `bootstrap.sh` to match the package and re-cut.

## Artifact contract

- `manifest.json`: `{ schema, version, commit, qt_min, arch, built_at, files[] }`.
  `qt_min` is the build host's `qt6-base` version; the installer refuses the
  prebuilt when the target host's Qt is older. `arch` must match the target.
- `SHA256SUMS` covers the tarball + `manifest.json`; the installer runs
  `sha256sum -c` **before** extracting anything.
- `build-env.txt`: provenance for the build — the tag and commit, the
  `QS_COMMIT`/`WE_COMMIT` pins from `bootstrap.sh` alongside the commits the
  build clones actually ended up on, and the full `pacman -Q` of the build
  container. The two AUR `-git` packages cannot be pinned (`yay -S` has no
  revision argument), so this is what makes a build reproducible in hindsight.
  **Not** covered by `SHA256SUMS` — it is metadata, not something the installer
  verifies, and `sha256sum -c` ignores files the sums file does not list.
- Tarball layout: `bin/quickshell` + `lib/*.so*` (bundled CEF/EGL/swiftshader +
  `liblinux-wallpaperengine-lib.so`). The system `linux-wallpaperengine-git`
  package (`/opt/linux-wallpaperengine`) is still a runtime prerequisite.
