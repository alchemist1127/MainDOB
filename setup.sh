#!/bin/bash
# MainDOB setup.sh - Installs build dependencies
# Supports: Ubuntu/Debian, Arch, Fedora, macOS (Homebrew)

set -e

echo "========================================="
echo "  MainDOB Development Environment Setup"
echo "========================================="
echo ""

# Detect OS
OS="$(uname -s)"
ARCH="$(uname -m)"

echo "Detected: $OS ($ARCH)"
echo ""

# ==============================
# macOS
# ==============================
if [ "$OS" = "Darwin" ]; then
    # Check Homebrew
    if ! command -v brew &>/dev/null; then
        echo "ERROR: Homebrew not found."
        echo "Install it from https://brew.sh then re-run this script."
        exit 1
    fi

    echo "[1/3] Installing packages via Homebrew..."
    brew install nasm xorriso qemu 2>/dev/null || brew upgrade nasm xorriso qemu 2>/dev/null || true

    echo ""
    echo "[2/3] Installing i686-elf cross-compiler..."

    # Try direct formulas first (some taps provide these)
    if brew list i686-elf-gcc &>/dev/null 2>&1; then
        echo "[OK] i686-elf-gcc already installed."
    else
        # Build cross-compiler from source (macOS-compatible)
        echo "Building cross-compiler from source (10-20 minutes)..."
        echo ""

        export PREFIX="$HOME/opt/cross"
        export TARGET=i686-elf
        export PATH="$PREFIX/bin:$PATH"

        BINUTILS_VER=2.42
        GCC_VER=14.1.0
        WORKDIR="/tmp/maindob-toolchain"

        mkdir -p "$WORKDIR" && cd "$WORKDIR"

        # Binutils
        if [ ! -f "binutils-${BINUTILS_VER}.tar.xz" ]; then
            echo "Downloading binutils ${BINUTILS_VER}..."
            curl -LO "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
        fi
        tar xf "binutils-${BINUTILS_VER}.tar.xz"
        rm -rf build-binutils && mkdir build-binutils && cd build-binutils
        ../binutils-${BINUTILS_VER}/configure \
            --target=$TARGET \
            --prefix="$PREFIX" \
            --with-sysroot \
            --disable-nls \
            --disable-werror \
            --disable-gprofng
        make -j$(sysctl -n hw.ncpu)
        make install
        cd "$WORKDIR"

        # GCC
        if [ ! -f "gcc-${GCC_VER}.tar.xz" ]; then
            echo "Downloading GCC ${GCC_VER}..."
            curl -LO "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
        fi
        tar xf "gcc-${GCC_VER}.tar.xz"

        # Download GCC prerequisites (gmp, mpfr, mpc)
        cd "gcc-${GCC_VER}"
        if [ ! -d gmp ]; then
            ./contrib/download_prerequisites
        fi
        cd "$WORKDIR"

        rm -rf build-gcc && mkdir build-gcc && cd build-gcc
        ../gcc-${GCC_VER}/configure \
            --target=$TARGET \
            --prefix="$PREFIX" \
            --disable-nls \
            --enable-languages=c \
            --without-headers \
            --with-system-zlib
        make -j$(sysctl -n hw.ncpu) all-gcc all-target-libgcc
        make install-gcc install-target-libgcc
        cd "$WORKDIR"

        echo ""
        echo "[OK] Cross-compiler installed to $PREFIX"

        # Cleanup
        rm -rf "$WORKDIR"
    fi

    # Add to PATH permanently
    SHELL_RC=""
    if [ -f "$HOME/.zshrc" ]; then
        SHELL_RC="$HOME/.zshrc"
    elif [ -f "$HOME/.bashrc" ]; then
        SHELL_RC="$HOME/.bashrc"
    elif [ -f "$HOME/.bash_profile" ]; then
        SHELL_RC="$HOME/.bash_profile"
    fi

    if [ -n "$SHELL_RC" ]; then
        if ! grep -q "maindob cross-compiler" "$SHELL_RC" 2>/dev/null; then
            echo "" >> "$SHELL_RC"
            echo "# maindob cross-compiler" >> "$SHELL_RC"
            echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> "$SHELL_RC"
            echo "[OK] Added to $SHELL_RC"
        fi
    fi

    # grub-mkrescue on macOS: needs special handling
    # xorriso is the backend; grub-mkrescue might not exist on macOS
    if ! command -v grub-mkrescue &>/dev/null; then
        echo ""
        echo "[NOTE] grub-mkrescue not found on macOS."
        echo "       You have two options:"
        echo ""
        echo "  Option A (recommended): Use Docker"
        echo "    docker run --rm -v \$(pwd):/src -w /src ubuntu:24.04 bash -c \\"
        echo "      'apt-get update && apt-get install -y grub-pc-bin grub-common xorriso && make iso'"
        echo ""
        echo "  Option B: Install GRUB for i386-pc target via Homebrew or from source"
        echo "    This is tricky on macOS. Docker is easier."
        echo ""
    fi

    echo ""
    echo "========================================="
    echo "  Setup complete!"
    echo "========================================="
    echo ""
    echo "  Cross-compiler: $HOME/opt/cross/bin/i686-elf-gcc"
    echo ""
    echo "  IMPORTANT: Close and reopen your terminal, then:"
    echo "    cd $(pwd)"
    echo "    make run"
    echo ""
    exit 0
