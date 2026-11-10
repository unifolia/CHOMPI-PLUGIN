#!/bin/bash
# Fail unless the release tag ($1, e.g. v1.0.1) is "v" plus the version in
# project() in CMakeLists.txt, the one place the release version is set.
set -euo pipefail
cd "$(dirname "$0")/.."
tag=${1:?Usage: check_release_tag.sh vX.Y.Z}
version=$(sed -n 's/^project(CHIMPO VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
test -n "$version"
if [ "$tag" != "v$version" ]; then
    echo "::error::Tag $tag does not match project(CHIMPO VERSION $version) in CMakeLists.txt (expected v$version)" >&2
    exit 1
fi
echo "Tag $tag matches version $version"
