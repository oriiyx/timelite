# Reproducible Linux x86-64 / x86-32 / MinGW-w64 toolchain for tools/test.py.
# The base image is pinned by digest (multi-arch manifest; the runner always
# requests linux/amd64). Package versions come from the Debian bookworm
# repositories at preparation time and are recorded in
# /etc/timelite-toolchain.txt, which `python3 tools/test.py prepare` prints and
# every container run stores in its report. Rebuild with `prepare` after any
# change to this file; the image tag is derived from this file's hash.
FROM debian:bookworm-slim@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        gcc libc6-dev gcc-multilib libc6-dev-i386 binutils \
        gcc-mingw-w64-x86-64 python3 \
    && rm -rf /var/lib/apt/lists/*

RUN { echo "base=debian:bookworm-slim@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171"; \
      echo "machine=$(uname -m)"; \
      echo "gcc=$(gcc --version | head -n 1) target=$(gcc -dumpmachine)"; \
      echo "mingw=$(x86_64-w64-mingw32-gcc --version | head -n 1) target=$(x86_64-w64-mingw32-gcc -dumpmachine)"; \
      echo "python=$(python3 --version)"; \
      dpkg-query -W -f='package=${Package} ${Version}\n' gcc gcc-multilib libc6-dev libc6-dev-i386 \
          binutils gcc-mingw-w64-x86-64 python3; \
    } > /etc/timelite-toolchain.txt

WORKDIR /source