fi

# ==============================
# Linux
# ==============================

# Detect distro
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO="$ID"
else
    DISTRO="unknown"
fi

echo "Detected distro: $DISTRO"
echo ""

case "$DISTRO" in
    ubuntu|debian|linuxmint|pop)
        echo "[1/3] Installing base packages..."
        sudo apt update -qq
        sudo apt install -y nasm grub-pc-bin grub-common xorriso \
            qemu-system-x86 gdb build-essential bison flex \
            libgmp-dev libmpc-dev libmpfr-dev texinfo curl
        ;;
    arch|manjaro|endeavouros)
        echo "[1/3] Installing base packages..."
        sudo pacman -Syu --noconfirm --needed nasm grub xorriso \
            qemu-system-x86 gdb base-devel
        if pacman -Si i686-elf-gcc &>/dev/null 2>&1; then
            sudo pacman -S --noconfirm --needed i686-elf-gcc i686-elf-binutils
            echo "[OK] Cross-compiler installed from packages."
            echo ""
            echo "Setup complete! Run 'make run' to build and launch."
            exit 0
        fi
        ;;
    fedora|rhel|centos)
        echo "[1/3] Installing base packages..."
        sudo dnf install -y nasm grub2-tools xorriso \
            qemu-system-x86 gdb gcc gcc-c++ make bison flex \
            gmp-devel libmpc-devel mpfr-devel texinfo curl
        ;;
    *)
        echo "Unknown distro. Please install manually:"
        echo "  nasm, grub-mkrescue, xorriso, qemu-system-i386, gdb"
        echo "  gcc, make, bison, flex, libgmp-dev, libmpc-dev, libmpfr-dev, texinfo"
        echo ""
        ;;
esac

# Check if cross-compiler already exists
if command -v i686-elf-gcc &>/dev/null; then
    echo "[OK] i686-elf-gcc already installed: $(i686-elf-gcc --version | head -1)"
    echo ""
    echo "Setup complete! Run 'make run' to build and launch."
    exit 0
fi

# Build cross-compiler from source
echo ""
echo "[2/3] Building i686-elf cross-compiler (this takes 10-20 minutes)..."
echo ""

export PREFIX="$HOME/opt/cross"
export TARGET=i686-elf
export PATH="$PREFIX/bin:$PATH"

BINUTILS_VER=2.42
GCC_VER=14.1.0

mkdir -p /tmp/maindob-toolchain && cd /tmp/maindob-toolchain

# Binutils
if [ ! -f "binutils-${BINUTILS_VER}.tar.xz" ]; then
    echo "Downloading binutils ${BINUTILS_VER}..."
    curl -LO "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
fi
tar xf "binutils-${BINUTILS_VER}.tar.xz"
rm -rf build-binutils && mkdir -p build-binutils && cd build-binutils
../binutils-${BINUTILS_VER}/configure --target=$TARGET --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j$(nproc)
make install
cd ..

# GCC
if [ ! -f "gcc-${GCC_VER}.tar.xz" ]; then
    echo "Downloading GCC ${GCC_VER}..."
    curl -LO "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
fi
tar xf "gcc-${GCC_VER}.tar.xz"
rm -rf build-gcc && mkdir -p build-gcc && cd build-gcc
../gcc-${GCC_VER}/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j$(nproc) all-gcc all-target-libgcc
make install-gcc install-target-libgcc
cd ..

echo ""
echo "[3/3] Cross-compiler installed to $PREFIX"
echo ""

# Add to PATH
SHELL_RC=""
if [ -n "$ZSH_VERSION" ] || [ -f "$HOME/.zshrc" ]; then
    SHELL_RC="$HOME/.zshrc"
elif [ -f "$HOME/.bashrc" ]; then
    SHELL_RC="$HOME/.bashrc"
fi

if [ -n "$SHELL_RC" ]; then
    if ! grep -q "maindob cross-compiler" "$SHELL_RC" 2>/dev/null; then
        echo "" >> "$SHELL_RC"
        echo "# maindob cross-compiler" >> "$SHELL_RC"
        echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> "$SHELL_RC"
        echo "Added to $SHELL_RC. Run 'source $SHELL_RC' or open a new terminal."
    fi
fi

echo "========================================="
echo "  Setup complete!"
echo "========================================="
echo ""
echo "  Cross-compiler: $PREFIX/bin/i686-elf-gcc"
echo ""
echo "  To build and run MainDOB:"
echo "    export PATH=\"\$HOME/opt/cross/bin:\$PATH\""
echo "    make run"
echo ""

# Cleanup
rm -rf /tmp/maindob-toolchain
