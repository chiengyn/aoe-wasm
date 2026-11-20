#!/bin/sh
# Container entry point.
#   serve    (default) Prepare the game data from /game on first start if needed, then run the server.
#   prepare  Prepare (or re-prepare) the game data from /game into /app/web/game, then exit.
#   anything else is run as a command.
set -eu
cd /app

prepare() {
    if [ ! -d /game ] || [ -z "$(ls -A /game 2>/dev/null)" ]; then
        echo "No game data: mount your Age of Empires folder (the one with EMPIRESX.EXE) at /game," >&2
        echo "for example: -v \"/path/to/Age of Empires:/game:ro\"" >&2
        exit 1
    fi
    echo "Preparing game data from /game (this takes a minute: intro videos are converted)..."
    python3 tools/prepare_native.py /game
}

case "${1:-serve}" in
    prepare)
        prepare
        ;;
    serve)
        if [ ! -f web/game/native.json ]; then
            prepare
        fi
        exec python3 tools/serve.py --host 0.0.0.0 --port "${PORT:-8788}"
        ;;
    *)
        exec "$@"
        ;;
esac
