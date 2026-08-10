########################  Stage 1: builder  ########################
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libmosquitto-dev \
        libssl-dev \
        nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY shared/         ./shared/
COPY compute-server/ ./compute-server/

RUN test ! -e control-server \
    || { echo "FATAL: control-server 가 빌드 컨텍스트에 유입됨 - 격리 위반" >&2; exit 1; }

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DVEDA_BUILD_CONTROL_SERVER=OFF \
    && cmake --build build --target compute-server -j"$(nproc)"

RUN test ! -e build/control-server \
    || { echo "FATAL: control-server 빌드 산출물이 생성됨 - 격리 위반" >&2; exit 1; }

########################  Stage 2: runtime  ########################
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 \
        libmosquitto1 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m veda_user

WORKDIR /opt/veda
RUN chown veda_user:veda_user /opt/veda

COPY --from=builder /src/build/compute-server/compute-server /usr/local/bin/compute-server

USER veda_user

STOPSIGNAL SIGTERM

ENTRYPOINT ["/usr/local/bin/compute-server"]