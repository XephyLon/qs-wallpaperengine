#!/usr/bin/env python3
"""Keep the libmpv vsync-fence cap (#16) applied and shaped correctly.

mpv's vo=libmpv leaks one glFenceSync per rendered frame (created in
ra_gl_ctx_submit_frame, only ever deleted in ra_gl_ctx_swap_buffers, which the
render API never calls). The bootstrap patch interposes glFenceSync/
glDeleteSync in GLPlayer's get_proc_address and caps the live set. This test
asserts the patch block's invariants in bootstrap.sh, and - when the build
tree exists and is patched - the applied result.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
BOOTSTRAP = (ROOT / "bootstrap.sh").read_text()
GLPLAYER = ROOT / "build/linux-wallpaperengine/src/WallpaperEngine/VideoPlayback/MPV/GLPlayer.cpp"

MARKER = "EMBED PATCH: cap libmpv vsync fences"


def code_only(text):
    """Strip // comments so assertions cannot match their own documentation."""
    return "\n".join(l for l in text.splitlines()
                     if not l.lstrip().startswith("//"))


def block():
    start = BOOTSTRAP.index("# GLPlayer: cap libmpv's vsync fences")
    end = BOOTSTRAP.index("PY", BOOTSTRAP.index("<<'PY'", start) + 6)
    return BOOTSTRAP[start:end]


def main():
    b = block()

    # Marker-guarded idempotence and fail-loud anchors: a silently no-op'd
    # anchor is how a past patch shipped a broken build (see the
    # getDestinationFramebuffer note in bootstrap.sh).
    assert MARKER in b
    assert b.count("sys.exit(\"EMBED PATCH:") == 3

    # The interposition itself: both wrappers, handed back from
    # get_proc_address, with a bounded ring and a drain for teardown.
    for needle in ("cappedFenceSync", "cappedDeleteSync", "drainFenceRing",
                   "glFenceSync", "glDeleteSync"):
        assert needle in b, needle

    cap = int(re.search(r"FENCE_RING_CAP = (\d+)", b).group(1))
    # Must exceed any plausible in-flight fence count (swapchain depth + PBO
    # pool) so a live fence is never deleted, but stay far below the ~2000
    # fences/min mpv creates so the cap actually bounds the leak.
    assert 16 <= cap <= 256, cap

    if GLPLAYER.is_file():
        src = GLPLAYER.read_text()
        if MARKER in src:
            assert src.count(MARKER) == 1
            code = code_only(src)
            # get_proc_address must return the wrappers, not the real symbols.
            gpa = code[code.index("void* get_proc_address"):]
            gpa = gpa[:gpa.index("\n}") + 2]
            assert "&cappedFenceSync" in gpa
            assert "&cappedDeleteSync" in gpa
            # stop() drains the ring while a share-group context is current -
            # before mpv teardown, so stale handles never outlive their group.
            stop = code[code.index("void GLPlayer::stop ()"):]
            stop = stop[:stop.index("\n}") + 2]
            assert stop.index("drainFenceRing ()") < stop.index("mpv_render_context_free")
            print("fence cap patch (applied tree): PASS")
        else:
            print("fence cap patch: build tree present but unpatched "
                  "(run bootstrap.sh); bootstrap block: PASS")
    else:
        print("fence cap patch (bootstrap block only): PASS")


if __name__ == "__main__":
    main()
