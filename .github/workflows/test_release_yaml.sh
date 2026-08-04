#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
y="$here/release.yml"
[[ -f "$y" ]] || { echo "FAIL: release.yml missing"; exit 1; }

# Checked separately from the parse below, and before it, because conflating the
# two burned a CI run: with PyYAML absent the parse died on ModuleNotFoundError,
# the `||` caught that exit status like any other, and the script reported
# "FAIL: invalid YAML" about a file that was perfectly well-formed. The diagnosis
# that message invites - go read release.yml - is a dead end, because there is
# nothing wrong with release.yml. Every other check in here also imports yaml, so
# one probe up front covers all of them.
python3 -c 'import yaml' 2>/dev/null || {
	echo "FAIL: PyYAML not installed (python3 -c 'import yaml' failed)"
	echo "      This says NOTHING about release.yml - install python-yaml/PyYAML and re-run."
	exit 1
}
python3 -c 'import sys,yaml; yaml.safe_load(open(sys.argv[1]))' "$y" || { echo "FAIL: invalid YAML"; exit 1; }
grep -q "tags:" "$y"              || { echo "FAIL: no tag trigger"; exit 1; }
grep -q "workflow_dispatch" "$y"  || { echo "FAIL: no manual dispatch"; exit 1; }
grep -q "build-we.sh" "$y"        || { echo "FAIL: does not call build-we.sh"; exit 1; }
grep -q "package-we.sh" "$y"      || { echo "FAIL: does not call package-we.sh"; exit 1; }
grep -q "gh release create" "$y"  || { echo "FAIL: does not publish a release"; exit 1; }

# --- issue #8: a 35-minute build must survive any single failure -------------
# The job used to keep exactly one copy of its output (out/, inside the
# container) and take exactly one attempt at every network call, so any failure
# at or after packaging threw the whole build away. Everything below pins down
# the properties that stop that happening again. They are checked structurally
# rather than textually: plain greps are actively unsafe in this file because
# its comments quote the very commands they explain ("`yay -S` always builds
# upstream HEAD", "--clobber is what makes a re-run converge"), so a naive grep
# passes on the prose alone even after the command it names has been deleted.
# Two rounds of mutation testing found exactly that. Every check below therefore
# parses the YAML and reads each step's script with full-line comments stripped
# and backslash continuations joined - joined because "Runtime + build deps"
# holds two independent AUR calls and a per-step check would let one of them go
# unretried.
python3 - "$y" <<'PY'
import sys, yaml
steps = yaml.safe_load(open(sys.argv[1]))["jobs"]["build"]["steps"]

def lines(s):
    """Code lines of a step's script: comments dropped, continuations joined."""
    out, acc = [], ""
    for l in (s.get("run") or "").splitlines():
        if l.lstrip().startswith("#"):
            continue
        acc += l.rstrip()
        if acc.endswith("\\"):
            acc = acc[:-1]
            continue
        out.append(acc); acc = ""
    if acc:
        out.append(acc)
    return out

def code(s):
    return "\n".join(lines(s)) + "\n" + (s.get("uses") or "")

def find(pred, what):
    for i, s in enumerate(steps):
        if pred(s):
            return i
    print("FAIL: no step " + what); sys.exit(1)

def fail(msg):
    print("FAIL: " + msg); sys.exit(1)

checkout = find(lambda s: "actions/checkout" in (s.get("uses") or ""), "checking out the repo")
upload   = find(lambda s: "actions/upload-artifact" in (s.get("uses") or ""), "uploading the build artifact")
package  = find(lambda s: "package-we.sh" in code(s), "running package-we.sh")
smoke    = find(lambda s: "unzstd" in code(s), "smoke-testing the packaged binary")
create   = find(lambda s: "gh release create" in code(s), "creating the release")
publish  = find(lambda s: "gh release upload" in code(s), "uploading the release assets")

# 1. Checkout before anything touches the network: a checkout failure must not
#    cost the dependency install first.
# `gh release upload` belongs here for the same reason the AUR calls do: it is a
# several-hundred-MB transfer and issue #8's fourth unretried network call. `gh
# release create` deliberately does NOT, because it is guarded by a `gh release
# view` and a retry around the create alone would be a retry around the wrong
# half of that pair.
NET = ("pacman -Syu", "aur.archlinux.org", "yay -S", "yay -G", "gh release upload")
net = [(i, l) for i, s in enumerate(steps) for l in lines(s) if any(t in l for t in NET)]
if not net:
    fail("no network dependency commands recognised, did they change shape?")
if checkout > min(i for i, _ in net):
    fail("checkout runs after a network step, so a checkout failure burns the dep install first")

