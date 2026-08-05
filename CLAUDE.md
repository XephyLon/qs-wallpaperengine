# CLAUDE.md

**Before doing any work in this repository, read `AGENTS.md` sequentially, in full, top to
bottom.** Grep hits and section jumps do not count as having read it: the rules that get broken are
the ones adjacent to the section someone jumped to. Re-read it after a context compaction.

Two mechanical rules guard that file (details in `AGENTS.md` → "Doc discipline"):

- **Every point added to `AGENTS.md` must cite the commit that motivated it** as
  `<sha> ("<subject>")`. `test/lint_doc_citations.py` (run by CI) fails on any citation that
  resolves to nothing.
- **Every PR body must carry a `Docs:` receipt line** — `Docs: updated AGENTS.md §<section>` or
  `Docs: not needed — <reason>`. CI rejects PRs without one.
