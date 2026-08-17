# Debian bookworm + EarthScope libs. Pin upstream tags via build args.

ARG BASE=debian:bookworm-slim
ARG SLINK2DALI_VERSION=v0.8
ARG LIBDALI_VERSION=develop

FROM ${BASE} AS buildenv
ARG SLINK2DALI_VERSION
ARG LIBDALI_VERSION
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      clang \
      git \
      make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN git clone --depth 1 --branch "${SLINK2DALI_VERSION}" \
      https://github.com/EarthScope/slink2dali.git upstream \
 && rm -rf upstream/libdali \
 && git clone --depth 1 --branch "${LIBDALI_VERSION}" \
      https://github.com/EarthScope/libdali.git upstream/libdali

COPY src/ringserver-feeder.c upstream/src/
COPY src/Makefile upstream/src/

RUN cd upstream \
 && make CC=clang \
 && make -C src CC=clang

FROM ${BASE}
ARG DEBIAN_FRONTEND=noninteractive
ARG UID=10000
ARG GID=10000
ARG USERNAME=containeruser

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      netbase \
      procps \
 && rm -rf /var/cache/apt/archives /var/lib/apt/lists/*

COPY --from=buildenv /build/upstream/ringserver-feeder /ringserver-feeder

RUN groupadd --gid "${GID}" "${USERNAME}" \
 && adduser --uid "${UID}" --gid "${GID}" "${USERNAME}" \
 && mkdir -p /data \
 && chown -R "${UID}:${GID}" /data

WORKDIR /data
USER ${USERNAME}

ENV FEEDER_STATE_FILE=/data/seedlink.state
ENV FEEDER_PKTID_FILE=/data/pktid.state
ENV FEEDER_LOCK_FILE=/data/feeder.lock

ENTRYPOINT ["/ringserver-feeder"]
