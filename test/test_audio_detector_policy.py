#!/usr/bin/env python3
"""Keep WE's per-frame PulseAudio detector out of every embed configuration."""

from pathlib import Path
import re


SOURCE = (
    Path(__file__).resolve().parents[1] / "quickshell-module" / "wethread.cpp"
).read_text()


def main() -> None:
    argv_start = SOURCE.index("std::vector<char*> argv")
    scaling_start = SOURCE.index("// Pass the scaling mode", argv_start)
    audio_policy = SOURCE[argv_start:scaling_start]

    # One unconditional argument must precede the audio branch. Putting it only
    # in mAudioEnabled recreates #16 for the common --silent configuration.
    assert audio_policy.count('const_cast<char*>("--noautomute")') == 1
    noautomute = audio_policy.index('const_cast<char*>("--noautomute")')
    branch = audio_policy.index("if (this->mAudioEnabled)")
    assert noautomute < branch

    enabled, disabled = re.search(
        r"if \(this->mAudioEnabled\) \{(?P<enabled>.*?)\n\t\} else \{"
        r"(?P<disabled>.*?)\n\t\}",
        audio_policy,
        re.S,
    ).group("enabled", "disabled")
    assert '"--volume"' in enabled
    assert '"--silent"' not in enabled
    assert '"--silent"' in disabled
    assert '"--volume"' not in disabled

    print("embedded audio detector policy: PASS")


if __name__ == "__main__":
    main()
