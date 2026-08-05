#!/usr/bin/env python3
"""Every commit citation in AGENTS.md / CLAUDE.md must resolve to a commit.

Points added to AGENTS.md cite the commit that motivated them, kernel `Fixes:`
style: `4b03988 ("fix(package): rewrite the bundled libraries' RUNPATH too")`.
A point with no commit behind it is unverifiable folklore - the citation is
what lets the next agent judge whether the reasoning still applies.

A citation resolves if EITHER
  - the SHA names a commit in this repository, OR
  - some commit's exact subject line matches the quoted subject.
The fallback is not a courtesy: rebase merges rewrite SHAs, so a doc entry
landing in the same PR as the commit it cites will have that SHA dangle after
merge. The subject is the half a rebase preserves.

Ported from immaterial-impulse's tests/lint_doc_citations.py; adapted to this
repo's single-doc layout.
"""
import re
import subprocess
import unittest
from pathlib import Path

REPO = next(p for p in Path(__file__).resolve().parents if (p / "AGENTS.md").exists())
DOCS = [REPO / "AGENTS.md", REPO / "CLAUDE.md"]

# `abc1234def ("subject line")` - hex run + quoted subject. The adjacency makes
# accidental matches (hashes in URLs, hex constants) effectively impossible.
CITATION = re.compile(r'\b([0-9a-f]{7,40})\s+\("([^"\n]+)"\)')


def git(*args):
    return subprocess.run(["git", "-C", str(REPO), *args],
                          capture_output=True, text=True)


def repo_subjects():
    out = git("log", "--all", "--format=%s")
    return set(out.stdout.splitlines()) if out.returncode == 0 else set()


def sha_resolves(sha):
    return git("cat-file", "-e", f"{sha}^{{commit}}").returncode == 0


class DocCitationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if git("rev-parse", "--git-dir").returncode != 0:
            raise unittest.SkipTest("not a git checkout (release tarball?)")
        cls.subjects = repo_subjects()

    def test_every_citation_resolves(self):
        unresolved = []
        for doc in DOCS:
            if not doc.exists():
                continue
            for sha, subject in CITATION.findall(doc.read_text()):
                if not sha_resolves(sha) and subject not in self.subjects:
                    unresolved.append(f"{doc.name}: {sha} (\"{subject}\")")
        self.assertEqual(unresolved, [],
                         "citations that resolve to no commit, by SHA or subject:\n  "
                         + "\n  ".join(unresolved))

    def test_the_docs_actually_carry_citations(self):
        # Guards the mechanism itself: strip every citation in a rewrite and
        # the test above passes vacuously, which is how a rule dies silently.
        self.assertTrue(CITATION.findall((REPO / "AGENTS.md").read_text()),
                        "AGENTS.md has no commit citations at all")

    def test_the_lint_can_fail(self):
        # Permanent can-fail proof: a fabricated citation must not resolve.
        sha, subject = "deadbeef123", "no commit has ever had this subject xyzzy"
        self.assertFalse(sha_resolves(sha) or subject in self.subjects,
                         "the resolver accepted a fabricated citation")


if __name__ == "__main__":
    unittest.main()
