#===============================================================================
#  zstreamer — Docker development environment
#
#  Build:    docker build -t zstreamer .
#  Run:      docker run --rm zstreamer              # runs ctest (ci target)
#  Run dev:  docker run --rm -it zstreamer bash     # interactive shell
#
#  For device access (V4L2 cameras) add:
#    --device /dev/video0
#
#  Build for a specific stage:
#    docker build --target deps -t zstreamer-deps .   # deps + source, no build
#    docker build --target dev -t zstreamer-dev .     # interactive dev shell
#===============================================================================

FROM ubuntu:24.04 AS deps

ENV DEBIAN_FRONTEND=noninteractive

# ── Build dependencies ──────────────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    ca-certificates \
    curl \
    zip \
    && rm -rf /var/lib/apt/lists/*

# ── Multimedia libraries (for element plugins) ──────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    libv4l-dev \
    libx264-dev \
    libx265-dev \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libswscale-dev \
    libswresample-dev \
    libasound2-dev \
    libfreetype-dev \
    libsrt-gnutls-dev \
    libgnutls28-dev \
    nettle-dev \
    libvulkan-dev \
    libx11-dev \
    libxext-dev \
    xvfb \
    fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

# ── Debugging / profiling tools ─────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    gdb \
    valgrind \
    strace \
    && rm -rf /var/lib/apt/lists/*

# ── Copy source code (no build yet) ─────────────────────────────────────────
WORKDIR /workspace
COPY . .

# ── Build (inherits all deps + source from the deps stage) ──────────────────
FROM deps AS base

RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON && \
    make -j$(nproc)

# ── Interactive development (inherits all of base) ──────────────────────────
FROM base AS dev
WORKDIR /workspace
CMD ["/bin/bash"]

# ── CI / one-shot test (default target — last in Dockerfile) ────────────────
FROM base AS ci
WORKDIR /workspace/build
CMD ["ctest", "--output-on-failure"]