# 2. Every network command retried, per command and not merely per step - one
#    step holds two independent AUR calls and covering only one is no cover.
for i, l in net:
    if "ci-retry" not in l:
        fail("network command in step %r is not wrapped in ci-retry: %s"
             % (steps[i].get("name"), l.strip()[:60]))
if not any("/usr/local/bin/ci-retry" in l for s in steps for l in lines(s)):
    fail("nothing installs /usr/local/bin/ci-retry, so every retried command above "
         "would die on 'ci-retry: command not found'")

# 3. The build output is saved before anything that can still fail, and saved
#    even when the step before it did fail.
if not package < upload < smoke < publish:
    fail("upload-artifact must sit between Package and the smoke test, otherwise "
         "a failed smoke test or publish still discards a 35-minute build")
# Compared as a literal, NOT with `"cancelled()" in ...`: that substring is just
# as happy with `if: ${{ cancelled() }}`, which uploads the build only when the
# run is cancelled and leaves the whole issue un-fixed. Mutation testing found
# exactly that inversion passing.
if str(steps[upload].get("if", "")).replace(" ", "") not in ("${{!cancelled()}}", "!cancelled()"):
    fail("upload-artifact's guard is not exactly `!cancelled()` (got %r): the default "
         "success(), or an inverted cancelled(), loses the build to a half-finished "
         "Package" % steps[upload].get("if"))
if steps[upload].get("continue-on-error") is not True:
    fail("upload-artifact without continue-on-error: a 503 from the artifact service "
         "on a several-hundred-MB payload fails the step, and Smoke test / Create / "
         "Publish are all success()-gated behind it, so a good build is never released")
if steps[upload].get("with", {}).get("overwrite") is not True:
    fail("upload-artifact without overwrite: re-running the failed job 409s on the "
         "artifact name, breaking the recovery path itself")
if (steps[upload].get("with", {}).get("path") or "").strip().strip("/") != "out":
    fail("upload-artifact does not save out/, so the artifact is not the build and "
         "the recovery path it exists for saves nothing")

# 4. Publishing converges on a re-run instead of demanding a hand-deleted release.
if "gh release view" not in code(steps[create]):
    fail("gh release create has no create-if-absent guard, a re-run dies on 'release already exists'")
if "--clobber" not in code(steps[publish]):
    fail("gh release upload without --clobber, a re-run dies on an already-uploaded asset")
for i in (create, publish):
    if "GH_REPO" not in (steps[i].get("env") or {}):
        fail("step %r does not set GH_REPO, so gh falls back to guessing the repo "
             "from a .git that checkout may not have left behind" % steps[i].get("name"))

# 4b. Draft first, publish last - and the flip must actually be there. This is
#     the single most consequential behaviour in the job and nothing used to
#     check it: delete the `gh release edit --draft=false` and every release
#     ships as an invisible draft, so the installer's WE_REF fetch 404s and
#     silently falls back to a 35-minute source compile on every user's machine.
if "--draft" not in code(steps[create]):
    fail("the release is not created as a draft, so a failed asset upload leaves a "
         "user-visible release with assets missing")
pub = lines(steps[publish])
up_i = next((i for i, l in enumerate(pub) if "gh release upload" in l), -1)
ed_i = next((i for i, l in enumerate(pub)
             if "gh release edit" in l and "--draft=false" in l), -1)
if ed_i < 0:
    fail("nothing in step %r flips the draft to published: every release would ship "
         "invisible and the installer would fall back to a source build for all of "
         "them (if the flip legitimately moved to its own step, move this check too)"
         % steps[publish].get("name"))
if ed_i < up_i:
    fail("the release is published before its assets are uploaded, so a failed upload "
         "leaves a visible release with assets missing")

# 4c. `|| anything` in the two publish steps swallows exactly the failures that
#     must fail the job. Checked here and not only as `|| true` below, because
#     `|| echo "oh well"` reads as harmless and does the same damage.
for i in (create, publish):
    for l in lines(steps[i]):
        if "||" in l:
            fail("step %r swallows a failure with '||': a publish that did not happen "
                 "must fail the job, not report green: %s"
                 % (steps[i].get("name"), l.strip()[:60]))

# 4d. The tag guard has to be at least as strict as package-we.sh's, or it does
#     not front-run it: `case "$tag" in v*)` accepts newlines and shell
#     metacharacters, which then reach $GITHUB_ENV as `name=value` appends.
# Both halves required on the SAME line: the error message quotes the character
# class too, so looking for the class alone passes on the diagnostic left behind
# after the test itself has been weakened back to `v*`.
if not any("=~" in l and "[A-Za-z0-9._-]" in l for s in steps for l in lines(s)):
    fail("no strict tag validation: a `v*` prefix test accepts newlines and shell "
         "metacharacters, which package-we.sh only rejects at minute 35 and which "
         "reach $GITHUB_ENV in between")

