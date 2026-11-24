#!/bin/sh
# Build the GitHub Pages site in docs/: the codelab rendered as on qnx.github.io/codelabs,
# and the example HTML report. Needs claat from https://github.com/qnx/tools (the QNX fork
# of googlecodelabs/tools: cd claat && go install).
#
#   tools/build_site.sh
#
# The QNX claat template loads its scripts from ../js/. Those are Google's codelab-elements
# files, also published at storage.googleapis.com/claat-public, so the page loads them from
# there instead of keeping copies in this repository.
#
# SPDX-License-Identifier: Apache-2.0
set -eu
cd "$(dirname "$0")/.."
CLAAT=${CLAAT:-claat}
ASSETS=https://storage.googleapis.com/claat-public

rm -rf docs/qnx-message-tracing
"$CLAAT" export -f html -ga "" -o docs codelab/qnx-message-tracing.md
sed -i.bak "s#\"\.\./js/#\"$ASSETS/#g" docs/qnx-message-tracing/index.html
rm docs/qnx-message-tracing/index.html.bak
if grep -q '"\.\./js/' docs/qnx-message-tracing/index.html; then
    echo "build_site: ../js/ references left in the codelab page" >&2
    exit 1
fi

mkdir -p docs/report
cp examples/sh-pipeline.html docs/report/sh-pipeline.html
echo "site in docs/: codelab docs/qnx-message-tracing/, report docs/report/sh-pipeline.html"
