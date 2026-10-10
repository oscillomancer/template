#!/usr/bin/env bash
# Runs clap-probe against the built CLAP in each of its host scenarios and
# fails if the editor did not come up or the audio path went silent. Each
# scenario's window capture lands in probe/<scenario>.png.
#   clap-probe-mac.sh <Product name>
set -uo pipefail
name="$1"
binary="build/release/plugins/$name.clap/Contents/MacOS/$name"
probe="build/release/clap-probe"
mkdir -p probe

# perl's alarm stands in for coreutils timeout, which macOS lacks.
run() { perl -e 'alarm shift; exec @ARGV' "$@"; }

failed=0
for scenario in "plain:" "resize:--resize" "reopen:--reopen" "no-host-timer:--no-host-timer" \
                "destroy-hot:--destroy-hot" "drop-parent:--drop-parent" "mono:--mono"; do
    label="${scenario%%:*}"
    flags="${scenario#*:}"
    echo "::group::clap-probe $label"
    rm -f probe-window.png
    # shellcheck disable=SC2086
    run 120 "$probe" "$binary" 400 $flags > "probe/$label.txt" 2>&1
    status=$?
    cat "probe/$label.txt"
    [ -f probe-window.png ] && mv probe-window.png "probe/$label.png"
    echo "::endgroup::"

    problems=()
    [ $status -eq 0 ] || problems+=("exit status $status")
    grep -q "gui: create=1 parent=1 show=1" "probe/$label.txt" || problems+=("editor did not open")
    grep -q "audio path is live" "probe/$label.txt" || problems+=("audio path silent")
    if [ "$label" = reopen ]; then
        grep -q "reopen: create=1 parent=1 show=1" "probe/$label.txt" || problems+=("editor did not reopen")
    fi
    if [ "$label" = resize ]; then
        grep -q "set_size=1" "probe/$label.txt" || problems+=("set_size refused")
    fi
    if [ ${#problems[@]} -gt 0 ]; then
        echo "::error title=clap-probe $label::$(IFS='; '; echo "${problems[*]}")"
        failed=1
    fi
done
exit $failed
