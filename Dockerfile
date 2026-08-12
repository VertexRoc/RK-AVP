# syntax=docker/dockerfile:1.7

FROM ubuntu:22.04 AS host-deps
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake git libgtest-dev libyaml-cpp-dev \
    pybind11-dev python3-dev python3-pytest ninja-build \
    && rm -rf /var/lib/apt/lists/*

FROM host-deps AS ci
WORKDIR /src
COPY . .
RUN cmake -S . -B build/ci -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DRKAVP_BUILD_TESTS=ON \
      -DRKAVP_FETCH_DEPS=OFF \
      -DRKAVP_ENABLE_PYTHON=ON \
    && cmake --build build/ci \
    && ctest --test-dir build/ci --output-on-failure

FROM ubuntu:22.04 AS sdk-arm64
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake git gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    make ninja-build pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/rkavp-src
COPY . .
ENV RKAVP_TOOLCHAIN=/opt/rkavp-src/cmake/toolchains/aarch64-linux-gnu.cmake
CMD ["cmake", "--version"]

FROM ubuntu:22.04 AS hardware-builder
ARG DEBIAN_FRONTEND=noninteractive
ARG RKAVP_SOC=rk3588
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake git libasound2-dev libssl-dev \
    ninja-build pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build/runtime -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/rkavp-runtime \
      -DRKAVP_TARGET_SOC=${RKAVP_SOC} \
      -DRKAVP_BUILD_TESTS=OFF \
      -DRKAVP_FETCH_DEPS=ON \
      -DRKAVP_ENABLE_ROCKCHIP=ON \
      -DRKAVP_ENABLE_AUDIO=ON \
      -DRKAVP_ENABLE_STREAMING=ON \
    && cmake --build build/runtime \
    && cmake --install build/runtime --prefix /opt/rkavp-runtime --component Runtime \
    && cmake --install build/runtime --prefix /opt/rkavp-sdk --component Runtime \
    && cmake --install build/runtime --prefix /opt/rkavp-sdk --component Development \
    && cmake --install build/runtime --prefix /opt/rkavp-sdk --component Testing \
    && cp 3rdparty/src/rk_rga/libs/Linux/gcc-aarch64/librga.so /opt/rkavp-runtime/lib/ \
    && cp 3rdparty/src/rknn-runtime/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so /opt/rkavp-runtime/lib/

FROM ubuntu:22.04 AS runtime-base
ARG RKAVP_VERSION=0.4.0
ARG VCS_REF=unknown
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libasound2 libssl3 libstdc++6 libyaml-cpp0.7 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=hardware-builder /opt/rkavp-runtime/bin /opt/rkavp/bin
COPY --from=hardware-builder /opt/rkavp-runtime/lib /opt/rkavp/lib
COPY --from=hardware-builder /opt/rkavp-runtime/share/rkavp /opt/rkavp/share/rkavp
COPY --from=hardware-builder /opt/rkavp-runtime/share/licenses /opt/rkavp/share/licenses
ENV PATH=/opt/rkavp/bin:${PATH}
ENV LD_LIBRARY_PATH=/opt/rkavp/lib
ENV RKAVP_PLUGIN_PATH=/opt/rkavp/lib
WORKDIR /work
LABEL org.opencontainers.image.title="RK-AVP" \
      org.opencontainers.image.version="${RKAVP_VERSION}" \
      org.opencontainers.image.revision="${VCS_REF}" \
      org.opencontainers.image.licenses="Apache-2.0"
ENTRYPOINT ["rkavp-run"]
CMD ["doctor"]

FROM runtime-base AS devel-base
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ninja-build pkg-config \
    && rm -rf /var/lib/apt/lists/*
COPY --from=hardware-builder /opt/rkavp-sdk/include /opt/rkavp/include
COPY --from=hardware-builder /opt/rkavp-sdk/lib/cmake /opt/rkavp/lib/cmake
COPY --from=hardware-builder /opt/rkavp-sdk/lib/librkavp_testing.a /opt/rkavp/lib/librkavp_testing.a
ENTRYPOINT []
CMD ["cmake", "--version"]

FROM devel-base AS devel-rk3588
LABEL org.opencontainers.image.description="RK-AVP development environment for RK3588"

FROM devel-base AS devel-rk3576
LABEL org.opencontainers.image.description="RK-AVP development environment for RK3576"

FROM runtime-base AS runtime-rk3588
LABEL org.opencontainers.image.description="RK-AVP framework runtime for RK3588"

FROM runtime-base AS runtime-rk3576
LABEL org.opencontainers.image.description="RK-AVP framework runtime for RK3576"
