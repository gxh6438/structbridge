#!/usr/bin/env bash
# Build the Windows .dll for Endstone using an MSVC-ABI cross toolchain.
#
# Endstone's Windows server is built with MSVC, so the plugin must use the
# MSVC C++ ABI. This script cross-compiles from Linux with:
#   - clang-cl + lld-link (MSVC ABI frontend)
#   - MSVC CRT / Windows SDK headers and libs fetched via xwin
#
# Layout produced by: xwin --accept-license --arch x86_64 splat --output /opt/msvc
#
# structbridge additionally includes Endstone's INTERNAL headers (src/) for
# EndstoneLevel/EndstoneServer and the native Level/Tag/IDataOutput shims,
# so the same dependency surface as the Linux build applies (RakNet shims,
# entt flags, cpptrace stub, boost/glm from the host include path).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENDSTONE_SRC="${ENDSTONE_SRC:-/workspace/endstone/src}"
ENDSTONE_INC="${ENDSTONE_INC:-/workspace/endstone/include}"
MSVC="${MSVC:-/opt/msvc}"
CLANG_CL="${CLANG_CL:-$(command -v clang-cl-19 || echo /usr/local/bin/clang-cl-19)}"
export PATH="/usr/lib/llvm-19/bin:${PATH}"
OUT="${ROOT}/dist/endstone_structbridge.dll"

mkdir -p "${ROOT}/dist"

# Generated headers (shared with the Linux build; identical content - the
# RakNetTypes shim has per-platform branches inside).
GEN_INC="${ROOT}/build/include"
mkdir -p "${GEN_INC}/endstone" "${GEN_INC}/cpptrace"

cat > "${GEN_INC}/endstone/version.h" <<'EOF'
#define ENDSTONE_VERSION_MAJOR 0
#define ENDSTONE_VERSION_MINOR 11
#define ENDSTONE_VERSION_PATCH 14
#define ENDSTONE_API_VERSION "0.11"
#define ENDSTONE_VERSION "0.11.14"
EOF

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
#include <WinSock2.h>
#include <WS2tcpip.h>
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

cat > "${GEN_INC}/RakPeerInterface.h" <<'EOF'
#pragma once

namespace RakNet {
class RakPeerInterface;
}  // namespace RakNet
EOF

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

# NOTE: the source file must be passed as a RELATIVE path. clang-cl parses
# arguments starting with "/" as MSVC-style options, and an absolute path
# like /workspace/... happens to parse as the /w<level> option, silently
# consuming the path ("no input files").
cd "${ROOT}"

"${CLANG_CL}" \
    /nologo /std:c++20 /EHsc /O2 /MD /LD \
    /W3 \
    /X \
    -DENTT_SPARSE_PAGE=2048 \
    -DENTT_NO_MIXIN \
    -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH \
    /I"${ROOT}/vendor" \
    /I"${ROOT}/vendor/entt" \
    /I"${ROOT}/src" \
    /I"${ROOT}/include" \
    /I"${ENDSTONE_SRC}" \
    /I"${ENDSTONE_INC}" \
    /I"${GEN_INC}" \
    /I"${ROOT}/vendor/host-includes" \
    /I"${MSVC}/crt/include" \
    /I"${MSVC}/sdk/include/um" \
    /I"${MSVC}/sdk/include/ucrt" \
    /I"${MSVC}/sdk/include/shared" \
    /I"${MSVC}/sdk/include/winrt" \
    /I"${MSVC}/sdk/include/cppwinrt" \
    -fuse-ld=lld-link \
    src/structbridge.cpp \
    /Fe:"${OUT}" \
    /link \
    /libpath:"${MSVC}/crt/lib/x86_64" \
    /libpath:"${MSVC}/sdk/lib/um/x86_64" \
    /libpath:"${MSVC}/sdk/lib/ucrt/x86_64"

echo "[ok] built ${OUT}"
