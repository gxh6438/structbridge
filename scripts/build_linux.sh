#!/usr/bin/env bash
# Build the Linux .so for Endstone with a low glibc requirement.
#
# Toolchain: clang-18 + static libc++ 18 (matching Endstone's Linux ABI, which
# also matches BDS's Linux build - clang + libc++).
# glibc compatibility: src/glibc_compat.h redirects symbols that modern glibc
# headers bind to new versions (see the header for details), and this script
# audits the final binary to guarantee no symbol requires a glibc newer than
# GLIBC_TARGET.
#
# structbridge additionally includes Endstone's INTERNAL headers (src/) for
# EndstoneLevel/EndstoneServer and the native Level/Tag/IDataOutput shims.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENDSTONE_SRC="${ENDSTONE_SRC:-/workspace/endstone/src}"
ENDSTONE_INC="${ENDSTONE_INC:-/workspace/endstone/include}"
GLIBC_TARGET="${GLIBC_TARGET:-2.17}"
OUT="${ROOT}/dist/endstone_structbridge.so"

# clang-19 + static libc++ 19 preferred (clang-18's libc++ 18.1.3 has a
# std::format consteval bug that breaks endstone's platform/callstack.h).
# Override CXX / LIBCXX_A / LIBCXXABI_A when only clang-18 is available.
CXX="${CXX:-clang++-19}"
LIBCXX_A="${LIBCXX_A:-/usr/lib/llvm-19/lib/libc++.a}"
LIBCXXABI_A="${LIBCXXABI_A:-/usr/lib/llvm-19/lib/libc++abi.a}"
# LIBCXX_INC optionally points at newer libc++ HEADERS (e.g. an extracted
# libc++-19-dev) used with clang-18 via -nostdinc++ -isystem.
LIBCXX_INC="${LIBCXX_INC:-}"
EXTRA_CXXFLAGS=()
if [[ -n "${LIBCXX_INC}" ]]; then
    EXTRA_CXXFLAGS+=(-nostdinc++ -isystem "${LIBCXX_INC}")
fi

# Generated version header (ENDSTONE_VERSION etc.)
GEN_INC="${ROOT}/build/include"
mkdir -p "${GEN_INC}/endstone" "${ROOT}/dist"
cat > "${GEN_INC}/endstone/version.h" <<'EOF'
#define ENDSTONE_VERSION_MAJOR 0
#define ENDSTONE_VERSION_MINOR 11
#define ENDSTONE_VERSION_PATCH 14
#define ENDSTONE_API_VERSION "0.11"
#define ENDSTONE_VERSION "0.11.14"
EOF

# Minimal RakNet PacketPriority shim (needed by endstone's internal packet.h).
# Values/names follow the real RakNet enum (endstone references
# PacketPriority::MEDIUM_PRIORITY).
cat > "${GEN_INC}/PacketPriority.h" <<'EOF'
#pragma once

enum PacketPriority {
    IMMEDIATE_PRIORITY,
    HIGH_PRIORITY,
    MEDIUM_PRIORITY,
    LOW_PRIORITY,
    NUMBER_OF_PRIORITIES
};
EOF

# Minimal RakNetTypes shim. Endstone's internal headers use these types:
#   network_identifier.h : RakNet::RakNetGUID (by value) + sockaddr_* (which the
#                          real RakNetTypes.h pulls in, so we must too)
#   rak_peer_helper.h    : RakNet::StartupResult (by value)
#   socket_address.h     : RakNet::SystemAddress (by reference)
# Layouts cross-checked against LeviLamina's PDB-derived RakNet headers.
cat > "${GEN_INC}/RakNetTypes.h" <<'EOF'
#pragma once

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace RakNet {

struct RakNetGUID {
    std::uint64_t g;
    std::uint16_t systemIndex;

    RakNetGUID() : g(0), systemIndex(0) {}
    RakNetGUID(std::uint64_t g_, std::uint16_t index) : g(g_), systemIndex(index) {}
};

struct SystemAddress {
    union {
        sockaddr_storage sa_stor;
        sockaddr_in6 addr6;
        sockaddr_in addr4;
    } address;
    std::uint16_t debugPort;
    std::uint16_t systemIndex;

    SystemAddress() : address(), debugPort(0), systemIndex(0) {}
};

enum class StartupResult : int {
    RaknetStarted               = 0,
    RaknetAlreadyStarted        = 1,
    InvalidSocketDescriptors    = 2,
    InvalidMaxConnections       = 3,
    SocketFamilyNotSupported    = 4,
    SocketPortAlreadyInUse      = 5,
    SocketFailedToBind          = 6,
    SocketFailedTestSend        = 7,
    PortCannotBeZero            = 8,
    FailedToCreateNetworkThread = 9,
    CouldNotGenerateGuid        = 10,
    StartupOtherFailure         = 11,
};

}  // namespace RakNet
EOF

