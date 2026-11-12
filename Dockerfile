# puddled, the headless Puddle, as a container image.
#
#   docker build -t puddle .
#   docker run -d --name puddle -p 8443:8443 -v puddle:/data -e PUDDLE_ADVERTISE=home.example.org:8443 puddle
#   docker exec -it puddle puddled --init     # once: makes the identity, shows the recovery phrase; then it starts
#
# Everything the Puddle keeps (puddle.yaml, profiles.yaml, its certificate, keys and chats) is in the /data volume.
# See docs/docker.md.

FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake libssl-dev libyaml-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src src
COPY tests tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPUDDLE_BUILD_GUI=OFF \
 && cmake --build build --parallel 4 --target puddled \
 && strip build/puddled

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3 libyaml-0-2 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home-dir /data --shell /usr/sbin/nologin puddle \
 && mkdir /data && chown puddle:puddle /data && chmod 700 /data
COPY --from=build /src/build/puddled /usr/local/bin/puddled
COPY dist/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh

USER puddle
ENV HOME=/data PUDDLE_CONFIG=/data/puddle.yaml
VOLUME /data
EXPOSE 8443
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
