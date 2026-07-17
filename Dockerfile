FROM ubuntu:24.04

# Install everything needed to build MainDOB
RUN apt-get update -qq && \
    apt-get install -y -qq \
        nasm grub-pc-bin grub-common xorriso \
        qemu-system-x86 gdb build-essential \
        bison flex libgmp-dev libmpc-dev libmpfr-dev \
        texinfo curl make && \
    rm -rf /var/lib/apt/lists/*

# Build i686-elf cross-compiler
ENV PREFIX=/opt/cross
ENV TARGET=i686-elf
ENV PATH="${PREFIX}/bin:${PATH}"

RUN cd /tmp && \
    curl -LO https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz && \
    tar xf binutils-2.42.tar.xz && \
    mkdir build-binutils && cd build-binutils && \
    ../binutils-2.42/configure --target=$TARGET --prefix=$PREFIX \
        --with-sysroot --disable-nls --disable-werror && \
    make -j$(nproc) && make install && \
    cd /tmp && rm -rf binutils-2.42 build-binutils binutils-2.42.tar.xz

RUN cd /tmp && \
    curl -LO https://ftp.gnu.org/gnu/gcc/gcc-14.1.0/gcc-14.1.0.tar.xz && \
    tar xf gcc-14.1.0.tar.xz && \
    mkdir build-gcc && cd build-gcc && \
    ../gcc-14.1.0/configure --target=$TARGET --prefix=$PREFIX \
        --disable-nls --enable-languages=c --without-headers && \
    make -j$(nproc) all-gcc all-target-libgcc && \
    make install-gcc install-target-libgcc && \
    cd /tmp && rm -rf gcc-14.1.0 build-gcc gcc-14.1.0.tar.xz

# Disk formatting, partitioning, and file copy tools
RUN apt-get update -qq && apt-get install -y -qq dosfstools fdisk util-linux mtools python3 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
