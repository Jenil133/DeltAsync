# Build from the project folder: docker build -t deltasync-dev docker/
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      g++-14 \
      clang-18 \
      lld-18 \
      libclang-rt-18-dev \
      cmake \
      ninja-build \
      git \
      ca-certificates \
      curl \
      python3 \
      iproute2 \
      iputils-ping \
      procps \
      time \
      util-linux \
 && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100 \
 && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
