# Use Debian Bookworm as the base image for TurboNet and its derivatives
FROM debian:bookworm

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Update and install necessary system dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    ninja-build \
    pkg-config \
    git \
    curl \
    zip \
    unzip \
    tar \
    python3 \
    liburing-dev \
    libssl-dev \
    linux-libc-dev \
    autoconf \
    libtool \
    re2c \
    openssl \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Install a recent CMake (3.31.0)
RUN curl -L https://github.com/Kitware/CMake/releases/download/v3.31.0/cmake-3.31.0-linux-x86_64.sh \
    -o /tmp/cmake-install.sh \
    && chmod +x /tmp/cmake-install.sh \
    && /tmp/cmake-install.sh --skip-license --prefix=/usr/local \
    && rm /tmp/cmake-install.sh

# Clone and bootstrap vcpkg
# Depth 1 is recommended for speed and to avoid network issues with large histories
ENV VCPKG_ROOT=/vcpkg
RUN git config --global http.sslVerify false && \
    git clone  https://github.com/microsoft/vcpkg /vcpkg && \
    /vcpkg/bootstrap-vcpkg.sh

# Set the working directory
WORKDIR /app

# Install project dependencies using vcpkg
# CRITICAL: We update the builtin-baseline to match the cloned vcpkg HEAD.
# This ensures that vcpkg can find the versions specified in vcpkg.json
# Without this, vcpkg often fails to find a valid registry or versions.
COPY vcpkg.json ./
RUN /vcpkg/vcpkg install --triplet x64-linux

# Copy source code and build/install TurboNet
# Using the linux-container preset from CMakeUserPresets.json
# We explicitly set VCPKG_FORCE_SYSTEM_BINARIES=1 to ensure tools like pkg-config
# can find system libraries like liburing.
COPY . .
RUN find . -exec touch {} +

RUN cmake --preset linux-container \
    -D VCPKG_FORCE_SYSTEM_BINARIES=1 \
    -D BUILD_EXAMPLES=OFF \
    -D BUILD_TESTS=OFF && \
    cmake --build build/linux-gcc-release && \
    cmake --install build/linux-gcc-release --prefix /usr/local

# Set the default directory to root for derived images
WORKDIR /root
