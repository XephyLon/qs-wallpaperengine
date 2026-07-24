#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
y="$here/release.yml"
[[ -f "$y" ]] || { echo "FAIL: release.yml missing"; exit 1; }
python3 -c 'import sys,yaml; yaml.safe_load(open(sys.argv[1]))' "$y" || { echo "FAIL: invalid YAML"; exit 1; }
grep -q "tags:" "$y"              || { echo "FAIL: no tag trigger"; exit 1; }
grep -q "workflow_dispatch" "$y"  || { echo "FAIL: no manual dispatch"; exit 1; }
grep -q "build-we.sh" "$y"        || { echo "FAIL: does not call build-we.sh"; exit 1; }
grep -q "package-we.sh" "$y"      || { echo "FAIL: does not call package-we.sh"; exit 1; }
grep -q "gh release create" "$y"  || { echo "FAIL: does not publish a release"; exit 1; }
echo "PASS"
