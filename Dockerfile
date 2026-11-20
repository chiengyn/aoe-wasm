# syntax=docker/dockerfile:1
#
# Two images, neither containing game data:
#   server  (default target)  the Go server (static files + DirectPlay relay), the page, the release engine
#   prepare (--target prepare) Python + ffmpeg: prepares your own game folder into the shared data volume
#
#   docker build -t aoe-wasm .
#   docker build --target prepare -t aoe-wasm-prepare .
#   docker run --rm -v aoe-data:/app/web/game -v "/path/to/Age of Empires:/game:ro" aoe-wasm-prepare
#   docker run -d -p 8788:8788 -v aoe-data:/app/web/game:ro aoe-wasm
#
# Both images run as UID 65532, so the data volume works the same whichever container mounts it first.

# --- Engine: compile the recovered C++ to WebAssembly (release build) ----------------------------------
FROM emscripten/emsdk:7.0.0 AS engine
WORKDIR /src
COPY engine/ engine/
RUN python3 engine/build.py --sdk /emsdk --jobs "$(nproc)" --game --release \
 && mkdir -p /out \
 && cp engine/build-release/game.mjs engine/build-release/game.wasm /out/ \
 && cp engine/build-release/link-game.json /out/game-build.json

# --- Music: SpessaSynth + GeneralUser GS soundfont (hash-checked). --build-arg MUSIC=0 skips it ----------
FROM python:3.14-slim AS vendor
ARG MUSIC=1
WORKDIR /src
COPY tools/fetch_vendor.py tools/
RUN mkdir -p web/vendor /empty && if [ "$MUSIC" = "1" ]; then python3 tools/fetch_vendor.py; fi

# --- Server binary ------------------------------------------------------------------------------------------
FROM golang:1.27.3 AS server-build
WORKDIR /src
COPY server/go.mod server/go.sum ./
RUN go mod download
COPY server/ ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/aoe-server .

# --- prepare: game data from a folder mounted at /game into the /app/web/game volume ------------------------
FROM python:3.14-slim AS prepare
RUN apt-get update \
 && apt-get install -y --no-install-recommends ffmpeg \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 65532 --home-dir /app aoe
WORKDIR /app
COPY tools/ tools/
COPY docker/prepare.sh /usr/local/bin/prepare
RUN chmod 755 /usr/local/bin/prepare && mkdir -p web/game && chown 65532:65532 web/game
USER 65532
VOLUME /app/web/game
ENTRYPOINT ["prepare"]

# --- server (default) ---------------------------------------------------------------------------------------
FROM gcr.io/distroless/static-debian13:nonroot AS server
WORKDIR /app
COPY web/ web/
COPY --from=engine /out/ web/engine/
COPY --from=vendor /src/web/vendor/ web/vendor/
COPY --from=vendor --chown=65532:65532 /empty/ web/game/
COPY --from=server-build /out/aoe-server /app/aoe-server
USER 65532
ENV PORT=8788
EXPOSE 8788
VOLUME /app/web/game
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s CMD ["/app/aoe-server", "healthcheck"]
ENTRYPOINT ["/app/aoe-server", "-host", "0.0.0.0", "-web", "/app/web"]
