# Dev container image: toolchain + debugger + the same runtime libs the app needs.
# Unlike the production ../Dockerfile, this does NOT copy/build the source — the
# workspace is bind-mounted by VS Code and you build interactively via the
# .vscode tasks (cmake / build / test).
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    gdb \
    git \
    # ── app dependencies (match the production Dockerfile) ──
    libmosquitto-dev \
    libmosquitto1 \
    # libjsoncpp-dev \
    nlohmann-json3-dev \
    # ── handy for testing the pipeline from inside the container ──
    mosquitto \
    mosquitto-clients \
    iproute2 \
    # ── Python (required by tests/ and test_ais.py simulator) ──
    # python3 \
    # python3-pip \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir --break-system-packages paho-mqtt pytest

# Tip: start a local broker for testing with `mosquitto -v &`
# then watch topics with `mosquitto_sub -t 'uuv/#' -v`.