# 5. `|| true` turns a failed publish into a green run that shipped nothing. The
#    provenance step deliberately falls back with `|| echo unknown` instead.
for s in steps:
    if "|| true" in code(s):
        fail("'|| true' in step %r would swallow a real failure" % s.get("name"))

# 6. The dubious-ownership guard that killed v0.1.0 34 minutes in.
if not any("git config" in l and "safe.directory" in l for s in steps for l in lines(s)):
    fail("no `git config --global --add safe.directory`, this is what killed v0.1.0")

# 7. GITHUB_REF_NAME is set on every event - on workflow_dispatch it is the
#    branch the run was started from - so a `${GITHUB_REF_NAME:-<input>}`
#    fallback never reaches the typed-in tag and a manual run would build,
#    package and publish a release named after the branch.
if any("GITHUB_REF_NAME:-" in l for s in steps for l in lines(s)):
    fail("tag resolution falls back FROM GITHUB_REF_NAME, which is always set - a "
         "workflow_dispatch run would publish a release named after the branch")
PY

# --- issue #8: the retry helper itself, pulled out and RUN --------------------
# release.yml's own comment claims the helper "can be grepped - and actually run
# - by test_release_yaml.sh", and only the grep was ever true. Mutation testing
# walked straight through the gap: `exit "$rc"` -> `exit 0` makes ci-retry report
# a failed command as a success, so `ci-retry gh release upload` would go green
# on a failed upload and the very next line would publish a release missing its
# tarball - strictly worse than the bug the retry was added to fix. `"$@" && exit
# 0` -> `exit 0` is worse still: the command never runs at all, and a dependency
# install that never happened surfaces much later as a missing compiler. Neither
# mutation is visible to any grep, so the only honest check is to lift the script
# out of its heredoc and exercise it.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

python3 - "$y" "$tmp/ci-retry" <<'PY'
import sys, yaml
steps = yaml.safe_load(open(sys.argv[1]))["jobs"]["build"]["steps"]
body = None
for s in steps:
    run = s.get("run") or ""
    if "/usr/local/bin/ci-retry" not in run:
        continue
    src = run.splitlines()
    for i, l in enumerate(src):
        if not l.rstrip().endswith("<<'SH'"):
            continue
        for j in range(i + 1, len(src)):
            if src[j].strip() == "SH":
                body = "\n".join(src[i + 1:j]) + "\n"
                break
        break
    if body:
        break
if not body:
    print("FAIL: could not lift the ci-retry script out of release.yml's heredoc, so "
          "its behaviour is unchecked")
    sys.exit(1)
open(sys.argv[2], "w").write(body)
PY
chmod 755 "$tmp/ci-retry"

# Records every invocation and exits with whatever RC says, so each case below
# can pin down both how many times the command ran and what ci-retry returned.
cat > "$tmp/subject" <<'EOF'
#!/usr/bin/env bash
echo run >> "$COUNT"
exit "${RC:-0}"
EOF
chmod 755 "$tmp/subject"
export COUNT="$tmp/count"
runs() { wc -l < "$COUNT" | tr -d ' '; }

: > "$COUNT"; rc=0
RC=0 "$tmp/ci-retry" "$tmp/subject" || rc=$?
[[ "$rc" == 0 && "$(runs)" == 1 ]] || {
  echo "FAIL: ci-retry ran a succeeding command $(runs) time(s) and exited $rc, want 1 and 0"
  exit 1
}

: > "$COUNT"; rc=0
# stderr dropped only because ci-retry is expected to narrate its retries here
# and this script's output is meant to be one word; the assertion below is what
# actually reads the result.
RC=7 CI_RETRY_TRIES=3 CI_RETRY_DELAY=0 "$tmp/ci-retry" "$tmp/subject" 2>/dev/null || rc=$?
[[ "$rc" == 7 && "$(runs)" == 3 ]] || {
  echo "FAIL: ci-retry on a persistently failing command ran $(runs) time(s) and exited" \
       "$rc, want 3 and 7 - an exit 0 here reports a failed publish as a success"
  exit 1
}

: > "$COUNT"; rc=0
CI_RETRY_TRIES=abc "$tmp/ci-retry" "$tmp/subject" 2>/dev/null || rc=$?
[[ "$rc" != 0 && "$(runs)" == 0 ]] || {
  echo "FAIL: ci-retry with a non-numeric CI_RETRY_TRIES ran $(runs) time(s) and exited" \
       "$rc, want 0 and non-zero - a command that never ran must not look like a success"
  exit 1
}

echo "PASS"
