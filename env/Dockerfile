# Linux x86-64 build environment for QNX SDP 8.0 on hosts that the SDP does not
# support natively (macOS, Apple Silicon). The SDP itself is NOT in the image:
# it is mounted from the host (~/qnx800), because it is licensed per user.
FROM --platform=linux/amd64 ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential make bash python3 file xxd openssh-client ca-certificates \
        e2fsprogs dosfstools mtools \
    && rm -rf /var/lib/apt/lists/*
ARG USER_NAME=dev
ARG USER_ID=1000
ARG GROUP_ID=1000
RUN (getent group ${GROUP_ID} || groupadd --gid ${GROUP_ID} dev) && \
    useradd --uid ${USER_ID} --gid ${GROUP_ID} --no-log-init --create-home ${USER_NAME}
USER ${USER_NAME}
WORKDIR /work
CMD ["/bin/bash"]
