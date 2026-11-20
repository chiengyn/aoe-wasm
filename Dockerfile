# syntax=docker/dockerfile:1
#
# Packages the browser game: the release engine build, the page, and the Python server that also relays
# multiplayer packets. Game data is NOT part of the image. Mount your own installation at /game, and keep the
# prepared data in a volume at /app/web/game (see the README, "Docker").
#
#   docker build -t aoe-wasm .
#   docker run -p 8788:8788 -v aoe-data:/app/web/game -v "/path/to/Age of Empires:/game:ro" aoe-wasm

# --- 1. Engine: compile the recovered C++ to WebAssembly (release build) --------------------------------
FROM emscripten/emsdk:7.0.0 AS engine
WORKDIR /src
COPY engine/ engine/
RUN python3 engine/build.py --sdk /emsdk --jobs "$(nproc)" --game --release \
 && mkdir -p /out \
 && cp engine/build-release/game.mjs engine/build-release/game.wasm /out/ \
 && cp engine/build-release/link-game.json /out/game-build.json

# --- 2. Music: SpessaSynth + GeneralUser GS soundfont (hash-checked). --build-arg MUSIC=0 skips it ------
FROM python:3.14-slim AS vendor
ARG MUSIC=1
WORKDIR /src
COPY tools/fetch_vendor.py tools/
RUN mkdir -p web/vendor && if [ "$MUSIC" = "1" ]; then python3 tools/fetch_vendor.py; fi

# --- 3. Runtime --------------------------------------------------------------------------------------------
FROM python:3.14-slim
# ffmpeg converts the game's intro videos when the data is prepared (tools/prepare_native.py).
RUN apt-get update \
 && apt-get install -y --no-install-recommends ffmpeg \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --create-home --uid 10001 aoe
ENV PYTHONUNBUFFERED=1 PORT=8788
WORKDIR /app
COPY tools/ tools/
COPY web/ web/
COPY --from=engine /out/ web/engine/
COPY --from=vendor /src/web/vendor/ web/vendor/
COPY docker/entrypoint.sh /usr/local/bin/entrypoint
RUN chmod 755 /usr/local/bin/entrypoint \
 && mkdir -p web/game \
 && chown aoe:aoe web/game
USER aoe
VOLUME /app/web/game
EXPOSE 8788
# The server shuts down cleanly on Ctrl+C (KeyboardInterrupt); SIGTERM would be ignored by PID 1.
STOPSIGNAL SIGINT
HEALTHCHECK --interval=30s --timeout=5s --start-period=10m \
  CMD python3 -c "import os,urllib.request;urllib.request.urlopen('http://127.0.0.1:%s/native.html' % os.environ['PORT'],timeout=4)"
ENTRYPOINT ["entrypoint"]
CMD ["serve"]
