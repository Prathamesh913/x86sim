# x86Sim release build environment (the same packages as CI's ubuntu-24.04 runner, build/release/deps-ubuntu.sh).
# Local check of the CI build: build/release/docker-build.sh
FROM ubuntu:24.04
COPY deps-ubuntu.sh /tmp/deps-ubuntu.sh
RUN sh /tmp/deps-ubuntu.sh && rm -rf /var/lib/apt/lists/*
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable && chmod -R a+rwX /opt/cargo /opt/rustup