# Minimal RakPeerInterface shim: endstone's internal headers only use
# RakNet::RakPeerInterface by pointer/reference, so a forward declaration is
# enough (never dereferenced inside this plugin).
cat > "${GEN_INC}/RakPeerInterface.h" <<'EOF'
#pragma once

namespace RakNet {
class RakPeerInterface;
}  // namespace RakNet
EOF

# Minimal cpptrace shim: endstone's core/server.h includes crash_handler.h,
# which only DECLARES functions taking cpptrace::stacktrace by reference.
# The plugin never instantiates them, so the type shape is all we need.
mkdir -p "${GEN_INC}/cpptrace"
cat > "${GEN_INC}/cpptrace/cpptrace.hpp" <<'EOF'
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cpptrace {

struct stacktrace_frame {
    std::uintptr_t address = 0;
    std::uintptr_t object_address = 0;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string filename;
    std::string symbol;
    bool symbolic_resolved = false;
    bool source_resolved = false;
};

class stacktrace {
public:
    stacktrace() = default;
    ~stacktrace() = default;

private:
    std::vector<stacktrace_frame> frames_;
};

}  // namespace cpptrace
EOF

# Endstone's entity_id.h requires these EnTT configuration macros.
ENTT_FLAGS="-DENTT_SPARSE_PAGE=2048 -DENTT_NO_MIXIN"

# Python headers: the plugin injects an in-process "structbridge" module into
# sys.modules so Python plugins can call the API (libpython is dynamically
# linked by the endstone runtime, so Py* symbols resolve at runtime).
PYTHON_INC="${PYTHON_INC:-$(python3 -c "import sysconfig; print(sysconfig.get_paths()['include'])" 2>/dev/null || true)}"
if [[ -n "${PYTHON_INC}" && -d "${PYTHON_INC}" ]]; then
    PYTHON_FLAGS=(-isystem "${PYTHON_INC}")
else
    echo "[warn] Python headers not found - Python API module disabled" >&2
    PYTHON_FLAGS=()
fi

"${CXX}" \
    -std=c++20 -stdlib=libc++ -nostdlib++ "${EXTRA_CXXFLAGS[@]}" \
    -shared -fPIC -fvisibility=hidden -fvisibility-inlines-hidden \
    -O2 -g1 -Wall -Wextra -Wno-unused-parameter \
    ${ENTT_FLAGS} \
    -I"${ROOT}/vendor" \
    -I"${ROOT}/vendor/entt" \
    -I"${ROOT}/src" \
    -I"${ROOT}/include" \
    -I"${ENDSTONE_SRC}" \
    -I"${ENDSTONE_INC}" \
    -I"${GEN_INC}" \
    "${PYTHON_FLAGS[@]}" \
    "${ROOT}/src/structbridge.cpp" \
    "${ROOT}/src/glibc_compat_impl.cpp" \
    "${LIBCXX_A}" "${LIBCXXABI_A}" \
    -Wl,--no-as-needed,/usr/lib/x86_64-linux-gnu/libpthread.so.0,/usr/lib/x86_64-linux-gnu/libdl.so.2 -Wl,--as-needed -lm \
    -o "${OUT}"

# Audit: fail if any dynamic symbol requires a glibc newer than the target.
version_gt() {
    [[ "$(printf '%s\n' "$1" "$2" | sort -V | tail -1)" == "$1" && "$1" != "$2" ]]
}

max_version=$(objdump -T "${OUT}" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/^GLIBC_//' | sort -Vu | tail -1)

if [[ -n "${max_version}" ]] && version_gt "${max_version}" "${GLIBC_TARGET}"; then
    echo "[error] ${OUT} requires glibc symbols newer than ${GLIBC_TARGET} (max: ${max_version}):" >&2
    objdump -T "${OUT}" | grep "GLIBC_${max_version}" >&2 || true
    exit 1
fi

echo "[ok] built ${OUT} (max required glibc: ${max_version:-none})"
