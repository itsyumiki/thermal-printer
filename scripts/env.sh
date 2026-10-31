# Shared settings for the build scripts. Sourced by the other scripts; override
# any of these from the environment.

# Linux x86_64 machine that has (or will get, via setup-build-host.sh) the
# JieLi toolchain. Also the machine the printer is plugged into for flashing.
: "${BUILD_HOST:=karna}"

# Where the JieLi SDK clone, build trees and Python venv live on that machine.
: "${JL_HOME:=$HOME/jl}"

# Where this repo's firmware/ scripts/ tools/ get synced to on that machine.
: "${REMOTE_DIR:=thermal-printer}"

# Pinned upstream SDK (official JieLi Bluetooth SDK, BR23 family among others).
SDK_URL=https://github.com/Jieli-Tech/fw-AC63_BT_SDK.git
SDK_COMMIT=fdc018de81629e21e594d677cf05f87d288be0f0

# Pinned toolchain archives. The pkgman.jieliapp.com links are aliases that
# can change; the sha256 values below are what this project was validated with.
TOOLCHAIN_URL=https://pkgman.jieliapp.com/s/linux-toolchain
TOOLCHAIN_SHA256=a1545cddbc451a06bac43eb5cd96d831470c42a97c5c0b1f98879a20fc7647bd
POSTBUILD_URL=https://pkgman.jieliapp.com/s/linux-postbuild
POSTBUILD_SHA256=6949d48c4cd82e274d7943d4ca67f214593fdb4f699ef9f51c2ecc78125ab739

# Fixed install locations expected by the SDK Makefiles and download.sh.
JIELI_PREFIX=/opt/jieli
UTILS_PREFIX=/opt/utils
