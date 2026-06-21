FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config libsqlite3-dev libudev-dev \
    nlohmann-json3-dev libcli11-dev python3 dpkg-dev file binutils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j2 \
    && ctest --test-dir build --output-on-failure \
    && cpack --config build/CPackConfig.cmake -B build/packages

FROM ubuntu:24.04 AS verify
COPY --from=build /src/build/packages/*.deb /tmp/
RUN apt-get update && apt-get install -y --no-install-recommends /tmp/*.deb python3 \
    && rm -rf /var/lib/apt/lists/* /tmp/*.deb
COPY --from=build /src/tests /verify/tests
RUN serialspark --version \
    && serialspark protocol validate /usr/share/doc/serialspark/examples/lines-duplex.json \
    && python3 /verify/tests/cli_live_test.py /usr/bin/serialspark /usr/share/doc/serialspark/examples \
    && python3 /verify/tests/cli_offline_test.py /usr/bin/serialspark /usr/share/doc/serialspark/examples/lines.json
CMD ["serialspark", "--help"]
