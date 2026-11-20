#!/bin/sh
# Entry point of the prepare image: copies the game folder mounted at /game into /app/web/game (the data
# volume the server image reads), converting the intro videos and the help file.
#   (no argument)  prepare once; does nothing if the volume already holds prepared data
#   --force        prepare again (after changing the game folder)
set -eu
cd /app

if [ -f web/game/native.json ] && [ "${1:-}" != "--force" ]; then
    echo "Game data is already prepared; run with --force to prepare it again."
    exit 0
fi
if [ ! -d /game ] || [ -z "$(ls -A /game 2>/dev/null)" ]; then
    echo "No game data: mount your Age of Empires folder (the one with EMPIRESX.EXE) at /game," >&2
    echo "for example: -v \"/path/to/Age of Empires:/game:ro\"" >&2
    exit 1
fi
echo "Preparing game data from /game (about a minute: the intro videos are converted)..."
exec python3 tools/prepare_native.py /game
