#!/usr/bin/env python3
"""Keep the external-pause patch (#19) applied and wired on both sides.

`occluded` idles the module's render thread, but only WE's own pause machinery
stops mpv decoding - measured at a constant ~180% CPU for a 7680x2160
software-decoded video under a fullscreen game before this patch. The
bootstrap patch gives WallpaperApplication a public setExternalPaused() ORed
into BOTH fullscreen-detector checks in render(); the module forwards the
occlusion flag into it every loop iteration. Each half is inert without the
other, so this test pins both: the bootstrap block's invariants, the applied
tree (when present), and the module call site's placement.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
BOOTSTRAP = (ROOT / "bootstrap.sh").read_text()
WETHREAD = (ROOT / "quickshell-module/wethread.cpp").read_text()
APP_CPP = ROOT / "build/linux-wallpaperengine/src/WallpaperEngine/Application/WallpaperApplication.cpp"
APP_H = ROOT / "build/linux-wallpaperengine/src/WallpaperEngine/Application/WallpaperApplication.h"

ORED = ("(this->m_fullScreenDetector->anythingFullscreen () || "
        "this->m_externalPaused) && this->m_context.state.general.keepRunning")


def code_only(text):
    """Strip // comments so assertions cannot match their own documentation."""
    return "\n".join(l for l in text.splitlines()
                     if not l.lstrip().startswith("//"))


def block():
    start = BOOTSTRAP.index("# WallpaperApplication: external pause control")
    end = BOOTSTRAP.index("PY", BOOTSTRAP.index("<<'PY'", start) + 6)
    return BOOTSTRAP[start:end]


def main():
    b = block()

    # Fail-loud anchors: the exactly-2 count check for the detector condition,
    # and the header-anchor check. A silently no-op'd anchor is how a past
    # patch shipped a broken build (getDestinationFramebuffer, bootstrap.sh).
    assert b.count("sys.exit(\"EMBED PATCH:") == 2
    assert "cpp.count(cond) != 2" in b

    # Both halves of the patch: the setter+member in the header, and the OR
    # into the detector condition in render().
    for needle in ("setExternalPaused", "m_externalPaused",
                   "anythingFullscreen () || this->m_externalPaused"):
        assert needle in b, needle

    # Module side: the loop must forward the occlusion flag into WE before
    # render() runs, else render() acts on the previous iteration's verdict.
    code = code_only(WETHREAD)
    loop = code[code.index("while (!this->mStop)"):]
    set_at = loop.index("app->setExternalPaused(occludedNow)")
    render_at = loop.index("app->render()")
    assert set_at < render_at, "setExternalPaused must precede app->render()"

    if APP_CPP.is_file():
        cpp = APP_CPP.read_text()
        if "m_externalPaused" in cpp:
            # Both the enter-pause and the stay-paused checks, or a covered
            # output pauses but never stays paused (or vice versa).
            assert code_only(cpp).count(ORED) == 2
            h = code_only(APP_H.read_text())
            assert "void setExternalPaused (bool paused)" in h
            assert "bool m_externalPaused = false;" in h
            print("external pause patch (applied tree): PASS")
        else:
            print("external pause patch: build tree present but unpatched "
                  "(run bootstrap.sh); bootstrap block + module: PASS")
    else:
        print("external pause patch (bootstrap block + module): PASS")


if __name__ == "__main__":
    main()
