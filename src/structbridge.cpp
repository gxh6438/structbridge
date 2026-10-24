// structbridge - Export/Import official .mcstructure files on the server side.
//
// Architecture (zero BDS symbol offsets needed for BOTH directions):
//
//   EXPORT (world region -> .mcstructure, byte-identical to a structure block):
//     1. dispatch the vanilla command
//          structure save <name> <x1> <y1> <z1> <x2> <y2> <z2> <entities> memory <blocks>
//        through the REQUESTER'S origin (a player keeps their dimension; the
//        console origin works fine and is bound to the overworld). The engine
//        captures the region into an in-memory StructureTemplate itself - the
//        plugin never reads blocks, so the result is engine-native by
//        construction.
//     2. endstone::Server::getLevel()              -> public API
//        EndstoneLevel::level_                     -> direct member access
//        Level::getStructureManager()              -> vtable call
//        StructureManager::getStructure            -> vtable call (shim below)
//        StructureTemplate::save()                 -> vtable call -> unique_ptr<Tag>
//        Tag::write(IDataOutput&)                  -> vtable call -> our MemoryDataOutput
//     3. prefix the engine payload with the standard root header (0x0A + empty
//        name, exactly what vanilla .mcstructure files start with) and write
//        plugins/structbridge/exports/<ns>/<name>.mcstructure.
//     Chunk availability: with no players online a dedicated server has NO
//     loaded chunks, and every block-touching command fails with "Cannot
//     access blocks outside of the world" - this is the ROOT CAUSE behind the
//     old "console origin is broken" misdiagnosis. The plugin therefore adds
//     named tickingareas covering the region footprint (BDS: 3-D rect form,
//     max 100 chunks per area, max 10 areas) and retries the save on a timer
//     until the template shows up.
//
//   IMPORT (.mcstructure -> structure manager):
//     1. read + validate the file (little-endian NBT, must contain
//        format_version),
//     2. StructureManager::getOrCreate(name)       -> vtable call
//     3. StructureTemplate::save()                 -> engine-allocated CompoundTag
//     4. Tag::load(IDataInput&)                    -> vtable call: BDS's OWN NBT
//        parser consumes the file bytes through our MemoryDataInput (the
//        plugin never builds NBT by hand),
//     5. StructureTemplate::load(CompoundTag const&) -> vtable call (bool).
//     After that the template is registered in the manager and can be placed
//     with the vanilla /structure load command (bare names auto-complete to
//     the mystructure: namespace, so imports are registered under
//     "mystructure:<name>" when the user gives a bare name).
//
// Self-containment: the endstone runtime does NOT export its internal symbols
// (Linux: -fvisibility=hidden + --exclude-libs,ALL; Windows: no dllexport), so
// this plugin must not reference ANY endstone-core or bedrock-shim function.
// Commands are registered through the plugin description; the native Level
// handle is reached via direct member access; the bedrock-shim constructors
// that header code instantiates are defined locally below.
//
// Safety gate: at startup the plugin PROBES the whole chain (export AND the
// full import roundtrip) inside a fork() sandbox on Linux; a crash in the
// child disables the feature, the server is never at risk. On Windows the gate
// stays closed unless explicitly enabled in the config.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Python API: enabled automatically when Python headers are reachable (the
// Linux build passes the interpreter's include dir). The Windows cross-build
// has no Python headers, and linking a specific python3xx.dll would risk a
// second interpreter instance, so the module is compiled out there - the C++
// API stays fully available.
#if __has_include(<Python.h>)
#define STRUCTBRIDGE_PYTHON_API 1
#include <Python.h>  // in-process "structbridge" module for Python plugins
#endif

#include <nlohmann/json.hpp>

#include "endstone/endstone.hpp"
#include "endstone/version.h"
#include "endstone/actor/actor.h"
#include "endstone/level/dimension.h"
#include "endstone/level/level.h"
#include "bedrock/world/level/level.h"             // native Level shim (pulls level_interface.h)
#include "bedrock/world/level/dimension/dimension.h"
#include "bedrock/util/data_io.h"                  // IDataOutput / IDataInput
#include "bedrock/nbt/tag.h"                       // Tag
#include "bedrock/nbt/compound_tag.h"              // CompoundTag
#include "bedrock/core/string/string_hash.h"       // HashedString (local ctor defs below)
#include "bedrock/resources/base_game_version.h"   // BaseGameVersion (local ctor def below)

// EndstoneLevel (internal header): we need the native ::Level reference it
// wraps. EndstoneLevel::getHandle() is implemented inside the endstone runtime
// and NOT exported, so instead we access the level_ member directly. The
// access-specifier trick below lets us do that while the offset is still
// computed from the runtime's own class definition (all transitive includes
// are pre-seeded above, so the macro only affects this one header).
#define private public
#include "endstone/core/level/level.h"
#undef private

#include "glibc_compat.h"
#define STRUCTBRIDGE_BUILDING 1
#include "structbridge_api.h"
#include "leveldb_scan.h"  // read-only world LevelDB scanner (/structlist world source)

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>  // canonical casing: SDK file is Windows.h (case-sensitive fs)
#else
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Local definitions of bedrock-shim constructors.
//
// Endstone's runtime implements these in its own .cpp files but does not
// export them, while header code instantiated in this plugin references them
// (e.g. the inline variable BaseGameVersion::EMPTY). The definitions below
// replicate endstone's implementations exactly (src/bedrock/core/string/
// string_hash.cpp and src/bedrock/resources/base_game_version.cpp), keeping
// the plugin free of unresolvable dynamic-symbol references.
// ---------------------------------------------------------------------------
HashedString::HashedString(std::nullptr_t) : last_match_(nullptr) {}

HashedString::HashedString(char const *str)
    : str_hash_(HashedString::computeHash(str)), str_(str), last_match_(nullptr)
{
}

BaseGameVersion::BaseGameVersion() = default;

// ---------------------------------------------------------------------------
// Native shims: replicate the vtable PREFIX of the real BDS classes.
//
// Virtual slots are assigned in declaration order by the compiler, per platform
// ABI (Itanium on Linux, MSVC on Windows) - so the compiler computes the correct
// index on each platform automatically. Only the functions we actually call need
// exact signatures; the others are placeholders that merely occupy their slot.
//
// Cross-checked against LeviLamina's PDB-derived headers (which are a factual
// description of the same binary) and against the live BDS 26.52 runtime:
//   IStructureTemplate : ~dtor, randomPalette, rawSize, shouldHandleUpgradeForBlock
//   StructureTemplate (new virtuals in order): load, save, clear, ...
//   IStructureTemplateManager : Bedrock::EnableNonOwnerReferences
//     (dtor only), then getOrCreateJigsawStructureMetadata, getOrCreate,
//     getStructure, getOrCreateLegacy, readStructure
// The save()/getStructure()/getOrCreate() slots have been verified live via the
// fork-sandbox probe; load() sits directly before save() in both shims.
//
// NOTE: "StructureManager" intentionally completes the forward declaration that
// Endstone's own headers already make (bedrock/forward.h); the class below only
// replicates the vtable prefix needed for safe virtual dispatch.
// ---------------------------------------------------------------------------

class StructureTemplate {
public:
    virtual ~StructureTemplate() = default;                       // dtor slots
    virtual void vslot_randomPalette() const = 0;                 // after dtor
    virtual void vslot_rawSize() const = 0;
    virtual void vslot_shouldHandleUpgradeForBlock() const = 0;
    // StructureTemplate's own new virtuals, declaration order:
    virtual bool vslot_load(CompoundTag const &) = 0;             // CALLED (import)
    [[nodiscard]] virtual std::unique_ptr<Tag> save() const = 0;  // CALLED (both)
};

class StructureManager {
public:
    virtual ~StructureManager() = default;                                     // dtor slots
    virtual void *vslot_getOrCreateJigsawStructureMetadata() = 0;              // never called
    [[nodiscard]] virtual StructureTemplate &getOrCreate(std::string const &) = 0;   // CALLED (import)
    [[nodiscard]] virtual StructureTemplate *getStructure(std::string const &) const = 0;  // CALLED (export)
    virtual void *vslot_getOrCreateLegacy() = 0;                               // never called
    virtual bool vslot_readStructure() = 0;                                    // never called
};

// ---------------------------------------------------------------------------
// MemoryDataOutput: our IDataOutput sink. Virtual dispatch from BDS's
// CompoundTag::write lands in these overrides; ABI matches because the plugin is
// compiled with the same compiler family / STL as BDS (clang+libc++ on Linux,
// MSVC ABI on Windows).
// ---------------------------------------------------------------------------
class MemoryDataOutput final : public IDataOutput {
public:
    MemoryDataOutput() = default;

    void writeString(std::string_view v) override
    {
        pushU16(static_cast<std::uint16_t>(v.size()));
        append(v.data(), v.size());
    }

    void writeLongString(std::string_view v) override
    {
        writeString(v);  // .mcstructure paths only need the u16 form
    }

    void writeFloat(float v) override
    {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        pushU32(bits);
    }

    void writeDouble(double v) override
    {
        std::uint64_t bits;
        std::memcpy(&bits, &v, 8);
        pushU64(bits);
    }

    void writeByte(char v) override
    {
        buffer_.push_back(static_cast<std::uint8_t>(v));
    }

    void writeShort(std::int16_t v) override
    {
        pushU16(static_cast<std::uint16_t>(v));
    }

    void writeInt(std::int32_t v) override
    {
        pushU32(static_cast<std::uint32_t>(v));
    }

    void writeLongLong(std::int64_t v) override
    {
        pushU64(static_cast<std::uint64_t>(v));
    }

    void writeBytes(void const *data, std::size_t bytes) override
    {
        append(data, bytes);
    }

    [[nodiscard]] const std::vector<std::uint8_t> &data() const
    {
        return buffer_;
    }

    std::vector<std::uint8_t> takeData()
    {
        return std::move(buffer_);
    }

    void appendRaw(std::uint8_t byte)
    {
        buffer_.push_back(byte);
    }

    void appendRaw16(std::uint16_t v)
    {
        pushU16(v);
    }

private:
    void append(void const *data, std::size_t n)
    {
        const auto *p = static_cast<const std::uint8_t *>(data);
        buffer_.insert(buffer_.end(), p, p + n);
    }
    void pushU16(std::uint16_t v)
    {
        buffer_.push_back(static_cast<std::uint8_t>(v & 0xFF));
        buffer_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }
    void pushU32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            buffer_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }
    void pushU64(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i) {
            buffer_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }

    std::vector<std::uint8_t> buffer_;
};

// ---------------------------------------------------------------------------
// MemoryDataInput: our IDataInput source for the import path. The engine's own
// CompoundTag::load pulls bytes from here (little-endian), so the plugin never
// needs its own NBT parser.
// ---------------------------------------------------------------------------
class MemoryDataInput final : public IDataInput {
public:
    explicit MemoryDataInput(const std::vector<std::uint8_t> &data) : data_(data) {}

    Bedrock::Result<std::string> readStringResult() override
    {
        std::uint16_t len;
        if (!readU16(len)) {
            return inputError<std::string>();
        }
        std::string s;
        if (len != 0) {
            s.resize(len);
            if (!readRaw(&s[0], len)) {
                return inputError<std::string>();
            }
        }
        return s;
    }

    Bedrock::Result<std::string> readLongStringResult() override
    {
        std::uint32_t len;
        if (!readU32(len)) {
            return inputError<std::string>();
        }
        if (len > data_.size()) {  // sanity cap before resize
            return inputError<std::string>();
        }
        std::string s;
        if (len != 0) {
            s.resize(len);
            if (!readRaw(&s[0], len)) {
                return inputError<std::string>();
            }
        }
        return s;
    }

    Bedrock::Result<float> readFloatResult() override
    {
        std::uint8_t b[4];
        if (!readRaw(b, 4)) {
            return inputError<float>();
        }
        std::uint32_t bits = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
                             (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }

    Bedrock::Result<double> readDoubleResult() override
    {
        std::uint8_t b[8];
        if (!readRaw(b, 8)) {
            return inputError<double>();
        }
        std::uint64_t bits = 0;
        for (int i = 7; i >= 0; --i) {
            bits = (bits << 8) | b[i];
        }
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }

    Bedrock::Result<std::uint8_t> readByteResult() override
    {
        std::uint8_t v;
        if (!readRaw(&v, 1)) {
            return inputError<std::uint8_t>();
        }
        return v;
    }

    Bedrock::Result<std::int16_t> readShortResult() override
    {
        std::uint16_t v;
        if (!readU16(v)) {
            return inputError<std::int16_t>();
        }
        return static_cast<std::int16_t>(v);
    }

    Bedrock::Result<std::int32_t> readIntResult() override
    {
        std::uint32_t v;
        if (!readU32(v)) {
            return inputError<std::int32_t>();
        }
        return static_cast<std::int32_t>(v);
    }

    Bedrock::Result<std::int64_t> readLongLongResult() override
    {
        std::uint64_t v;
        if (!readU64(v)) {
            return inputError<std::int64_t>();
        }
        return static_cast<std::int64_t>(v);
    }

    Bedrock::Result<void> readBytesResult(void *dst, std::uint64_t bytes) override
    {
        if (!readRaw(dst, static_cast<std::size_t>(bytes))) {
            return inputError<void>();
        }
        return {};
    }

    [[nodiscard]] std::size_t numBytesLeft() const override
    {
        return pos_ >= data_.size() ? 0 : data_.size() - pos_;
    }

    // Skips the .mcstructure root header: [0x0A][u16 name length][name bytes].
    // Vanilla files always use an empty root name, but any name is accepted.
    bool skipStructureHeader()
    {
        std::uint8_t type = 0;
        std::uint16_t name_len = 0;
        if (!readRaw(&type, 1) || type != 0x0A) {
            return false;
        }
        if (!readU16(name_len)) {
            return false;
        }
        return skip(name_len);
    }

    bool skip(std::size_t bytes)
    {
        if (bytes > numBytesLeft()) {
            return false;
        }
        pos_ += bytes;
        return true;
    }

private:
    template <typename T>
    static Bedrock::Result<T> inputError()
    {
        return Bedrock::Result<T>(BEDROCK_NEW_ERROR(std::errc::bad_message));
    }

    bool readRaw(void *dst, std::size_t n)
    {
        if (n > numBytesLeft()) {
            return false;
        }
        if (n != 0) {
            std::memcpy(dst, data_.data() + pos_, n);
        }
        pos_ += n;
        return true;
    }

    bool readU16(std::uint16_t &v)
    {
        std::uint8_t b[2];
        if (!readRaw(b, 2)) {
            return false;
        }
        v = static_cast<std::uint16_t>(b[0]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[1]) << 8);
        return true;
    }

    bool readU32(std::uint32_t &v)
    {
        std::uint8_t b[4];
        if (!readRaw(b, 4)) {
            return false;
        }
        v = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
            (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        return true;
    }

    bool readU64(std::uint64_t &v)
    {
        std::uint8_t b[8];
        if (!readRaw(b, 8)) {
            return false;
        }
        v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8) | b[i];
        }
        return true;
    }

    const std::vector<std::uint8_t> &data_;
    std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Minimal little-endian NBT validator (used to verify probe/export output and
// to sanity-check files before import).
// ---------------------------------------------------------------------------
namespace nbt_check {

namespace {
struct Reader {
    const std::uint8_t *p;
    std::size_t n;
    std::size_t off = 0;
    bool ok = true;

    bool u8(std::uint8_t &out)
    {
        if (!ok || off + 1 > n) {
            ok = false;
            return false;
        }
        out = p[off++];
        return true;
    }
    bool i32(std::int32_t &out)
    {
        if (!ok || off + 4 > n) {
            ok = false;
            return false;
        }
        out = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[off]) |
                                        (static_cast<std::uint32_t>(p[off + 1]) << 8) |
                                        (static_cast<std::uint32_t>(p[off + 2]) << 16) |
                                        (static_cast<std::uint32_t>(p[off + 3]) << 24));
        off += 4;
        return true;
    }
    bool u16(std::uint16_t &out)
    {
        if (!ok || off + 2 > n) {
            ok = false;
            return false;
        }
        out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[off]) |
                                         (static_cast<std::uint16_t>(p[off + 1]) << 8));
        off += 2;
        return true;
    }
    bool skip(std::size_t bytes)
    {
        if (!ok || bytes > n || off > n - bytes) {
            ok = false;
            return false;
        }
        off += bytes;
        return true;
    }
};

bool readPayload(Reader &r, std::uint8_t type, int depth);

bool readCompound(Reader &r, int depth)
{
    if (depth > 512) {
        r.ok = false;
        return false;
    }
    for (;;) {
        std::uint8_t type;
        if (!r.u8(type)) {
            return false;
        }
        if (type == 0) {
            return true;  // TAG_End
        }
        std::uint16_t name_len;
        if (!r.u16(name_len) || !r.skip(name_len)) {
            return false;
        }
        if (!readPayload(r, type, depth + 1)) {
            return false;
        }
    }
}

bool readPayload(Reader &r, std::uint8_t type, int depth)
{
    std::int32_t len;
    std::uint16_t slen;
    std::uint8_t elem;
    switch (type) {
        case 1:  // Byte
            return r.skip(1);
        case 2:  // Short
            return r.skip(2);
        case 3:  // Int
            return r.skip(4);
        case 4:  // Long
            return r.skip(8);
        case 5:  // Float
            return r.skip(4);
        case 6:  // Double
            return r.skip(8);
        case 7:  // ByteArray
            return r.i32(len) && len >= 0 && r.skip(static_cast<std::size_t>(len));
        case 8:  // String
            return r.u16(slen) && r.skip(slen);
        case 9:  // List
            if (!r.u8(elem) || !r.i32(len)) {
                return false;
            }
            if (len < 0) {
                r.ok = false;
                return false;
            }
            for (std::int32_t i = 0; i < len; ++i) {
                if (!readPayload(r, elem, depth + 1)) {
                    return false;
                }
            }
            return true;
        case 10:  // Compound
            return readCompound(r, depth);
        case 11:  // IntArray
            return r.i32(len) && len >= 0 && r.skip(static_cast<std::size_t>(len) * 4);
        default:
            r.ok = false;
            return false;
    }
}
}  // namespace

// Validates a complete .mcstructure byte stream (root: [0x0A][u16 name len][name][payload]).
// If required_key is non-empty, the root compound must contain that key.
bool validate(const std::vector<std::uint8_t> &data, const std::string &required_key)
{
    Reader r{data.data(), data.size()};
    std::uint8_t root_type;
    if (!r.u8(root_type) || root_type != 10) {
        return false;
    }
    std::uint16_t root_name_len;
    if (!r.u16(root_name_len) || !r.skip(root_name_len)) {
        return false;
    }
    if (required_key.empty()) {
        return readCompound(r, 0) && r.off == data.size();
    }
    // Walk the root compound; check for the required key while parsing.
    int depth_guard = 0;
    for (;;) {
        if (++depth_guard > 1024) {
            return false;
        }
        std::uint8_t type;
        if (!r.u8(type) || type == 0) {
            return false;  // reached end without finding the key
        }
        std::uint16_t name_len;
        if (!r.u16(name_len)) {
            return false;
        }
        std::string key(reinterpret_cast<const char *>(data.data() + r.off), name_len);
        if (!r.skip(name_len)) {
            return false;
        }
        if (key == required_key) {
            // Key exists; still require the rest of the document to parse cleanly.
            Reader full{data.data(), data.size()};
            std::uint8_t rt;
            std::uint16_t rnl;
            if (!full.u8(rt) || rt != 10 || !full.u16(rnl) || !full.skip(rnl)) {
                return false;
            }
            return readCompound(full, 0) && full.off == data.size();
        }
        if (!readPayload(r, type, 1)) {
            return false;
        }
    }
}

}  // namespace nbt_check

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------
namespace structbridge {

struct Config {
    int version = 3;
    // Canonical structure store (was "exports" pre-0.1.0; auto-migrated).
    std::string library_dir = "library";
    // Legacy key kept only so existing config.json files can be migrated.
    std::string output_dir_legacy;
    bool include_entities = true;
    bool include_blocks = true;
    bool probe_enabled = true;
    int probe_timeout_seconds = 15;
    std::string probe_template_name = "structbridge:__probe__";
    bool require_op = true;
    // Windows has no fork sandbox; the native gate stays closed unless explicitly allowed.
    bool windows_allow_without_probe = false;
    std::string windows_expected_minecraft_version;  // empty = any
    bool diagnostic_logging = true;
    // Export retry behaviour: the region's chunks are force-loaded with named
    // tickingareas and the vanilla save command is re-dispatched until the
    // template appears (or the attempt budget is spent).
    int export_max_attempts = 20;
    int export_retry_interval_ticks = 20;  // 20 ticks = 1 s
    int export_area_chunk_limit = 100;     // BDS hard limit per tickingarea
    int export_max_areas = 10;             // BDS global tickingarea limit
};

enum class GateState { NotProbed, Available, Unavailable };

// Probe exit codes (also used as failure reasons).
enum ProbeStage : int {
    STAGE_OK = 0,
    STAGE_NO_LEVEL = 10,
    STAGE_MANAGER = 11,
    STAGE_GET_STRUCTURE = 12,
    STAGE_SAVE = 13,
    STAGE_WRITE = 14,
    STAGE_PARSE = 15,
    STAGE_FILE = 16,
    STAGE_NBT_READ = 18,
    STAGE_IMPORT_PARSE = 20,
    STAGE_TEMPLATE_LOAD = 21,
    STAGE_IMPORT_MISMATCH = 22,
    // API-only rejection reasons (never produced by the native chain itself).
    STAGE_API_ARG = 30,
    STAGE_API_SIZE = 31,
    STAGE_API_DIM = 32,
    STAGE_API_GATE = 33,
    STAGE_API_NOT_FOUND = 34,
    STAGE_API_TIMEOUT = 35,
    // v0.1 placement / bundle stages.
    STAGE_PLACE_AREA = 40,       // could not load the target chunks (areas + TP)
    STAGE_PLACE_LOAD = 41,       // vanilla structure load did not take effect
    STAGE_BUNDLE_MANIFEST = 42,  // manifest missing / unreadable / inconsistent
    STAGE_BUNDLE_TILE = 43,      // a tile export failed (bundle left partial)
    STAGE_API_NO_PLAYER = 44,    // tp_player named but not online / not found
};

const char *stageName(int stage)
{
    switch (stage) {
        case STAGE_OK: return "ok";
        case STAGE_NO_LEVEL: return "level-handle";
        case STAGE_MANAGER: return "structure-manager";
        case STAGE_GET_STRUCTURE: return "get-structure";
        case STAGE_SAVE: return "template-save";
        case STAGE_WRITE: return "nbt-write";
        case STAGE_PARSE: return "nbt-parse";
        case STAGE_FILE: return "file-write";
        case STAGE_NBT_READ: return "nbt-read-back";
        case STAGE_IMPORT_PARSE: return "import-nbt-parse";
        case STAGE_TEMPLATE_LOAD: return "template-load";
        case STAGE_IMPORT_MISMATCH: return "import-roundtrip-mismatch";
        case STAGE_API_ARG: return "invalid-name-or-argument";
        case STAGE_API_SIZE: return "region-too-large";
        case STAGE_API_DIM: return "unsupported-dimension";
        case STAGE_API_GATE: return "probe-gate-closed";
        case STAGE_API_NOT_FOUND: return "not-found";
        case STAGE_API_TIMEOUT: return "export-retry-budget-exhausted";
        case STAGE_PLACE_AREA: return "place-chunk-load";
        case STAGE_PLACE_LOAD: return "place-structure-load";
        case STAGE_BUNDLE_MANIFEST: return "bundle-manifest";
        case STAGE_BUNDLE_TILE: return "bundle-tile-export";
        case STAGE_API_NO_PLAYER: return "tp-player-unavailable";
        default: return "crashed-or-signal";
    }
}

// ---------------------------------------------------------------------------
// Bundles: a large region sharded into engine-legal tiles, described by a
// manifest so the whole thing can be re-imported and re-placed (including at
// its original position and dimension).
// ---------------------------------------------------------------------------
// (declared here, defined below - the unnamed namespace merges with the
// definition site, so BundleManifest::totalBytes can use them)
namespace {
fs::path pathFromUtf8(const std::string &s);
std::string pathToUtf8(const fs::path &p);
}  // namespace

struct BundleTile {
    std::string file;           // file name inside the .bundle dir
    std::string key;            // engine key, e.g. mystructure:castle__tile_r0c1
    int x1 = 0, y1 = 0, z1 = 0, x2 = 0, y2 = 0, z2 = 0;  // world-space bounds
    bool done = false;
};

struct BundleManifest {
    int format = 1;
    std::string name;           // bare name, e.g. "castle"
    std::string dimension;      // "overworld" | "nether" | "the_end"
    int x1 = 0, y1 = 0, z1 = 0, x2 = 0, y2 = 0, z2 = 0;  // ORIGINAL region
    bool entities = true;
    bool blocks = true;
    bool partial = false;       // true if some tiles failed (can be re-run)
    std::vector<BundleTile> tiles;
    fs::path src_dir;           // RUNTIME-ONLY (not serialized): the .bundle
                                // directory this manifest was read/written
                                // from - library, or a custom API out_dir.

    [[nodiscard]] std::string dirName() const { return name + ".bundle"; }
    [[nodiscard]] std::uintmax_t totalBytes(const fs::path &dir) const
    {
        std::uintmax_t total = 0;
        std::error_code ec;
        for (const auto &t : tiles) {
            const std::uintmax_t sz = fs::file_size(dir / pathFromUtf8(t.file), ec);
            if (!ec && sz != static_cast<std::uintmax_t>(-1)) {
                total += sz;
            }
            ec.clear();
        }
        return total;
    }
};

// One in-flight /structexport request (also used by the API's exportRegion).
struct ExportJob {
    std::string name;
    int x1 = 0, y1 = 0, z1 = 0, x2 = 0, y2 = 0, z2 = 0;
    bool entities = true;
    bool blocks = true;
    bool via_player = false;
    std::string player_name;               // only valid when via_player
    // v0.1: player who may be TELEPORTED to help load chunks. This is the
    // player who initiated the command (or an explicit API parameter) -
    // NEVER a random online player.
    std::string tp_player_name;
    // v0.1: custom output directory (API exportRegionTo). Empty = library.
    fs::path out_dir;
    // v0.1: bundle directory override (out_dir/<name>.bundle). Empty = the
    // canonical library location library/mystructure/<name>.bundle.
    fs::path bundle_dir_override;
    std::vector<std::string> area_names;   // tickingareas we added (cleaned up on finish)
    int attempts = 0;
    // v0.1: bundle export state (null for single-file exports).
    std::shared_ptr<BundleManifest> bundle;
    std::size_t tile_index = 0;
    int tile_attempts = 0;
    // v0.1: tickingarea coverage shortfall (slots exhausted by others or
    // footprint > the 10-area budget) - TP the designated player as backup.
    bool tp_assist = false;
    std::shared_ptr<endstone::Task> task;
    // API callbacks (command-initiated jobs leave these null). py_on_done is a
    // STRONG reference to a Python callable, released after it fires.
    void (*c_on_done)(void *ctx, int stage, const char *message) = nullptr;
    void *c_ctx = nullptr;
    void *py_on_done = nullptr;
};

// One in-flight placement (structure load driven by the API / place command).
struct PlaceJob {
    std::string name;            // structure or bundle name (bare or namespaced)
    int x = 0, y = 0, z = 0;     // target origin (ignored when original)
    bool original = false;       // bundle: use manifest's original position
    std::string dimension;       // "overworld" | "nether" | "the_end" | ""
    bool entities = true;
    bool blocks = true;
    std::string tp_player_name;  // may be teleported to load target chunks
    std::shared_ptr<BundleManifest> bundle;  // null for single structures
    std::size_t tile_index = 0;
    int attempts = 0;
    bool areas_added = false;
    std::vector<std::string> area_names;
    bool tp_assist = false;
    std::shared_ptr<endstone::Task> task;
    void (*c_on_done)(void *ctx, int stage, const char *message) = nullptr;
    void *c_ctx = nullptr;
    void *py_on_done = nullptr;
};

// Strip Minecraft color codes (UTF-8 0xC2 0xA7 + one code char) - API
// callbacks receive plain text, not console-styled strings.
inline std::string stripColorCodes(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i + 2 < s.size() && s[i] == '\xC2' && s[i + 1] == '\xA7') {
            i += 2;  // skip the code character as well
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

class StructBridgePlugin : public endstone::Plugin {
public:
    void onLoad() override;
    void onEnable() override;
    void onDisable() override;

    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                   const std::vector<std::string> &args) override;

private:
    // Command handlers (the unified /struct command dispatches into these;
    // the legacy /structxxx commands alias them for one transition release).
    void handleExport(endstone::CommandSender &sender, const std::vector<std::string> &args);
    void handleImport(endstone::CommandSender &sender, const std::vector<std::string> &args);
    void handleDump(endstone::CommandSender &sender, const std::vector<std::string> &args);
    void handleDelete(endstone::CommandSender &sender, const std::vector<std::string> &args);
    void handleClear(endstone::CommandSender &sender, const std::vector<std::string> &args);
    void handleList(endstone::CommandSender &sender, const std::vector<std::string> &args);
    void handlePlace(endstone::CommandSender &sender, const std::vector<std::string> &args);
    void handleProbe(endstone::CommandSender &sender);
    void handleStatus(endstone::CommandSender &sender, bool rerun_probe);
    void handleApiSelfTest(endstone::CommandSender &sender);

    // ---- Public API core (structured stage results, independent of any
    // CommandSender). Implemented on top of the same verified primitives the
    // command handlers use. ----
    int apiImport(const std::string &name_or_path, std::string &msg);
    int apiDump(const std::string &name, std::string &msg);
    int apiDelete(const char *name_or_null, std::string &msg);  // null = delete all
    std::uint64_t apiExportRegion(const std::string &name, int x1, int y1, int z1, int x2, int y2, int z2,
                                  const char *dimension, bool entities, bool blocks,
                                  void (*on_done)(void *ctx, int stage, const char *message), void *ctx,
                                  void *py_on_done);
    // v2: adds custom output dir, explicit TP player and bundle support.
    std::uint64_t apiExportRegionTo(const std::string &name, int x1, int y1, int z1, int x2, int y2, int z2,
                                    const char *dimension, bool entities, bool blocks, const char *out_dir,
                                    const char *tp_player, bool force_bundle,
                                    void (*on_done)(void *ctx, int stage, const char *message), void *ctx,
                                    void *py_on_done);
    std::uint64_t apiPlaceStructure(const std::string &name, int x, int y, int z, bool original,
                                    const char *dimension, bool entities, bool blocks, const char *tp_player,
                                    void (*on_done)(void *ctx, int stage, const char *message), void *ctx,
                                    void *py_on_done);
#ifdef STRUCTBRIDGE_PYTHON_API
    void registerPythonModule();
#endif

    // Core
    void loadConfig();
    void saveDefaultConfig();
    bool checkPermission(endstone::CommandSender &sender);
    [[nodiscard]] endstone::Player *findPlayer(const std::string &name) const;

    // Structure library: exports dir is the canonical store; a mirrored copy
    // inside the vanilla behavior pack makes names natively visible to
    // /structure load and structure blocks (the engine scans pack structures
    // at level start and loads them lazily - no plugin-side memory cost).
    [[nodiscard]] fs::path libraryDir() const;
    [[nodiscard]] fs::path packStructuresDir() const;
    [[nodiscard]] fs::path behaviorPacksDir() const;  // <server>/behavior_packs
    // <server>/worlds/<level-name>/db - level-name from server.properties
    // (BDS default "Bedrock Level").
    [[nodiscard]] fs::path worldDbDir() const;
    static void writeStructureFile(const fs::path &file, const std::vector<std::uint8_t> &bytes);
    void syncExportsToPack();

    // One structure visible on this server, merged across its sources.
    struct LibItem {
        std::string key;     // e.g. "mystructure:house" (lookup key / engine key)
        std::string local;   // e.g. "house"
        std::string ns;      // e.g. "mystructure"
        fs::path file;       // representative file (library preferred, else pack)
        std::uintmax_t size = 0;
        // v0.0.2 sources - the same name can exist in several at once.
        bool in_library = false;  // plugin exports dir (or its legacy default/ layout)
        bool in_pack = false;     // some behavior pack's structures/ dir (read-only game data)
        bool in_world = false;    // world LevelDB entry (structure block / "save ... disk")
        fs::path mirror;          // the plugin's own pack mirror file, if present (deletable)
    };
    [[nodiscard]] std::vector<LibItem> collectStructures() const;

    // Structure bundles in the library (library/mystructure/<name>.bundle).
    struct BundleEntry {
        std::string name;                       // bare name, e.g. "castle"
        fs::path dir;                           // library/mystructure/castle.bundle
        std::shared_ptr<BundleManifest> manifest;
    };
    [[nodiscard]] std::vector<BundleEntry> collectBundles() const;
    // Bundle lookup by bare name (nullptr when not a bundle).
    [[nodiscard]] std::shared_ptr<BundleManifest> findBundle(const std::string &name) const;
    // Bundle lookup by bare name OR by .bundle directory path (relative to
    // the server root, or absolute) - lets API consumers place bundles they
    // exported to a custom out_dir. Returns nullptr when not found.
    [[nodiscard]] std::shared_ptr<BundleManifest> resolveBundleRef(const std::string &name) const;

    // Native chain helpers (run in both parent and probe child)
    static StructureManager *getManager(endstone::Server &server, int &stage);
    static bool buildMcstructureBytes(StructureTemplate *tpl, std::vector<std::uint8_t> &out, int &stage,
                                      std::vector<std::uint8_t> *raw_out = nullptr,
                                      bool require_format_version = true);
    static int importMcstructureBytes(StructureManager *manager, const std::string &key,
                                      const std::vector<std::uint8_t> &bytes);

    // Export retry machinery
    void addTickingAreasForJob(ExportJob &job);
    void addOneArea(ExportJob &job, int cx1, int cz1, int cx2, int cz2, bool via_player);
    void exportTick(std::shared_ptr<ExportJob> job);
    void finishExport(std::shared_ptr<ExportJob> &job, const std::string &message, int stage = STAGE_OK);
    // TP the job's designated player to (x, z) [y from the job] to bring the
    // surrounding chunks into simulation range. Returns false if the player
    // is gone. The target is ALWAYS job->tp_player_name - never a random one.
    bool teleportJobPlayer(ExportJob &job, int x, int y, int z);
    // Count how many 10x10-chunk ticking areas the job footprint needs.
    [[nodiscard]] static int areasNeededFor(int x1, int z1, int x2, int z2);

    // Bundle machinery
    static std::shared_ptr<BundleManifest> planBundle(const std::string &name, int x1, int y1, int z1, int x2,
                                                      int y2, int z2, const std::string &dimension, bool entities,
                                                      bool blocks);
    static bool writeBundleManifest(const BundleManifest &m, const fs::path &dir);
    [[nodiscard]] static std::shared_ptr<BundleManifest> readBundleManifest(const fs::path &dir);
    [[nodiscard]] fs::path bundleDirInLibrary(const std::string &name) const;  // library/mystructure/<n>.bundle
    void bundleTick(std::shared_ptr<ExportJob> job);
    void startExportJob(std::shared_ptr<ExportJob> job);

    // Placement machinery
    void placeTick(std::shared_ptr<PlaceJob> job);
    void finishPlace(std::shared_ptr<PlaceJob> &job, const std::string &message, int stage = STAGE_OK);
    bool teleportPlacePlayer(PlaceJob &job, int x, int y, int z);
    void startPlaceJob(std::shared_ptr<PlaceJob> job);
    [[nodiscard]] static std::string normalizeDimension(std::string dim);  // "" -> "overworld"

    // Layout migration (pre-0.1.0 -> library layout)
    void migrateLegacyLayout();

    // <server root> derived from the (absolute) plugin data folder.
    [[nodiscard]] fs::path serverRoot() const;

    // Probe
    void startProbeWhenReady();
    void runProbe(bool manual);
#if !defined(_WIN32)
    static int probeChildMain(endstone::Server &server, const Config &config, const fs::path &out_file);
#endif
    bool evaluateWindowsGate();

    Config config_;
    GateState gate_ = GateState::NotProbed;
    int probe_stage_ = -1;
    std::string probe_detail_;
    bool probe_scheduled_ = false;
    int probe_attempts_ = 0;
    std::shared_ptr<endstone::Task> probe_task_;

    std::vector<std::shared_ptr<ExportJob>> export_jobs_;
    std::vector<std::shared_ptr<PlaceJob>> place_jobs_;
    int area_counter_ = 0;
    std::uint64_t api_job_counter_ = 0;
    // /struct clear confirmation window (server-wide, 60 s).
    bool clear_pending_ = false;
    std::chrono::steady_clock::time_point clear_pending_at_;

    static bool isValidStructureName(const std::string &name);
    static fs::path sanitizeRelativePath(const std::string &name);

    friend class ApiImpl;
};

// Public API plumbing (ApiImpl itself is defined below, after the handlers;
// pointers keep the declaration order simple).
class ApiImpl;
ApiImpl *g_api_instance = nullptr;               // created in onLoad, alive for the process
ApiImpl *createApiImpl(StructBridgePlugin *plugin);  // defined next to ApiImpl
constexpr const char *kPluginVersion = "0.1.0";  // twin of the ENDSTONE_PLUGIN version string

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
void StructBridgePlugin::saveDefaultConfig()
{
    auto dir = getDataFolder();
    std::error_code ec;
    fs::create_directories(dir, ec);
    json j;
    j["version"] = config_.version;
    j["library_dir"] = config_.library_dir;
    j["include_entities"] = config_.include_entities;
    j["include_blocks"] = config_.include_blocks;
    j["probe_enabled"] = config_.probe_enabled;
    j["probe_timeout_seconds"] = config_.probe_timeout_seconds;
    j["probe_template_name"] = config_.probe_template_name;
    j["require_op"] = config_.require_op;
    j["windows_allow_without_probe"] = config_.windows_allow_without_probe;
    j["windows_expected_minecraft_version"] = config_.windows_expected_minecraft_version;
    j["diagnostic_logging"] = config_.diagnostic_logging;
    j["export_max_attempts"] = config_.export_max_attempts;
    j["export_retry_interval_ticks"] = config_.export_retry_interval_ticks;
    j["export_area_chunk_limit"] = config_.export_area_chunk_limit;
    j["export_max_areas"] = config_.export_max_areas;
    std::ofstream out(dir / "config.json");
    out << j.dump(2) << std::endl;
}

void StructBridgePlugin::loadConfig()
{
    auto path = getDataFolder() / "config.json";
    if (!fs::exists(path)) {
        saveDefaultConfig();
        return;
    }
    try {
        std::ifstream in(path);
        json j;
        in >> j;
        auto get_bool = [&j](const char *k, bool d) { return j.contains(k) && j[k].is_boolean() ? j[k].get<bool>() : d; };
        auto get_int = [&j](const char *k, int d) {
            return j.contains(k) && j[k].is_number_integer() ? j[k].get<int>() : d;
        };
        auto get_str = [&j](const char *k, const std::string &d) {
            return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : d;
        };
        config_.version = get_int("version", 3);
        config_.library_dir = get_str("library_dir", "library");
        // Legacy "output_dir" (pre-0.1.0) - remembered for the one-time
        // directory migration in onEnable().
        config_.output_dir_legacy = get_str("output_dir", "");
        config_.include_entities = get_bool("include_entities", true);
        config_.include_blocks = get_bool("include_blocks", true);
        config_.probe_enabled = get_bool("probe_enabled", true);
        config_.probe_timeout_seconds = std::clamp(get_int("probe_timeout_seconds", 15), 3, 120);
        config_.probe_template_name = get_str("probe_template_name", "structbridge:__probe__");
        config_.require_op = get_bool("require_op", true);
        config_.windows_allow_without_probe = get_bool("windows_allow_without_probe", false);
        config_.windows_expected_minecraft_version = get_str("windows_expected_minecraft_version", "");
        config_.diagnostic_logging = get_bool("diagnostic_logging", true);
        config_.export_max_attempts = std::clamp(get_int("export_max_attempts", 20), 1, 600);
        config_.export_retry_interval_ticks = std::clamp(get_int("export_retry_interval_ticks", 20), 1, 200);
        config_.export_area_chunk_limit = std::clamp(get_int("export_area_chunk_limit", 100), 1, 100);
        config_.export_max_areas = std::clamp(get_int("export_max_areas", 10), 1, 10);
    }
    catch (const std::exception &e) {
        getLogger().error("Failed to parse config.json ({}), using defaults.", e.what());
    }
}

// ---------------------------------------------------------------------------
// Native chain
// ---------------------------------------------------------------------------
StructureManager *StructBridgePlugin::getManager(endstone::Server &server, int &stage)
{
    auto *elevel = server.getLevel();
    if (elevel == nullptr) {
        stage = STAGE_NO_LEVEL;
        return nullptr;
    }
    auto *endstone_level = static_cast<endstone::core::EndstoneLevel *>(elevel);
    ::Level &level = endstone_level->level_;  // direct member access (self-contained; see header comment)
    auto manager_handle = level.getStructureManager();
    StructureManager *manager = manager_handle.get().access();
    if (manager == nullptr) {
        stage = STAGE_MANAGER;
        return nullptr;
    }
    return manager;
}

bool StructBridgePlugin::buildMcstructureBytes(StructureTemplate *tpl, std::vector<std::uint8_t> &out,
                                               int &stage, std::vector<std::uint8_t> *raw_out,
                                               bool require_format_version)
{
    auto tag = tpl->save();
    if (!tag) {
        stage = STAGE_SAVE;
        return false;
    }
    MemoryDataOutput sink;
    sink.appendRaw(0x0A);       // TAG_Compound
    sink.appendRaw16(0x0000);   // root name: empty (vanilla .mcstructure layout)
    tag->write(sink);           // payload via native vtable dispatch
    auto data = sink.takeData();
    if (raw_out != nullptr) {
        *raw_out = data;  // always expose the raw bytes for diagnosis
    }
    if (data.size() < 4) {
        stage = STAGE_WRITE;
        return false;
    }
    // For real exports the file must contain format_version. The startup probe
    // serializes an EMPTY template (no vanilla command involved - see
    // probeChildMain), whose payload may legitimately lack the key, so there we
    // only require structurally valid NBT.
    if (require_format_version ? !nbt_check::validate(data, "format_version")
                               : !nbt_check::validate(data, "")) {
        stage = STAGE_PARSE;
        return false;
    }
    out = std::move(data);
    return true;
}

// Imports raw .mcstructure bytes into the structure manager under `key`.
// The engine's own NBT parser (virtual Tag::load) consumes the bytes through
// our MemoryDataInput; the plugin never builds NBT by hand. Returns STAGE_OK
// (0) on success or a ProbeStage error code.
int StructBridgePlugin::importMcstructureBytes(StructureManager *manager, const std::string &key,
                                               const std::vector<std::uint8_t> &bytes)
{
    auto &tpl = manager->getOrCreate(key);
    // Any engine-allocated CompoundTag works as the parse target; save() is the
    // only engine CompoundTag factory we can reach without symbols.
    auto tag = tpl.save();
    if (!tag) {
        return STAGE_SAVE;
    }
    MemoryDataInput input(bytes);
    if (!input.skipStructureHeader()) {
        return STAGE_IMPORT_PARSE;
    }
    auto result = tag->load(input);  // BDS's own CompoundTag::load via vtable
    if (!result.ignoreError()) {
        return STAGE_IMPORT_PARSE;
    }
    if (input.numBytesLeft() != 0) {
        return STAGE_IMPORT_PARSE;  // trailing bytes -> not a clean single root
    }
    if (!tpl.vslot_load(*static_cast<CompoundTag *>(tag.get()))) {
        return STAGE_TEMPLATE_LOAD;
    }
    return STAGE_OK;
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------
#if !defined(_WIN32)
int StructBridgePlugin::probeChildMain(endstone::Server &server, const Config &config, const fs::path &out_file)
{
    alarm(static_cast<unsigned>(config.probe_timeout_seconds));  // watchdog: kill child on hang

    int stage = STAGE_OK;
    auto *manager = getManager(server, stage);
    if (manager == nullptr) {
        _exit(stage);
    }

    // A) EXPORT chain: serialize a template (existing one if a previous run
    //    left it, otherwise an empty one from getOrCreate) and write the file.
    auto *tpl = manager->getStructure(config.probe_template_name);
    if (tpl == nullptr) {
        tpl = &manager->getOrCreate(config.probe_template_name);
    }
    std::vector<std::uint8_t> bytes_a;
    if (!buildMcstructureBytes(tpl, bytes_a, stage, nullptr, /*require_format_version=*/false)) {
        _exit(stage);
    }
    {
        std::ofstream f(out_file, std::ios::binary);
        if (!f) {
            _exit(STAGE_FILE);
        }
        f.write(reinterpret_cast<const char *>(bytes_a.data()),
                static_cast<std::streamsize>(bytes_a.size()));
        f.flush();
        if (!f.good()) {
            _exit(STAGE_FILE);
        }
    }

    // B) IMPORT chain: read the file back and drive it through the full
    //    virtual-load route (Tag::load -> StructureTemplate::load), then
    //    re-serialize and require a byte-identical roundtrip.
    std::vector<std::uint8_t> bytes_b;
    {
        std::ifstream in(out_file, std::ios::binary);
        bytes_b.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    if (bytes_b != bytes_a || bytes_b.empty()) {
        _exit(STAGE_NBT_READ);
    }
    {
        auto &tpl2 = manager->getOrCreate(config.probe_template_name + "_import");
        auto tag2 = tpl2.save();
        if (!tag2) {
            _exit(STAGE_SAVE);
        }
        MemoryDataInput input(bytes_b);
        if (!input.skipStructureHeader()) {
            _exit(STAGE_IMPORT_PARSE);
        }
        auto result = tag2->load(input);
        if (!result.ignoreError()) {
            _exit(STAGE_IMPORT_PARSE);
        }
        if (input.numBytesLeft() != 0) {
            _exit(STAGE_IMPORT_PARSE);
        }
        if (!tpl2.vslot_load(*static_cast<CompoundTag *>(tag2.get()))) {
            _exit(STAGE_TEMPLATE_LOAD);
        }
        std::vector<std::uint8_t> bytes_c;
        int stage_c = STAGE_OK;
        if (!buildMcstructureBytes(&tpl2, bytes_c, stage_c, nullptr, false)) {
            _exit(stage_c);
        }
        if (bytes_c != bytes_a) {
            // Dump the roundtrip output so the mismatch can be inspected.
            std::ofstream dbg(out_file.string() + ".roundtrip.bin", std::ios::binary);
            if (dbg) {
                dbg.write(reinterpret_cast<const char *>(bytes_c.data()),
                          static_cast<std::streamsize>(bytes_c.size()));
            }
            _exit(STAGE_IMPORT_MISMATCH);
        }
    }

    _exit(STAGE_OK);
}
#endif

bool StructBridgePlugin::evaluateWindowsGate()
{
#if defined(_WIN32)
    if (!config_.windows_allow_without_probe) {
        probe_detail_ =
            "Windows: fork sandbox unavailable; set windows_allow_without_probe=true in config.json to "
            "enable with header-derived (unverified) vtable indices.";
        return false;
    }
    const std::string expected = config_.windows_expected_minecraft_version;
    if (!expected.empty()) {
        std::string actual = getServer().getMinecraftVersion();
        if (actual.rfind(expected, 0) != 0) {
            probe_detail_ = "Minecraft version mismatch: expected prefix '" + expected + "', got '" + actual + "'.";
            return false;
        }
    }
    probe_detail_ = "Windows: enabled without runtime probe (config override).";
    return true;
#else
    return false;
#endif
}

void StructBridgePlugin::startProbeWhenReady()
{
    if (probe_scheduled_) {
        return;
    }
    probe_scheduled_ = true;
    auto *self = this;
    probe_task_ = getServer().getScheduler().runTaskTimer(
        *this,
        [self]() {
            if (self->gate_ != GateState::NotProbed) {
                if (self->probe_task_) {
                    self->probe_task_->cancel();
                }
                return;
            }
            if (self->getServer().getLevel() == nullptr) {
                if (++self->probe_attempts_ > 120) {
                    self->getLogger().warning("Level not available after 120 attempts; native gate stays closed.");
                    self->probe_task_->cancel();
                }
                return;
            }
            self->probe_task_->cancel();
            self->runProbe(false);
        },
        20, 20);
}

void StructBridgePlugin::runProbe(bool manual)
{
#if !defined(_WIN32)
    (void)manual;
    fs::path out_file = getDataFolder() / "probe.mcstructure";
    std::error_code ec;
    fs::create_directories(getDataFolder(), ec);

    auto &server = getServer();
    if (server.getLevel() == nullptr) {
        probe_stage_ = STAGE_NO_LEVEL;
        probe_detail_ = "level not loaded yet";
        gate_ = GateState::Unavailable;
        return;
    }

    if (!config_.probe_enabled) {
        probe_stage_ = STAGE_OK;
        gate_ = GateState::Unavailable;
        probe_detail_ = "probe disabled in config (probe_enabled=false)";
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        probe_detail_ = "fork() failed";
        gate_ = GateState::Unavailable;
        return;
    }
    if (pid == 0) {
        // Child: crash-isolated sandbox (copy-on-write snapshot of the server).
        probeChildMain(server, config_, out_file);
        _exit(STAGE_MANAGER);  // unreachable
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        probe_stage_ = code;
        if (code == STAGE_OK) {
            gate_ = GateState::Available;
            probe_detail_ =
                "full roundtrip verified (manager -> template save -> NBT write -> engine NBT parse -> "
                "template load -> byte-identical re-serialization)";
            getLogger().info("Native probe passed: structure export/import is ENABLED.");
            auto sz = fs::file_size(out_file, ec);
            if (!ec && config_.diagnostic_logging) {
                getLogger().info("Probe artifact: {} bytes written to {}", sz, out_file.string());
            }
        }
        else {
            gate_ = GateState::Unavailable;
            probe_detail_ = std::string("probe failed at stage '") + stageName(code) + "'";
            getLogger().warning(
                "Native probe FAILED at stage '{}' (code {}). Structure export/import stays DISABLED - "
                "this server build is not adapted yet. All /struct* commands will refuse safely.",
                stageName(code), code);
            // Hex-dump the head of the roundtrip output (written by the child on
            // import-roundtrip mismatches) so the actual bytes can be inspected.
            const fs::path dbg = out_file.string() + ".roundtrip.bin";
            std::error_code ec2;
            auto dbg_size = fs::file_size(dbg, ec2);
            if (!ec2 && dbg_size > 0) {
                std::ifstream in(dbg, std::ios::binary);
                std::size_t want = static_cast<std::size_t>(dbg_size);
                if (want > 96) {
                    want = 96;
                }
                std::vector<char> head(want);
                in.read(head.data(), static_cast<std::streamsize>(want));
                const auto got = static_cast<std::size_t>(in.gcount());
                std::string hex;
                hex.reserve(got * 3);
                char buf[4];
                for (std::size_t i = 0; i < got; ++i) {
                    std::snprintf(buf, sizeof(buf), "%02X ",
                                  static_cast<unsigned char>(head[i]));
                    hex += buf;
                }
                getLogger().warning("Roundtrip head ({} of {} bytes): {}", got, dbg_size, hex);
            }
        }
        return;
    }
    if (WIFSIGNALED(status)) {
        probe_stage_ = 128 + WTERMSIG(status);
        gate_ = GateState::Unavailable;
        probe_detail_ = std::string("probe child killed by signal ") + std::to_string(WTERMSIG(status));
        getLogger().warning("Native probe child crashed (signal {}): server is unaffected; feature DISABLED.",
                            WTERMSIG(status));
    }
#else
    probe_stage_ = 0;
    if (evaluateWindowsGate()) {
        gate_ = GateState::Available;
        getLogger().info("Windows native gate opened via config override: {}", probe_detail_);
    }
    else {
        gate_ = GateState::Unavailable;
        getLogger().warning("Windows native gate stays closed: {}", probe_detail_);
    }
#endif
}

// ---------------------------------------------------------------------------
// Name / path helpers
// ---------------------------------------------------------------------------
// fs::path <-> UTF-8 std::string.
//
// On POSIX the narrow encoding is UTF-8 on any realistic host, so paths pass
// through unchanged. On Windows, fs::path's NARROW conversions go through the
// ANSI code page, which mangles Chinese structure names - so build/convert
// paths through the wide API there instead.
namespace {
fs::path pathFromUtf8(const std::string &s)
{
#if defined(_WIN32)
    if (s.empty()) {
        return fs::path();
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) {
        return fs::path(s);  // not valid UTF-8 - fall back to the old behavior
    }
    std::wstring w(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), need);
    return fs::path(w);
#else
    return fs::path(s);
#endif
}

std::string pathToUtf8(const fs::path &p)
{
#if defined(_WIN32)
    const std::wstring w = p.native();
    if (w.empty()) {
        return {};
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return p.string();
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), need, nullptr, nullptr);
    return out;
#else
    return p.string();
#endif
}
}  // namespace

bool StructBridgePlugin::isValidStructureName(const std::string &name)
{
    // The ENGINE has no restriction here (LevelDB stores keys verbatim;
    // structure blocks and /structure load accept any UTF-8 name - verified
    // in the sandbox with mystructure:中文屋2). v0.0.1 wrongly rejected every
    // non-ASCII byte, which made Chinese names unusable. Now well-formed
    // UTF-8 is accepted; still rejected are control characters, spaces,
    // quotes and path separators (they break the vanilla command line or the
    // on-disk layout) plus a second namespace colon.
    if (name.empty() || name.size() > 128) {
        return false;
    }
    bool has_colon = false;
    std::size_t i = 0;
    while (i < name.size()) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x80) {
            if (c == ':') {
                if (has_colon) {
                    return false;
                }
                has_colon = true;
            }
            else if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) {
                return false;
            }
            ++i;
            continue;
        }
        // Multibyte UTF-8: validate the whole sequence, then accept verbatim.
        unsigned cp = 0;
        int len;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        }
        else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        }
        else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        }
        else {
            return false;  // stray continuation byte or 0xF8+
        }
        if (i + static_cast<std::size_t>(len) > name.size()) {
            return false;  // truncated sequence
        }
        for (int k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(name[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) {
            return false;  // overlong encoding
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return false;  // surrogate half
        }
        if (cp > 0x10FFFF) {
            return false;
        }
        i += static_cast<std::size_t>(len);
    }
    return true;
}

fs::path StructBridgePlugin::sanitizeRelativePath(const std::string &name)
{
    std::string ns = "mystructure";
    std::string local = name;
    auto pos = name.find(':');
    if (pos != std::string::npos) {
        ns = name.substr(0, pos);
        local = name.substr(pos + 1);
    }
    // Keep the ASCII allow-list (lowercased) AND valid UTF-8 multibyte
    // sequences verbatim (Chinese names!); drop everything else. Only ASCII
    // letters are lowercased - std::tolower on multibyte lead/continuation
    // bytes would corrupt them, and the engine does not fold case anyway.
    auto clean = [](const std::string &in) {
        std::string out;
        out.reserve(in.size());
        std::size_t i = 0;
        while (i < in.size()) {
            const auto c = static_cast<unsigned char>(in[i]);
            if (c < 0x80) {
                if (std::isalnum(c) || c == '_' || c == '-' || c == '.') {
                    out += static_cast<char>(std::tolower(c));
                }
                ++i;
                continue;
            }
            int len;
            if ((c & 0xE0) == 0xC0) {
                len = 2;
            }
            else if ((c & 0xF0) == 0xE0) {
                len = 3;
            }
            else if ((c & 0xF8) == 0xF0) {
                len = 4;
            }
            else {
                ++i;
                continue;  // stray byte - drop it
            }
            bool valid = i + static_cast<std::size_t>(len) <= in.size();
            for (int k = 1; valid && k < len; ++k) {
                valid = (static_cast<unsigned char>(in[i + static_cast<std::size_t>(k)]) & 0xC0) == 0x80;
            }
            if (!valid) {
                ++i;
                continue;
            }
            out.append(in, i, static_cast<std::size_t>(len));
            i += static_cast<std::size_t>(len);
        }
        return out;
    };
    std::string safe_ns = clean(ns);
    std::string safe_local = clean(local);
    if (safe_ns.empty()) {
        safe_ns = "mystructure";
    }
    if (safe_local.empty()) {
        safe_local = "structure";
    }
    return pathFromUtf8(safe_ns) / pathFromUtf8(safe_local + ".mcstructure");
}

// ---------------------------------------------------------------------------
// Structure library helpers
// ---------------------------------------------------------------------------
fs::path StructBridgePlugin::libraryDir() const
{
    return getDataFolder() / config_.library_dir;
}

fs::path StructBridgePlugin::packStructuresDir() const
{
    // getDataFolder() is <server>/plugins/structbridge, so the bedrock_server
    // root is two levels up.
    return getDataFolder().parent_path().parent_path() / "behavior_packs" / "vanilla" / "structures";
}

fs::path StructBridgePlugin::behaviorPacksDir() const
{
    return getDataFolder().parent_path().parent_path() / "behavior_packs";
}

fs::path StructBridgePlugin::worldDbDir() const
{
    // World folder comes from server.properties' level-name (default
    // "Bedrock Level"); the LevelDB lives at worlds/<level-name>/db.
    const fs::path root = getDataFolder().parent_path().parent_path();
    std::string level_name = "Bedrock Level";
    std::ifstream props(root / "server.properties");
    std::string line;
    while (std::getline(props, line)) {
        if (line.rfind("level-name=", 0) != 0) {
            continue;
        }
        level_name = line.substr(sizeof("level-name=") - 1);
        while (!level_name.empty() && (level_name.back() == '\r' || level_name.back() == ' ')) {
            level_name.pop_back();
        }
        if (level_name.size() >= 2 && level_name.front() == '"' && level_name.back() == '"') {
            level_name = level_name.substr(1, level_name.size() - 2);
        }
        break;
    }
    return root / "worlds" / pathFromUtf8(level_name) / "db";
}

void StructBridgePlugin::writeStructureFile(const fs::path &file, const std::vector<std::uint8_t> &bytes)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) {
        return;
    }
    f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void StructBridgePlugin::syncExportsToPack()
{
    const auto root = libraryDir();
    std::error_code ec;
    fs::create_directories(root, ec);
    int synced = 0;
    for (auto &entry : fs::recursive_directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        std::error_code ec2;
        if (!entry.is_regular_file(ec2) || entry.path().extension() != ".mcstructure") {
            continue;
        }
        const auto rel = fs::relative(entry.path(), root, ec2);
        if (ec2 || rel.empty()) {
            continue;
        }
        // First path component = namespace folder (bare names live under
        // mystructure/; the legacy layout used default/ - map it). Deeper
        // components keep their sub-path.
        std::string ns = "mystructure";
        fs::path rest;
        bool first = true;
        for (const auto &part : rel) {
            if (first) {
                ns = pathToUtf8(part);
                first = false;
            }
            else {
                rest /= part;
            }
        }
        if (rel.parent_path().empty()) {
            // file directly in the library root: treat as bare name
            ns = "mystructure";
            rest = rel;
        }
        if (ns == "default") {
            ns = "mystructure";
        }
        // Bundle tiles live at mystructure/<name>.bundle/tile_rXcY.mcstructure
        // but their ENGINE key is mystructure:<name>__tile_rXcY - mirror them
        // at the flat key-compatible name so the engine's own pack scan can
        // lazily load them after a restart.
        bool in_bundle = false;
        std::string bundle_name;
        for (const auto &part : rel) {
            const std::string comp = pathToUtf8(part);
            if (comp.size() > 7 && comp.substr(comp.size() - 7) == ".bundle") {
                in_bundle = true;
                bundle_name = comp.substr(0, comp.size() - 7);
            }
        }
        fs::path flat_rest = rest;
        if (in_bundle) {
            flat_rest = pathFromUtf8(bundle_name + "__" + pathToUtf8(rest.filename()));
        }
        const auto dest = packStructuresDir() / pathFromUtf8(ns) / flat_rest;
        fs::create_directories(dest.parent_path(), ec2);
        std::error_code copy_ec;
        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, copy_ec);
        if (!copy_ec) {
            ++synced;
        }
    }
    if (synced > 0) {
        getLogger().info(
            "Synced {} structure file(s) into behavior_packs/vanilla/structures - names stay available to "
            "/structure load and structure blocks even without this plugin.",
            synced);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void StructBridgePlugin::onLoad()
{
    loadConfig();
    migrateLegacyLayout();
    // The API singleton must exist before ANY consumer asks for it (Python
    // plugins import the module during their own onLoad/onEnable, which may
    // run right after this one; C++ consumers dlsym at any time).
    g_api_instance = createApiImpl(this);
#ifdef STRUCTBRIDGE_PYTHON_API
    registerPythonModule();
#endif
    getLogger().info("StructBridge {} loaded (built against Endstone {}). Public API v{} is open.",
                     kPluginVersion, ENDSTONE_VERSION, STRUCTBRIDGE_API_VERSION);
}

void StructBridgePlugin::onEnable()
{
    // Create the structure library upfront so users can drop .mcstructure
    // files in before running any command, then mirror it into the vanilla
    // behavior pack (the engine scans that at level start).
    std::error_code ec;
    fs::create_directories(libraryDir() / "mystructure", ec);
    syncExportsToPack();
#if !defined(_WIN32)
    startProbeWhenReady();
#else
    runProbe(false);
#endif
}

void StructBridgePlugin::onDisable()
{
    if (probe_task_) {
        probe_task_->cancel();
        probe_task_ = nullptr;
    }
    // Cancel pending exports and clean up their tickingareas.
    for (auto &job : export_jobs_) {
        if (job->task) {
            job->task->cancel();
            job->task = nullptr;
        }
        for (const auto &area : job->area_names) {
            getServer().dispatchCommand(getServer().getCommandSender(), "tickingarea remove " + area);
        }
        // Release any pending Python callback reference (the callback never
        // fires for a cancelled job).
#ifdef STRUCTBRIDGE_PYTHON_API
        if (job->py_on_done != nullptr) {
            PyGILState_STATE gil = PyGILState_Ensure();
            Py_DECREF(reinterpret_cast<PyObject *>(job->py_on_done));
            PyGILState_Release(gil);
            job->py_on_done = nullptr;
        }
#endif
    }
    export_jobs_.clear();
}

bool StructBridgePlugin::checkPermission(endstone::CommandSender &sender)
{
    if (!config_.require_op) {
        return true;
    }
    // Console always allowed; players must be OPs.
    auto *player = sender.asPlayer();
    if (player == nullptr) {
        return true;
    }
    if (player->isOp()) {
        return true;
    }
    sender.sendErrorMessage("此命令仅限管理员 (OP) 使用。");
    return false;
}

endstone::Player *StructBridgePlugin::findPlayer(const std::string &name) const
{
    for (auto *p : getServer().getOnlinePlayers()) {
        if (p != nullptr && p->getName() == name) {
            return p;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Command dispatch: ONE unified /struct command whose first argument selects
// the subcommand (an engine-side enum, so tab completion works and invalid
// subcommands are rejected by the command parser itself). The old /structxxx
// commands stay registered as transition aliases for one release and map to
// the same handlers with the action word stripped.
// ---------------------------------------------------------------------------
namespace {
void sendStructUsage(endstone::CommandSender &sender)
{
    sender.sendMessage("§eStructBridge 用法:");
    sender.sendMessage("§e /struct export <名称> <x1> <y1> <z1> <x2> <y2> <z2> [entities] [blocks]");
    sender.sendMessage("§7   导出区域 (超过 64x64 自动分片为结构束 .bundle)");
    sender.sendMessage("§e /struct import <文件或名称>");
    sender.sendMessage("§7   导入 .mcstructure 或 .bundle 结构束");
    sender.sendMessage("§e /struct place <名称> <x> <y> <z> [entities] [blocks]");
    sender.sendMessage("§e /struct place <名称> original [entities] [blocks]");
    sender.sendMessage("§7   放置结构; original = 结构束按导出时的原坐标与维度复原");
    sender.sendMessage("§e /struct dump <名称>  (导出已保存的结构)");
    sender.sendMessage("§e /struct list [all|library|world|pack]");
    sender.sendMessage("§7   列出结构 (默认只显示插件管理的; all=全部来源)");
    sender.sendMessage("§e /struct delete <名称>");
    sender.sendMessage("§e /struct clear  (删除全部插件管理的结构, 需二次确认)");
    sender.sendMessage("§e /struct status | /struct probe | /struct api");
}

// The unified /struct command receives [action: enum][args: message] - the
// message tail arrives as ONE string and must be re-split for the handlers.
// Quote-aware so quoted tokens (paths with spaces) survive as single args.
std::vector<std::string> splitSubArgs(const std::string &msg)
{
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (const char c : msg) {
        if (c == '"') {
            in_quotes = !in_quotes;
            cur += c;  // handlers strip quotes from the positions they read
        }
        else if (!in_quotes && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        }
        else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}
}  // namespace

bool StructBridgePlugin::onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                                   const std::vector<std::string> &args)
{
    const auto &name = command.getName();
    std::string action;
    std::vector<std::string> rest;
    if (name == "struct") {
        if (args.empty()) {
            sendStructUsage(sender);
            return true;
        }
        action = args[0];
        if (args.size() > 1) {
            rest = splitSubArgs(args[1]);
        }
    }
    else {
        // Legacy /structxxx aliases -> subcommand (their args carry no action
        // word). Deprecated; will be removed in the next release.
        static const std::map<std::string, std::string> legacy = {
            {"structexport", "export"}, {"structimport", "import"},   {"structdump", "dump"},
            {"structdelete", "delete"}, {"structlist", "list"},       {"structprobe", "probe"},
            {"structstatus", "status"}, {"structapi", "api"},
        };
        const auto it = legacy.find(name);
        if (it == legacy.end()) {
            return false;
        }
        action = it->second;
        rest = args;
        sender.sendMessage("§7/{} 已弃用，将在下个版本移除。请改用 §e/struct {}§7。", name, action);
    }

    if (action == "export") {
        handleExport(sender, rest);
    }
    else if (action == "import") {
        handleImport(sender, rest);
    }
    else if (action == "place") {
        handlePlace(sender, rest);
    }
    else if (action == "dump") {
        handleDump(sender, rest);
    }
    else if (action == "list") {
        handleList(sender, rest);
    }
    else if (action == "delete") {
        handleDelete(sender, rest);
    }
    else if (action == "clear") {
        handleClear(sender, rest);
    }
    else if (action == "status") {
        handleStatus(sender, false);
    }
    else if (action == "probe") {
        handleProbe(sender);
    }
    else if (action == "api") {
        handleApiSelfTest(sender);
    }
    else {
        sendStructUsage(sender);
    }
    return true;
}

// ---------------------------------------------------------------------------
// /struct export - chunk loading + retry machinery
// ---------------------------------------------------------------------------
int StructBridgePlugin::areasNeededFor(int x1, int z1, int x2, int z2)
{
    const int cx1 = x1 >> 4, cz1 = z1 >> 4, cx2 = x2 >> 4, cz2 = z2 >> 4;
    if (cx2 < cx1 || cz2 < cz1) {
        return 0;
    }
    const auto chunks = static_cast<long long>(cx2 - cx1 + 1) * static_cast<long long>(cz2 - cz1 + 1);
    if (chunks <= 100) {
        return 1;  // a single area suffices (BDS: <=100 chunks per area)
    }
    // 10x10-chunk tiles, row-major.
    int rows = (cx2 - cx1 + 1 + 9) / 10;
    int cols = (cz2 - cz1 + 1 + 9) / 10;
    return rows * cols;
}

void StructBridgePlugin::addOneArea(ExportJob &job, int cx1, int cz1, int cx2, int cz2, bool via_player)
{
    const std::string area_name = "sbexp" + std::to_string(++area_counter_);
    // BDS 26.5x rect form: full 3-D coords (y is ignored) + optional name.
    const std::string cmd = "tickingarea add " + std::to_string(cx1 * 16) + " 64 " + std::to_string(cz1 * 16) +
                            " " + std::to_string(cx2 * 16 + 15) + " 64 " + std::to_string(cz2 * 16 + 15) + " " +
                            area_name;
    // Dispatch through the PLAYER when the job is dimensioned: the console
    // origin is bound to the overworld, so console-dispatched areas would
    // land in the wrong dimension for nether/end exports.
    endstone::CommandSender *origin = &getServer().getCommandSender();
    if (via_player) {
        if (auto *player = findPlayer(job.player_name)) {
            origin = player;
        }
    }
    if (getServer().dispatchCommand(*origin, cmd)) {
        job.area_names.push_back(area_name);
    }
    else if (config_.diagnostic_logging) {
        getLogger().warning("Could not add ticking area '{}' (all 10 area slots may be in use).", area_name);
    }
}

void StructBridgePlugin::addTickingAreasForJob(ExportJob &job)
{
    const int cx1 = job.x1 >> 4, cz1 = job.z1 >> 4;
    const int cx2 = job.x2 >> 4, cz2 = job.z2 >> 4;
    const auto chunks =
        static_cast<long long>(cx2 - cx1 + 1) * static_cast<long long>(cz2 - cz1 + 1);
    if (chunks <= 0) {
        return;
    }
    if (chunks <= config_.export_area_chunk_limit) {
        addOneArea(job, cx1, cz1, cx2, cz2, job.via_player);
        return;
    }
    // Tile the footprint with areas of at most 10x10 chunks (100 chunks each),
    // bounded by the server-wide area limit.
    const int limit = config_.export_max_areas;
    int added = 0;
    for (int cx = cx1; cx <= cx2 && added < limit; cx += 10) {
        for (int cz = cz1; cz <= cz2 && added < limit; cz += 10) {
            const int ex = std::min(cx + 9, cx2);
            const int ez = std::min(cz + 9, cz2);
            addOneArea(job, cx, cz, ex, ez, job.via_player);
            ++added;
        }
    }
    const int needed = areasNeededFor(job.x1, job.z1, job.x2, job.z2);
    if (added < needed) {
        // Coverage shortfall: the remaining chunks can only come from a
        // player's simulation distance. Mark for TP assist; exportTick
        // decides whether that is possible at all.
        job.tp_assist = true;
        if (config_.diagnostic_logging) {
            getLogger().warning("Tickingarea budget: {}/{} tiles covered for '{}' - TP assist required.",
                                added, needed, job.name);
        }
    }
}

bool StructBridgePlugin::teleportJobPlayer(ExportJob &job, int x, int y, int z)
{
    // NEVER pick a random player: only the designated initiator (command
    // sender or explicit API tp_player) may be moved.
    if (job.tp_player_name.empty()) {
        return false;
    }
    auto *player = findPlayer(job.tp_player_name);
    if (player == nullptr) {
        return false;
    }
    auto &level = *getServer().getLevel();
    if (job.via_player) {
        // keep the job's dimension (the save origin's dimension)
        if (auto *origin = findPlayer(job.player_name)) {
            endstone::Location loc(origin->getLocation().getDimension(),
                                   static_cast<float>(x) + 0.5F, static_cast<float>(y),
                                   static_cast<float>(z) + 0.5F);
            return player->teleport(loc);
        }
    }
    endstone::Location loc(*level.getDimension("overworld"), static_cast<float>(x) + 0.5F,
                           static_cast<float>(y), static_cast<float>(z) + 0.5F);
    return player->teleport(loc);
}

void StructBridgePlugin::finishExport(std::shared_ptr<ExportJob> &job, const std::string &message, int stage)
{
    if (job->task) {
        job->task->cancel();
        job->task = nullptr;
    }
    for (const auto &area : job->area_names) {
        getServer().dispatchCommand(getServer().getCommandSender(), "tickingarea remove " + area);
    }
    if (job->via_player) {
        if (auto *player = findPlayer(job->player_name)) {
            player->sendMessage("{}", message);
        }
        else {
            getLogger().info("Export '{}' finished (player left): {}", job->name, message);
        }
    }
    else if (job->c_on_done == nullptr && job->py_on_done == nullptr) {
        // Command-initiated job: report through the console sender.
        getServer().getCommandSender().sendMessage("{}", message);
        getLogger().info("Export '{}': {}", job->name, message);
    }
    else {
        getLogger().info("Export '{}': {}", job->name, message);
    }

    // API callbacks (C first, then Python). Both fire exactly once, on this
    // (server) thread, with plain text. The Python callable's strong
    // reference is released after the call.
    if (job->c_on_done != nullptr) {
        const std::string plain = stripColorCodes(message);
        job->c_on_done(job->c_ctx, stage, plain.c_str());
    }
#ifdef STRUCTBRIDGE_PYTHON_API
    if (job->py_on_done != nullptr) {
        PyObject *fn = reinterpret_cast<PyObject *>(job->py_on_done);
        job->py_on_done = nullptr;  // release ownership even if the call raises
        const std::string plain = stripColorCodes(message);
        PyGILState_STATE gil = PyGILState_Ensure();
        PyObject *cb_args = Py_BuildValue("(is)", stage, plain.c_str());
        if (cb_args != nullptr) {
            PyObject *result = PyObject_CallObject(fn, cb_args);
            Py_XDECREF(result);
            Py_DECREF(cb_args);
        }
        if (PyErr_Occurred() != nullptr) {
            PyErr_Print();  // never propagate a Python exception into the C++ tick
        }
        Py_DECREF(fn);  // MUST stay inside the GIL scope: dealloc may run Python code
        PyGILState_Release(gil);
    }
#endif

    export_jobs_.erase(std::remove_if(export_jobs_.begin(), export_jobs_.end(),
                                      [&job](const std::shared_ptr<ExportJob> &j) { return j.get() == job.get(); }),
                       export_jobs_.end());
}

void StructBridgePlugin::exportTick(std::shared_ptr<ExportJob> job)
{
    auto &server = getServer();
    job->attempts++;

    // Resolve the origin fresh on every attempt (players may disconnect).
    endstone::CommandSender *origin = &server.getCommandSender();
    if (job->via_player) {
        auto *player = findPlayer(job->player_name);
        if (player == nullptr) {
            finishExport(job, "§c导出中止: 玩家 " + job->player_name + " 已离线。", STAGE_API_ARG);
            return;
        }
        origin = player;
    }

    // TP assist: the tickingarea budget could not cover the whole footprint
    // (or area slots are taken). Move the DESIGNATED player (the initiator,
    // never a random one) to the region center so their simulation distance
    // patches the hole. Retry every other attempt to give chunks time.
    if (job->tp_assist && (job->attempts % 2) == 1) {
        const int cx = (job->x1 + job->x2) / 2;
        const int cz = (job->z1 + job->z2) / 2;
        if (!teleportJobPlayer(*job, cx, job->y1, cz) && job->attempts >= config_.export_max_attempts) {
            finishExport(job, "§c导出失败: 常加载区块预算不足，且无可传送的发起玩家 (仅允许传送发起者，绝不随机选择)。",
                         STAGE_PLACE_AREA);
            return;
        }
    }

    const std::string cmd = "structure save " + job->name + " " + std::to_string(job->x1) + " " +
                            std::to_string(job->y1) + " " + std::to_string(job->z1) + " " +
                            std::to_string(job->x2) + " " + std::to_string(job->y2) + " " +
                            std::to_string(job->z2) + (job->entities ? " true" : " false") + " memory " +
                            (job->blocks ? "true" : "false");
    const bool ok = server.dispatchCommand(*origin, cmd);

    if (ok) {
        int stage = STAGE_OK;
        auto *manager = getManager(server, stage);
        if (manager != nullptr) {
            StructureTemplate *tpl = manager->getStructure(job->name);
            if (tpl == nullptr && job->name.find(':') == std::string::npos) {
                // Bare names are stored by the engine under the mystructure:
                // namespace ("Saved a structure with name mystructure:<name>").
                tpl = manager->getStructure("mystructure:" + job->name);
            }
            if (tpl != nullptr) {
                std::vector<std::uint8_t> bytes;
                int bstage = STAGE_OK;
                if (buildMcstructureBytes(tpl, bytes, bstage)) {
                    // Custom output dir (API exportRegionTo): write ONLY there.
                    // Default: the plugin library + behavior-pack mirror.
                    const fs::path out_file = job->out_dir.empty()
                                                  ? libraryDir() / sanitizeRelativePath(job->name)
                                                  : job->out_dir / sanitizeRelativePath(job->name);
                    std::error_code ec;
                    fs::create_directories(out_file.parent_path(), ec);
                    std::ofstream f(out_file, std::ios::binary);
                    if (f) {
                        f.write(reinterpret_cast<const char *>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size()));
                        f.close();
                        if (f.good()) {
                            if (job->out_dir.empty()) {
                                // Mirror into the vanilla behavior pack so the name
                                // stays usable after a restart without any plugin
                                // memory pre-registration.
                                writeStructureFile(packStructuresDir() / sanitizeRelativePath(job->name), bytes);
                            }
                            getLogger().info("Exported '{}' ({} bytes) to {}", job->name, bytes.size(),
                                             pathToUtf8(out_file));
                            finishExport(job, "§a导出成功: " + pathToUtf8(out_file) + " (" +
                                                  std::to_string(bytes.size()) + " 字节, 区域 " +
                                                  std::to_string(job->x1) + " " + std::to_string(job->y1) + " " +
                                                  std::to_string(job->z1) + " -> " + std::to_string(job->x2) + " " +
                                                  std::to_string(job->y2) + " " + std::to_string(job->z2) + ")");
                            return;
                        }
                    }
                    finishExport(job, "§c无法写入文件: " + pathToUtf8(out_file), STAGE_FILE);
                    return;
                }
                finishExport(job, std::string("§c序列化失败 (阶段: ") + stageName(bstage) + ")，导出中止。", bstage);
                return;
            }
        }
    }

    if (job->attempts >= config_.export_max_attempts) {
        std::string extra;
        if (job->tp_assist) {
            extra = " 本次区域超出了常加载区块预算 (10 个区域 × 100 区块)，"
                    "且传送辅助未能补足。建议拆分区域，或由在线玩家在目标维度内发起导出。";
        }
        else {
            extra = " 常见原因: 目标区域区块未能加载 (控制台导出且无人在线)、或服务器日志中有命令报错。";
        }
        finishExport(job, "§c导出失败: 原版 structure save 命令在 " + std::to_string(job->attempts) +
                              " 次尝试内未生效。" + extra, STAGE_API_TIMEOUT);
    }
}

// ---------------------------------------------------------------------------
// Bundle machinery: shard a too-large region into engine-legal tiles.
// The engine's per-template limit on current servers is (64, 384, 64) - the
// E2E run against BDS 1.26.52 confirmed it: "A structure's size cannot be
// larger than (64, 384, 64)". So tiles are 64x64 footprints (Y needs no
// tiling: the world itself is 384 tall and taller selections are rejected
// earlier), each tile loads with a single tickingarea.
// ---------------------------------------------------------------------------
std::shared_ptr<BundleManifest> StructBridgePlugin::planBundle(const std::string &name, int x1, int y1, int z1,
                                                               int x2, int y2, int z2,
                                                               const std::string &dimension, bool entities,
                                                               bool blocks)
{
    auto m = std::make_shared<BundleManifest>();
    m->name = name;
    m->dimension = dimension;
    m->x1 = x1;
    m->y1 = y1;
    m->z1 = z1;
    m->x2 = x2;
    m->y2 = y2;
    m->z2 = z2;
    m->entities = entities;
    m->blocks = blocks;
    // Tile edge: 64 blocks - the ENGINE's per-template size limit is
    // (64, 384, 64), verified against BDS 1.26.52. A 64x64 footprint is 16
    // chunks = a single tickingarea per tile.
    constexpr int TILE = 64;
    int row = 0;
    for (int tx = x1; tx <= x2; tx += TILE, ++row) {
        int col = 0;
        for (int tz = z1; tz <= z2; tz += TILE, ++col) {
            BundleTile t;
            t.file = "tile_r" + std::to_string(row) + "c" + std::to_string(col) + ".mcstructure";
            t.key = "mystructure:" + name + "__tile_r" + std::to_string(row) + "c" + std::to_string(col);
            t.x1 = tx;
            t.y1 = y1;
            t.z1 = tz;
            t.x2 = std::min(tx + TILE - 1, x2);
            t.y2 = y2;
            t.z2 = std::min(tz + TILE - 1, z2);
            m->tiles.push_back(std::move(t));
        }
    }
    return m;
}

bool StructBridgePlugin::writeBundleManifest(const BundleManifest &m, const fs::path &dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    json j;
    j["format"] = m.format;
    j["name"] = m.name;
    j["dimension"] = m.dimension;
    j["region"] = {m.x1, m.y1, m.z1, m.x2, m.y2, m.z2};
    j["entities"] = m.entities;
    j["blocks"] = m.blocks;
    j["partial"] = m.partial;
    j["tiles"] = json::array();
    for (const auto &t : m.tiles) {
        j["tiles"].push_back({
            {"file", t.file},
            {"key", t.key},
            {"bounds", {t.x1, t.y1, t.z1, t.x2, t.y2, t.z2}},
            {"done", t.done},
        });
    }
    std::ofstream out(dir / "manifest.json");
    out << j.dump(2) << std::endl;
    return out.good();
}

std::shared_ptr<BundleManifest> StructBridgePlugin::readBundleManifest(const fs::path &dir)
{
    std::error_code ec;
    if (!fs::exists(dir / "manifest.json", ec)) {
        return nullptr;
    }
    try {
        std::ifstream in(dir / "manifest.json");
        json j;
        in >> j;
        auto m = std::make_shared<BundleManifest>();
        m->format = j.value("format", 1);
        m->name = j.value("name", std::string());
        m->dimension = j.value("dimension", std::string("overworld"));
        if (j.contains("region") && j["region"].is_array() && j["region"].size() == 6) {
            m->x1 = j["region"][0];
            m->y1 = j["region"][1];
            m->z1 = j["region"][2];
            m->x2 = j["region"][3];
            m->y2 = j["region"][4];
            m->z2 = j["region"][5];
        }
        m->entities = j.value("entities", true);
        m->blocks = j.value("blocks", true);
        m->partial = j.value("partial", false);
        m->src_dir = dir;
        if (j.contains("tiles") && j["tiles"].is_array()) {
            for (const auto &t : j["tiles"]) {
                BundleTile tile;
                tile.file = t.value("file", std::string());
                tile.key = t.value("key", std::string());
                if (t.contains("bounds") && t["bounds"].is_array() && t["bounds"].size() == 6) {
                    tile.x1 = t["bounds"][0];
                    tile.y1 = t["bounds"][1];
                    tile.z1 = t["bounds"][2];
                    tile.x2 = t["bounds"][3];
                    tile.y2 = t["bounds"][4];
                    tile.z2 = t["bounds"][5];
                }
                tile.done = t.value("done", false);
                if (!tile.file.empty() && !tile.key.empty()) {
                    m->tiles.push_back(std::move(tile));
                }
            }
        }
        if (m->name.empty() || m->tiles.empty()) {
            return nullptr;
        }
        return m;
    }
    catch (const std::exception &) {
        return nullptr;
    }
}

fs::path StructBridgePlugin::bundleDirInLibrary(const std::string &name) const
{
    return libraryDir() / "mystructure" / pathFromUtf8(name + ".bundle");
}

void StructBridgePlugin::startExportJob(std::shared_ptr<ExportJob> job)
{
    addTickingAreasForJob(*job);
    export_jobs_.push_back(job);
    auto *self = this;
    std::weak_ptr<ExportJob> weak = job;
    job->task = getServer().getScheduler().runTaskTimer(
        *this,
        [self, weak]() {
            if (auto locked = weak.lock()) {
                if (locked->bundle) {
                    self->bundleTick(locked);
                }
                else {
                    self->exportTick(locked);
                }
            }
        },
        1, static_cast<std::uint64_t>(std::max(1, config_.export_retry_interval_ticks)));
}

void StructBridgePlugin::bundleTick(std::shared_ptr<ExportJob> job)
{
    auto &m = *job->bundle;
    // Bundle destination: the canonical library dir unless the API caller
    // redirected it (exportRegionTo out_dir).
    const fs::path bundle_dir =
        job->bundle_dir_override.empty() ? bundleDirInLibrary(m.name) : job->bundle_dir_override;
    if (job->tile_index >= m.tiles.size()) {
        // All tiles done - finalize the manifest and mirror to the pack.
        const fs::path lib_dir = bundle_dir;
        m.partial = false;
        writeBundleManifest(m, lib_dir);
        // Mirror every tile at its flat ENGINE-KEY-compatible name
        // (mystructure/<name>__tile_rXcY.mcstructure == key
        // mystructure:<name>__tile_rXcY), so post-restart "structure load"
        // finds them through the engine's own pack scan. SKIPPED for bundles
        // exported to a custom out_dir: the API consumer owns those files
        // and places them by path (tiles are imported on demand).
        if (job->bundle_dir_override.empty()) {
            std::error_code ec;
            for (const auto &t : m.tiles) {
                if (!t.done) {
                    continue;
                }
                std::error_code c;
                fs::copy_file(lib_dir / pathFromUtf8(t.file),
                              packStructuresDir() / "mystructure" / pathFromUtf8(m.name + "__" + t.file),
                              fs::copy_options::overwrite_existing, c);
            }
        }
        const std::string dim_note = m.dimension == "overworld" ? "" : "，维度 " + m.dimension;
        finishExport(job, "§a结构束导出成功: " + m.dirName() + " (" + std::to_string(m.tiles.size()) + " 片，区域 " +
                              std::to_string(m.x1) + " " + std::to_string(m.y1) + " " + std::to_string(m.z1) +
                              " -> " + std::to_string(m.x2) + " " + std::to_string(m.y2) + " " +
                              std::to_string(m.z2) + dim_note + ")");
        return;
    }

    BundleTile &tile = m.tiles[job->tile_index];
    job->tile_attempts++;

    // Origin: player dimension exports run through the player.
    endstone::CommandSender *origin = &getServer().getCommandSender();
    if (job->via_player) {
        auto *player = findPlayer(job->player_name);
        if (player == nullptr) {
            m.partial = true;
            writeBundleManifest(m, bundle_dir);
            finishExport(job, "§c结构束导出中止 (玩家离线)，已完成 " + std::to_string(job->tile_index) + "/" +
                                  std::to_string(m.tiles.size()) + " 片，已标记为 partial。",
                         STAGE_API_ARG);
            return;
        }
        origin = player;
    }

    // Per-tile tickingareas: added fresh for the CURRENT tile only, removed
    // when the tile finishes, so the 10-area budget is reused per tile.
    if (!tile.done && job->area_names.empty()) {
        ExportJob scoped = *job;  // temporary view for the tiling helpers
        scoped.x1 = tile.x1;
        scoped.z1 = tile.z1;
        scoped.x2 = tile.x2;
        scoped.z2 = tile.z2;
        addTickingAreasForJob(scoped);
        job->area_names = std::move(scoped.area_names);
        job->tp_assist = scoped.tp_assist;
    }

    if (job->tp_assist && (job->tile_attempts % 2) == 1) {
        const int cx = (tile.x1 + tile.x2) / 2;
        const int cz = (tile.z1 + tile.z2) / 2;
        teleportJobPlayer(*job, cx, tile.y1, cz);  // designated player only
    }

    const std::string cmd = "structure save " + tile.key + " " + std::to_string(tile.x1) + " " +
                            std::to_string(tile.y1) + " " + std::to_string(tile.z1) + " " +
                            std::to_string(tile.x2) + " " + std::to_string(tile.y2) + " " +
                            std::to_string(tile.z2) + (m.entities ? " true" : " false") + " memory " +
                            (m.blocks ? "true" : "false");
    const bool ok = getServer().dispatchCommand(*origin, cmd);

    if (ok) {
        int stage = STAGE_OK;
        if (auto *manager = getManager(getServer(), stage); manager != nullptr) {
            if (StructureTemplate *tpl = manager->getStructure(tile.key); tpl != nullptr) {
                std::vector<std::uint8_t> bytes;
                int bstage = STAGE_OK;
                if (buildMcstructureBytes(tpl, bytes, bstage)) {
                    const fs::path tile_file = bundle_dir / pathFromUtf8(tile.file);
                    std::error_code ec;
                    fs::create_directories(tile_file.parent_path(), ec);
                    std::ofstream f(tile_file, std::ios::binary);
                    if (f) {
                        f.write(reinterpret_cast<const char *>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size()));
                        f.close();
                        if (f.good()) {
                            tile.done = true;
                            writeBundleManifest(m, bundle_dir);  // incremental
                            getLogger().info("Bundle '{}': tile {}/{} ({} bytes)", m.name, job->tile_index + 1,
                                             m.tiles.size(), bytes.size());
                            // release this tile's areas and move on
                            for (const auto &area : job->area_names) {
                                getServer().dispatchCommand(getServer().getCommandSender(),
                                                            "tickingarea remove " + area);
                            }
                            job->area_names.clear();
                            job->tile_attempts = 0;
                            job->tile_index++;
                            return;
                        }
                    }
                }
            }
        }
    }

    if (job->tile_attempts >= config_.export_max_attempts) {
        // Give up on this tile; keep completed tiles + mark partial.
        m.partial = true;
        writeBundleManifest(m, bundle_dir);
        for (const auto &area : job->area_names) {
            getServer().dispatchCommand(getServer().getCommandSender(), "tickingarea remove " + area);
        }
        job->area_names.clear();
        finishExport(job, "§c结构束第 " + std::to_string(job->tile_index + 1) + " 片导出失败 (区块加载超时)。已保留 " +
                              std::to_string(job->tile_index) + "/" + std::to_string(m.tiles.size()) +
                              " 片并标记 partial；重跑同一区域可续传。",
                     STAGE_BUNDLE_TILE);
    }
}

// ---------------------------------------------------------------------------
// Placement machinery: load a structure (or a whole bundle) into the world at
// a given position - "structure load" needs the TARGET chunks loaded, so the
// same area/TP assist applies.
// ---------------------------------------------------------------------------
std::string StructBridgePlugin::normalizeDimension(std::string dim)
{
    if (dim.empty() || dim == "overworld" || dim == "minecraft:overworld") {
        return "overworld";
    }
    if (dim == "nether" || dim == "the_nether" || dim == "minecraft:the_nether") {
        return "nether";
    }
    if (dim == "end" || dim == "the_end" || dim == "minecraft:the_end") {
        return "the_end";
    }
    return dim;
}

bool StructBridgePlugin::teleportPlacePlayer(PlaceJob &job, int x, int y, int z)
{
    // Designated player only - the command initiator or the explicit API
    // parameter. NEVER a random online player.
    if (job.tp_player_name.empty()) {
        return false;
    }
    auto *player = findPlayer(job.tp_player_name);
    if (player == nullptr) {
        return false;
    }
    auto *level = getServer().getLevel();
    if (level == nullptr) {
        return false;
    }
    auto *dim = level->getDimension(normalizeDimension(job.dimension));
    if (dim == nullptr) {
        dim = level->getDimension("overworld");
    }
    if (dim == nullptr) {
        return false;
    }
    endstone::Location loc(*dim, static_cast<float>(x) + 0.5F, static_cast<float>(y),
                           static_cast<float>(z) + 0.5F);
    return player->teleport(loc);
}

void StructBridgePlugin::startPlaceJob(std::shared_ptr<PlaceJob> job)
{
    place_jobs_.push_back(job);
    auto *self = this;
    std::weak_ptr<PlaceJob> weak = job;
    job->task = getServer().getScheduler().runTaskTimer(
        *this,
        [self, weak]() {
            if (auto locked = weak.lock()) {
                self->placeTick(locked);
            }
        },
        1, static_cast<std::uint64_t>(std::max(1, config_.export_retry_interval_ticks)));
}

void StructBridgePlugin::placeTick(std::shared_ptr<PlaceJob> job)
{
    job->attempts++;

    // Single structure: dispatch "structure load" once the target chunks are
    // up. Bundle: iterate tiles, each at its offset position.
    std::string key = job->name;
    int tx = job->x, ty = job->y, tz = job->z;
    if (job->bundle) {
        if (job->tile_index >= job->bundle->tiles.size()) {
            const std::string dim_note = job->bundle->dimension == "overworld"
                                             ? ""
                                             : "，维度 " + job->bundle->dimension;
            finishPlace(job, "§a结构束放置完成: " + job->bundle->dirName() + " (" +
                                 std::to_string(job->bundle->tiles.size()) + " 片" + dim_note + ")");
            return;
        }
        const BundleTile &tile = job->bundle->tiles[job->tile_index];
        key = tile.key;
        // placement position: target origin + tile's offset inside the region
        tx = job->x + (tile.x1 - job->bundle->x1);
        ty = job->y + (tile.y1 - job->bundle->y1);
        tz = job->z + (tile.z1 - job->bundle->z1);
        if (job->original) {
            tx = tile.x1;
            ty = tile.y1;
            tz = tile.z1;
        }
    }

    // Chunk loading for the target footprint (structure size unknown here,
    // so cover a generous square around the origin; the engine tolerates
    // extra loaded chunks).
    if (!job->areas_added) {
        const int r = 64;  // one structure / one bundle tile is at most 64x64 wide
        ExportJob scoped;
        scoped.x1 = tx;
        scoped.z1 = tz;
        scoped.x2 = tx + r - 1;
        scoped.z2 = tz + r - 1;
        scoped.via_player = false;
        scoped.player_name.clear();
        addTickingAreasForJob(scoped);
        job->area_names = std::move(scoped.area_names);
        job->tp_assist = scoped.tp_assist;
        job->areas_added = true;
    }
    if (job->tp_assist && (job->attempts % 2) == 1) {
        if (!teleportPlacePlayer(*job, tx + 8, ty, tz + 8) && job->attempts >= config_.export_max_attempts) {
            finishPlace(job, "§c放置失败: 目标区块无法加载 (常加载区块预算不足且无可传送的发起玩家)。", STAGE_PLACE_AREA);
            return;
        }
    }

    // Nether/end targets need a player origin (console = overworld).
    endstone::CommandSender *origin = &getServer().getCommandSender();
    const std::string dim = normalizeDimension(job->bundle ? job->bundle->dimension : job->dimension);
    if (dim != "overworld") {
        auto *player = findPlayer(job->tp_player_name);
        if (player == nullptr) {
            finishPlace(job, "§c放置失败: 维度 " + dim + " 的放置需要一位在该维度的玩家 (tp_player)，"
                             "仅允许使用指定的发起玩家。",
                        STAGE_API_NO_PLAYER);
            return;
        }
        origin = player;
    }

    // Bundle tiles only live in the engine while their export session ran -
    // after a restart (or when the bundle came from another server) import
    // the tile on demand from the bundle directory before the load dispatch.
    // Single structures get the same treatment from the library file. A
    // structure with NO library copy (world-database-only, e.g. an old
    // structure-block save) falls through: the vanilla load command itself
    // resolves names through the engine's lazy db loader, so dispatching it
    // is the correct "import" there.
    {
        int stage = STAGE_OK;
        auto *manager = getManager(getServer(), stage);
        if (manager == nullptr) {
            finishPlace(job, "§c获取 StructureManager 失败。", stage);
            return;
        }
        const std::string full_key =
            key.find(':') == std::string::npos ? "mystructure:" + key : key;
        if (manager->getStructure(full_key) == nullptr) {
            fs::path src;
            bool have_src = true;
            if (job->bundle) {
                const fs::path bdir = job->bundle->src_dir.empty() ? bundleDirInLibrary(job->bundle->name)
                                                                   : job->bundle->src_dir;
                src = bdir / pathFromUtf8(job->bundle->tiles[job->tile_index].file);
            }
            else {
                src = libraryDir() / sanitizeRelativePath(job->name);
                have_src = fs::is_regular_file(src);
            }
            if (have_src) {
                std::ifstream in(src, std::ios::binary);
                std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                                std::istreambuf_iterator<char>());
                if (!in || !nbt_check::validate(bytes, "format_version")) {
                    finishPlace(job, "§c结构文件缺失或损坏 (引擎与结构库均不可用): " + pathToUtf8(src),
                                job->bundle ? STAGE_BUNDLE_MANIFEST : STAGE_API_NOT_FOUND);
                    return;
                }
                const int rc = importMcstructureBytes(manager, full_key, bytes);
                if (rc != STAGE_OK) {
                    finishPlace(job, std::string("§c结构导入失败 (阶段: ") + stageName(rc) + ")。", rc);
                    return;
                }
            }
            // !have_src (non-bundle): no library copy - the dispatch below
            // goes through the engine's own lazy world-db lookup.
        }
    }

    const std::string cmd = "structure load " + key + " " + std::to_string(tx) + " " + std::to_string(ty) + " " +
                            std::to_string(tz) + " 0_degrees none " + (job->entities ? "true" : "false") + " " +
                            (job->blocks ? "true" : "false");
    const bool ok = getServer().dispatchCommand(*origin, cmd);

    if (ok) {
        if (job->bundle) {
            job->tile_index++;
            job->attempts = 0;
            // refresh areas for the next tile
            for (const auto &area : job->area_names) {
                getServer().dispatchCommand(getServer().getCommandSender(), "tickingarea remove " + area);
            }
            job->area_names.clear();
            job->areas_added = false;
            return;
        }
        for (const auto &area : job->area_names) {
            getServer().dispatchCommand(getServer().getCommandSender(), "tickingarea remove " + area);
        }
        job->area_names.clear();
        finishPlace(job, "§a放置完成: " + job->name + " -> " + std::to_string(tx) + " " + std::to_string(ty) + " " +
                             std::to_string(tz));
        return;
    }

    if (job->attempts >= config_.export_max_attempts) {
        for (const auto &area : job->area_names) {
            getServer().dispatchCommand(getServer().getCommandSender(), "tickingarea remove " + area);
        }
        job->area_names.clear();
        finishPlace(job, "§c放置失败: structure load 命令在 " + std::to_string(job->attempts) +
                             " 次尝试内未生效 (目标区块可能未加载)。", STAGE_PLACE_LOAD);
    }
}

void StructBridgePlugin::finishPlace(std::shared_ptr<PlaceJob> &job, const std::string &message, int stage)
{
    if (job->task) {
        job->task->cancel();
        job->task = nullptr;
    }
    for (const auto &area : job->area_names) {
        getServer().dispatchCommand(getServer().getCommandSender(), "tickingarea remove " + area);
    }
    if (job->tp_player_name.empty()) {
        getServer().getCommandSender().sendMessage("{}", message);
    }
    else if (auto *player = findPlayer(job->tp_player_name)) {
        player->sendMessage("{}", message);
    }
    getLogger().info("Place '{}': {}", job->name, stripColorCodes(message));

    if (job->c_on_done != nullptr) {
        job->c_on_done(job->c_ctx, stage, stripColorCodes(message).c_str());
    }
#ifdef STRUCTBRIDGE_PYTHON_API
    if (job->py_on_done != nullptr) {
        PyObject *fn = reinterpret_cast<PyObject *>(job->py_on_done);
        job->py_on_done = nullptr;
        const std::string plain = stripColorCodes(message);
        PyGILState_STATE gil = PyGILState_Ensure();
        PyObject *cb_args = Py_BuildValue("(is)", stage, plain.c_str());
        if (cb_args != nullptr) {
            PyObject *result = PyObject_CallObject(fn, cb_args);
            Py_XDECREF(result);
            Py_DECREF(cb_args);
        }
        if (PyErr_Occurred() != nullptr) {
            PyErr_Print();
        }
        Py_DECREF(fn);
        PyGILState_Release(gil);
    }
#endif
    place_jobs_.erase(std::remove_if(place_jobs_.begin(), place_jobs_.end(),
                                     [&job](const std::shared_ptr<PlaceJob> &j) { return j.get() == job.get(); }),
                      place_jobs_.end());
}

// ---------------------------------------------------------------------------
// Layout migration: pre-0.1.0 used exports/ (+ a default/ subfolder from an
// even older release). 0.1.0 renames to library/ and folds default/ into
// mystructure/ once, then keeps the config in sync.
// ---------------------------------------------------------------------------
void StructBridgePlugin::migrateLegacyLayout()
{
    const fs::path data = getDataFolder();
    const fs::path legacy_dir = data / "exports";
    const fs::path lib_dir = data / pathFromUtf8(config_.library_dir);
    std::error_code ec;

    // 1) exports/ -> library/ (only when the target does not exist yet).
    if (fs::exists(legacy_dir, ec) && !fs::exists(lib_dir, ec)) {
        fs::rename(legacy_dir, lib_dir, ec);
        if (ec) {
            // rename across devices or similar - copy instead
            ec.clear();
            fs::copy(legacy_dir, lib_dir, fs::copy_options::recursive, ec);
            if (!ec) {
                fs::remove_all(legacy_dir, ec);
            }
        }
        if (!ec) {
            getLogger().info("Migrated structure library: exports/ -> {}", config_.library_dir);
        }
    }

    // 2) legacy default/ namespace folder -> mystructure/ inside the library.
    const fs::path legacy_ns = lib_dir / "default";
    const fs::path real_ns = lib_dir / "mystructure";
    if (fs::exists(legacy_ns, ec) && !fs::exists(real_ns, ec)) {
        fs::rename(legacy_ns, real_ns, ec);
        if (!ec) {
            getLogger().info("Migrated legacy library layout: default/ -> mystructure/");
        }
    }

    // 3) Rewrite config.json when it still references the old layout.
    if (!config_.output_dir_legacy.empty()) {
        json j;
        bool need_write = false;
        try {
            std::ifstream in(data / "config.json");
            in >> j;
            if (j.contains("output_dir")) {
                j.erase("output_dir");
                need_write = true;
            }
            if (!j.contains("library_dir")) {
                j["library_dir"] = config_.library_dir;
                need_write = true;
            }
        }
        catch (const std::exception &) {
            return;
        }
        if (need_write) {
            std::ofstream out(data / "config.json");
            out << j.dump(2) << std::endl;
            getLogger().info("Migrated config.json: output_dir -> library_dir");
        }
    }
}

fs::path StructBridgePlugin::serverRoot() const
{
    // getDataFolder() = <server>/plugins/structbridge
    fs::path p = fs::absolute(getDataFolder());
    if (p.filename() == "structbridge") {
        p = p.parent_path();  // plugins/
    }
    return p.parent_path();  // <server>/
}

void StructBridgePlugin::handleExport(endstone::CommandSender &sender, const std::vector<std::string> &args)
{
    if (gate_ != GateState::Available) {
        sender.sendMessage("§cStructBridge 未适配当前服务端，结构导出暂不可用。输入 /struct status 查看详情。");
        return;
    }
    if (!checkPermission(sender)) {
        return;
    }
    if (args.size() < 7) {
        sender.sendMessage("§e用法: /struct export <名称> <x1> <y1> <z1> <x2> <y2> <z2> [entities] [blocks]");
        return;
    }
    const std::string &name = args[0];
    if (!isValidStructureName(name)) {
        sender.sendMessage(
            "§c结构名称非法: 只允许 字母/数字/_/-/. 和一个可选的命名空间冒号，例如 myhouse 或 pack:house");
        return;
    }
    int coords[6];
    try {
        for (int i = 0; i < 6; ++i) {
            coords[i] = std::stoi(args[i + 1]);
        }
    }
    catch (const std::exception &) {
        sender.sendMessage("§c坐标必须是整数。");
        return;
    }
    bool entities = config_.include_entities;
    bool blocks = config_.include_blocks;
    if (args.size() >= 8) {
        entities = args[7] == "true" || args[7] == "1";
    }
    if (args.size() >= 9) {
        blocks = args[8] == "true" || args[8] == "1";
    }

    int x1 = std::min(coords[0], coords[3]), y1 = std::min(coords[1], coords[4]),
        z1 = std::min(coords[2], coords[5]);
    int x2 = std::max(coords[0], coords[3]), y2 = std::max(coords[1], coords[4]),
        z2 = std::max(coords[2], coords[5]);
    if (y2 - y1 > 384) {
        sender.sendMessage("§c区域过大 (Y 轴最多 384 格 - 引擎结构高度上限; 世界本身也只有 384 格高)。");
        return;
    }

    auto job = std::make_shared<ExportJob>();
    job->name = name;
    job->x1 = x1;
    job->y1 = y1;
    job->z1 = z1;
    job->x2 = x2;
    job->y2 = y2;
    job->z2 = z2;
    job->entities = entities;
    job->blocks = blocks;
    std::string dimension = "overworld";
    if (auto *player = sender.asPlayer()) {
        // Player origin: the vanilla command runs in the PLAYER's dimension.
        job->via_player = true;
        job->player_name = player->getName();
        // The initiator is the ONLY player the plugin may ever teleport.
        job->tp_player_name = player->getName();
        dimension = normalizeDimension(player->getLocation().getDimension().getName());
    }
    // Console origin: works fine, but Endstone binds it to the overworld.

    const bool need_bundle = (x2 - x1 + 1) > 64 || (z2 - z1 + 1) > 64;
    if (need_bundle) {
        job->bundle = planBundle(name, x1, y1, z1, x2, y2, z2, dimension, entities, blocks);
        job->bundle->src_dir = bundleDirInLibrary(name);
        writeBundleManifest(*job->bundle, bundleDirInLibrary(name));
        sender.sendMessage("§7区域 {}x{} 超过单片上限 64x64 (引擎限制 64x384x64)，自动分片为结构束 §e{}§7 ({} 片，逐片导出)…",
                           x2 - x1 + 1, z2 - z1 + 1, name + ".bundle", job->bundle->tiles.size());
    }
    startExportJob(job);

    const int total_seconds =
        (config_.export_max_attempts * std::max(1, config_.export_retry_interval_ticks)) / 20;
    sender.sendMessage("§7正在导出 {} …(区块加载中，最多等待约 {} 秒)", name, total_seconds);
}

// ---------------------------------------------------------------------------
// /struct import - a .mcstructure file OR a .bundle structure bundle, by
// library name or by explicit path. Bundles register every tile under its
// engine key and are then placeable as one unit (/struct place <name>).
// ---------------------------------------------------------------------------
void StructBridgePlugin::handleImport(endstone::CommandSender &sender, const std::vector<std::string> &args)
{
    if (gate_ != GateState::Available) {
        sender.sendMessage("§cStructBridge 未适配当前服务端，结构导入暂不可用。输入 /struct status 查看详情。");
        return;
    }
    if (!checkPermission(sender)) {
        return;
    }
    if (args.size() != 1) {
        sender.sendMessage("§e用法: /struct import <文件或名称>  (名称=文件名: 自动查找结构库; 也可直接给文件路径)");
        return;
    }
    // Message-typed parameter: accepts bare numeric names, quoted strings and
    // slash paths alike (string-typed params reject all three).
    std::string arg = args[0];
    if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
        arg = arg.substr(1, arg.size() - 2);
    }
    fs::path file;
    std::string name;
    bool maybe_bundle = false;
    if (arg.find('/') != std::string::npos || arg.find('\\') != std::string::npos) {
        // Explicit path: the structure name comes from the file name.
        file = pathFromUtf8(arg);
        name = pathToUtf8(file.stem());
        maybe_bundle = file.extension() == ".bundle";
    }
    else {
        // Bare name (optionally with an extension): look it up in the library.
        name = arg;
        if (name.size() > 12) {
            const std::string low = [&] {
                std::string s = name.substr(name.size() - 12);
                for (char &c : s) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return s;
            }();
            if (low == ".mcstructure") {
                name = name.substr(0, name.size() - 12);
            }
        }
        if (name.size() > 7) {
            const std::string low = [&] {
                std::string s = name.substr(name.size() - 7);
                for (char &c : s) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return s;
            }();
            if (low == ".bundle") {
                name = name.substr(0, name.size() - 7);
                maybe_bundle = true;
            }
        }
        file = maybe_bundle ? bundleDirInLibrary(name) : libraryDir() / sanitizeRelativePath(name);
    }
    if (!isValidStructureName(name)) {
        sender.sendMessage("§c结构名称非法: 只允许 中文等UTF-8字符/字母/数字/_/-/. 和一个可选的命名空间冒号，"
                           "例如 123、myhouse 或 中文屋。");
        return;
    }
    if (!fs::exists(file)) {
        // BDS chdirs into the bedrock_server folder at startup, so relative
        // paths resolve against it. As a fallback, also accept paths relative
        // to the server root (the folder that contains bedrock_server/).
        if (file.is_relative()) {
            const fs::path alt = serverRoot() / file;
            if (fs::exists(alt)) {
                file = alt;
            }
        }
    }
    // Bundle by explicit path: the directory itself (or its parent when the
    // user pointed at the manifest).
    if (maybe_bundle && fs::exists(file) && fs::is_regular_file(file)) {
        file = file.parent_path();
    }
    if (!fs::exists(file)) {
        sender.sendMessage("§c文件不存在: {} (未指定路径时自动查找结构库)", pathToUtf8(file));
        return;
    }

    // ---- Bundle branch: register every tile, copy into the library. ----
    if (fs::is_directory(file)) {
        auto manifest = readBundleManifest(file);
        if (manifest == nullptr) {
            sender.sendMessage("§c{} 不是有效的结构束 (缺少或无法解析 manifest.json)。", pathToUtf8(file));
            return;
        }
        int stage = STAGE_OK;
        auto *manager = getManager(getServer(), stage);
        if (manager == nullptr) {
            sender.sendMessage("§c获取 StructureManager 失败 (阶段: {})。", stageName(stage));
            return;
        }
        int imported = 0;
        for (const auto &tile : manifest->tiles) {
            std::ifstream in(file / pathFromUtf8(tile.file), std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
            if (!in || !nbt_check::validate(bytes, "format_version")) {
                sender.sendMessage("§c结构束分片文件缺失或损坏: {}", tile.file);
                return;
            }
            if (importMcstructureBytes(manager, tile.key, bytes) != STAGE_OK) {
                sender.sendMessage("§c结构束分片导入失败: {} (阶段: {})。", tile.file, stageName(STAGE_IMPORT_PARSE));
                return;
            }
            ++imported;
        }
        // Persist the whole bundle dir in the library (canonical copy) and
        // mirror the tiles at their flat engine-key-compatible names.
        if (bundleDirInLibrary(manifest->name) != fs::absolute(file)) {
            std::error_code ec;
            fs::remove_all(bundleDirInLibrary(manifest->name), ec);
            fs::copy(file, bundleDirInLibrary(manifest->name), fs::copy_options::recursive, ec);
        }
        for (const auto &tile : manifest->tiles) {
            std::ifstream in(bundleDirInLibrary(manifest->name) / pathFromUtf8(tile.file), std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
            if (in) {
                writeStructureFile(packStructuresDir() / "mystructure" /
                                       pathFromUtf8(manifest->name + "__" + tile.file),
                                   bytes);
            }
        }
        const std::string partial_note = manifest->partial ? "§c[partial: 部分分片缺失]§a " : "";
        sender.sendMessage("§a结构束导入成功: {}{} ({} 片已注册)。放置: §e/struct place {} <x> <y> <z>§a 或 §e/struct "
                           "place {} original§a 原位复原。",
                           partial_note, manifest->name, imported, manifest->name, manifest->name);
        getLogger().info("Imported bundle '{}' ({} tiles) from {}", manifest->name, imported, pathToUtf8(file));
        return;
    }

    // ---- Single .mcstructure branch. ----
    std::ifstream in(file, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!nbt_check::validate(bytes, "format_version")) {
        sender.sendMessage("§c文件不是有效的 .mcstructure (NBT 校验失败，缺少 format_version)。");
        return;
    }

    int stage = STAGE_OK;
    auto *manager = getManager(getServer(), stage);
    if (manager == nullptr) {
        sender.sendMessage("§c获取 StructureManager 失败 (阶段: {})。", stageName(stage));
        return;
    }

    // Bare names are looked up by /structure load under the mystructure:
    // namespace, so register bare-name imports there. Namespaced names are
    // registered verbatim.
    const std::string key = name.find(':') == std::string::npos ? "mystructure:" + name : name;
    const int rc = importMcstructureBytes(manager, key, bytes);
    if (rc != STAGE_OK) {
        sender.sendMessage("§c导入失败 (阶段: {})。文件可能损坏或与本服务端版本不兼容。", stageName(rc));
        return;
    }
    // Persist: canonical copy in the library + mirrored copy in the vanilla
    // behavior pack so the engine's own pack scan picks the name up on the
    // next start (lazy, zero plugin-side memory). The in-memory registration
    // above only covers THIS session.
    writeStructureFile(libraryDir() / sanitizeRelativePath(name), bytes);
    writeStructureFile(packStructuresDir() / sanitizeRelativePath(name), bytes);
    sender.sendMessage(
        "§a导入成功: {}。立即可用: §e/structure load {} [x y z]§a 或 §e/struct place {} <x> <y> <z>§a；重启后依然可用。",
        name, name, name);
    getLogger().info("Imported '{}' ({} bytes) from {} into manager key '{}'", name, bytes.size(), pathToUtf8(file),
                     key);
}

// ---------------------------------------------------------------------------
// /struct place - place a structure (or a whole bundle) into the world:
//   /struct place <name> <x> <y> <z> [entities] [blocks]
//   /struct place <name> original [entities] [blocks]   (bundle: original
//     position + dimension from the manifest - cross-server building import)
// Chunk loading uses the same budget as exports (tickingareas, then TP of the
// INITIATING player only - never anyone else, and never a random player).
// ---------------------------------------------------------------------------
void StructBridgePlugin::handlePlace(endstone::CommandSender &sender, const std::vector<std::string> &args)
{
    if (gate_ != GateState::Available) {
        sender.sendMessage("§cStructBridge 未适配当前服务端，结构放置暂不可用。输入 /struct status 查看详情。");
        return;
    }
    if (!checkPermission(sender)) {
        return;
    }
    if (args.size() < 2) {
        sender.sendMessage("§e用法: /struct place <名称> <x> <y> <z> [entities] [blocks]");
        sender.sendMessage("§e      /struct place <名称> original [entities] [blocks]  (结构束按原坐标/维度复原)");
        return;
    }
    std::string name = args[0];
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        name = name.substr(1, name.size() - 2);
    }
    // Path-form bundle references (e.g. backups/castle.bundle, relative to
    // the server root) are valid placement targets even though they are not
    // plain structure names.
    auto bundle_ref = resolveBundleRef(name);
    if (bundle_ref == nullptr && !isValidStructureName(name)) {
        sender.sendMessage("§c结构名称非法。");
        return;
    }

    bool original = false;
    int x = 0, y = 0, z = 0;
    if (args[1] == "original") {
        original = true;
    }
    else {
        if (args.size() < 4) {
            sender.sendMessage("§e用法: /struct place <名称> <x> <y> <z> [entities] [blocks]");
            sender.sendMessage("§e      /struct place <名称> original [entities] [blocks]");
            return;
        }
        try {
            x = std::stoi(args[1]);
            y = std::stoi(args[2]);
            z = std::stoi(args[3]);
        }
        catch (const std::exception &) {
            sender.sendMessage("§c坐标必须是整数 (或使用 original 进行结构束原位复原)。");
            return;
        }
    }
    const std::size_t flag_base = original ? 2 : 4;
    bool entities = config_.include_entities;
    bool blocks = config_.include_blocks;
    if (args.size() > flag_base) {
        entities = args[flag_base] == "true" || args[flag_base] == "1";
    }
    if (args.size() > flag_base + 1) {
        blocks = args[flag_base + 1] == "true" || args[flag_base + 1] == "1";
    }

    auto job = std::make_shared<PlaceJob>();
    job->name = name;
    job->x = x;
    job->y = y;
    job->z = z;
    job->original = original;
    job->entities = entities;
    job->blocks = blocks;
    // The initiator is the ONLY player the plugin may ever teleport.
    std::string dimension = "overworld";
    if (auto *player = sender.asPlayer()) {
        job->tp_player_name = player->getName();
        dimension = normalizeDimension(player->getLocation().getDimension().getName());
    }

    // Bundle? ("castle", "castle.bundle", "mystructure:castle" and PATHS like
    // "backups/castle.bundle" - relative to the server root - all work)
    if (bundle_ref) {
        job->bundle = bundle_ref;
        if (original) {
            // Restore at the exported position, in the exported dimension -
            // this is the cross-server building migration path.
            job->x = bundle_ref->x1;
            job->y = bundle_ref->y1;
            job->z = bundle_ref->z1;
            dimension = bundle_ref->dimension;
        }
        sender.sendMessage("§7结构束 §e{}§7: {} 片，{}",
                           bundle_ref->dirName(), bundle_ref->tiles.size(),
                           original ? "按原坐标复原 (" + std::to_string(bundle_ref->x1) + " " +
                                           std::to_string(bundle_ref->y1) + " " +
                                           std::to_string(bundle_ref->z1) + ", 维度 " +
                                           bundle_ref->dimension + ")"
                                    : "目标 " + std::to_string(x) + " " + std::to_string(y) + " " +
                                           std::to_string(z));
        if (bundle_ref->partial) {
            sender.sendMessage("§e注意: 该结构束标记为 partial (导出时有分片失败)，复原可能不完整。");
        }
    }
    else {
        // Single structure: must exist in the engine memory, the library, a
        // behavior pack or the world database - otherwise fail fast instead
        // of spinning the retry timer.
        if (original) {
            sender.sendMessage("§coriginal 仅支持结构束 (.bundle)。单个结构文件不记录原坐标，请给出 <x> <y> <z>。");
            return;
        }
        const std::string full_key = name.find(':') == std::string::npos ? "mystructure:" + name : name;
        bool found = false;
        int stage = STAGE_OK;
        auto *manager = getManager(getServer(), stage);
        if (manager != nullptr && manager->getStructure(full_key) != nullptr) {
            found = true;
        }
        if (!found) {
            for (const auto &item : collectStructures()) {
                if (item.key == full_key) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            sender.sendMessage("§c未找到结构 §e{}§c。用 §e/struct list§c 查看可用的结构 (引擎内存/结构库/行为包/世界数据库)。",
                               name);
            return;
        }
    }
    job->dimension = dimension;
    startPlaceJob(job);

    const int total_seconds =
        (config_.export_max_attempts * std::max(1, config_.export_retry_interval_ticks)) / 20;
    sender.sendMessage("§7正在放置 {} …(区块加载中，最多等待约 {} 秒)", name, total_seconds);
}

// ---------------------------------------------------------------------------
// /structdump - dump an ALREADY-SAVED structure template to a .mcstructure
// file by name (structure block saves and /structure save both register the
// template in the engine's StructureManager, so no chunk loading or retry is
// needed - this is a synchronous lookup + engine serialization).
// ---------------------------------------------------------------------------
void StructBridgePlugin::handleDump(endstone::CommandSender &sender, const std::vector<std::string> &args)
{
    if (gate_ != GateState::Available) {
        sender.sendMessage("§cStructBridge 未适配当前服务端，结构导出暂不可用。输入 /structstatus 查看详情。");
        return;
    }
    if (!checkPermission(sender)) {
        return;
    }
    if (args.empty()) {
        sender.sendMessage("§e用法: /struct dump <名称>  (导出已保存的结构: 结构块界面保存的或 /structure save 保存的)");
        return;
    }
    // The message-typed parameter preserves surrounding quotes - strip them,
    // so both /structdump 123 and /structdump "123" work.
    std::string name = args[0];
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        name = name.substr(1, name.size() - 2);
    }
    if (!isValidStructureName(name)) {
        sender.sendMessage("§c结构名称非法: 只允许 中文等UTF-8字符/字母/数字/_/-/. 和一个可选的命名空间冒号，"
                           "例如 123、myhouse 或 中文屋。");
        return;
    }

    int stage = STAGE_OK;
    auto *manager = getManager(getServer(), stage);
    if (manager == nullptr) {
        sender.sendMessage("§c获取 StructureManager 失败 (阶段: {})。", stageName(stage));
        return;
    }

    // Structure blocks and /structure save register bare names under the
    // mystructure: namespace ("Saved a structure with name mystructure:<name>"),
    // so try the exact key first and then the namespaced fallback.
    StructureTemplate *tpl = manager->getStructure(name);
    if (tpl == nullptr && name.find(':') == std::string::npos) {
        tpl = manager->getStructure("mystructure:" + name);
    }
    if (tpl == nullptr) {
        // Disk-saved structures are LAZILY loaded after a server restart: poke
        // the engine with a zero-side-effect vanilla load (integrity 0.0 places
        // no blocks, entities/waterlogging disabled - verified in the sandbox:
        // testforblock confirms the target stays untouched). Registration of
        // the template happens synchronously during command execution.
        const std::string key = name.find(':') == std::string::npos ? "mystructure:" + name : name;
        getServer().dispatchCommand(getServer().getCommandSender(),
                                    "structure load \"" + key + "\" 0 64 0 0_degrees none false true false 0.0");
        tpl = manager->getStructure(name);
        if (tpl == nullptr && name.find(':') == std::string::npos) {
            tpl = manager->getStructure("mystructure:" + name);
        }
    }
    if (tpl == nullptr) {
        sender.sendMessage("§c未找到结构 '{}'。请先保存: 在结构块界面点击\"保存\"，或使用 "
                           "§e/structure save {} <x1> <y1> <z1> <x2> <y2> <z2>§c (末尾加 §edisk§c 可跨重启保存)。",
                           name, name);
        return;
    }

    std::vector<std::uint8_t> bytes;
    int bstage = STAGE_OK;
    if (!buildMcstructureBytes(tpl, bytes, bstage)) {
        sender.sendMessage("§c序列化失败 (阶段: {})。", stageName(bstage));
        return;
    }

    auto out_file = libraryDir() / sanitizeRelativePath(name);
    std::error_code ec;
    fs::create_directories(out_file.parent_path(), ec);
    {
        std::ofstream f(out_file, std::ios::binary);
        if (!f) {
            sender.sendMessage("§c无法写入文件: {}", pathToUtf8(out_file));
            return;
        }
        f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        f.close();
        if (!f.good()) {
            sender.sendMessage("§c无法写入文件: {}", pathToUtf8(out_file));
            return;
        }
    }
    // Mirror into the vanilla behavior pack so the name stays loadable after
    // a restart (and remains available even without this plugin).
    writeStructureFile(packStructuresDir() / sanitizeRelativePath(name), bytes);
    sender.sendMessage("§a导出成功: {} ({} 字节)", pathToUtf8(out_file), bytes.size());
    getLogger().info("Dumped saved structure '{}' ({} bytes) to {}", name, bytes.size(), pathToUtf8(out_file));
}

// Collect every structure visible on this server, merged and deduplicated by
// engine key across the three sources it can exist in:
//   1. the plugin's exports library (any namespace folder; the legacy
//      "default" maps to mystructure),
//   2. every behavior pack's structures/ dir - vanilla ships e.g. bastion:,
//      minecraft:; the engine loads these lazily at level start. The mirror
//      dirs some tools create (__brarchive/) are SIBLINGS of structures/, so
//      this walk never enters them. Pack files are read-only game data,
//      except the plugin's own mystructure/ mirror inside the vanilla pack,
//   3. the world database (LevelDB): structure block saves and
//      "/structure save ... disk" entries, Chinese names included (the engine
//      stores keys as raw UTF-8 - see leveldb_scan.h).
std::vector<StructBridgePlugin::LibItem> StructBridgePlugin::collectStructures() const
{
    std::vector<LibItem> items;
    std::map<std::string, std::size_t> index;

    auto upsert = [&](const std::string &key, const std::string &local, const std::string &ns) -> LibItem & {
        auto it = index.find(key);
        if (it == index.end()) {
            index.emplace(key, items.size());
            LibItem item;
            item.key = key;
            item.local = local;
            item.ns = ns;
            items.push_back(std::move(item));
            return items.back();
        }
        return items[it->second];
    };

    // .mcstructure files under root. forced_ns empty -> the first path
    // component is the namespace folder (the engine's pack layout:
    // structures/<ns>/<rest>.mcstructure == key "<ns>:<rest>"); otherwise
    // every file maps into forced_ns.
    auto add_dir = [&](const fs::path &root, const std::string &forced_ns, bool library, bool mirror_root) {
        std::error_code ec;
        if (!fs::exists(root, ec)) {
            return;
        }
        for (auto &entry : fs::recursive_directory_iterator(root, ec)) {
            if (ec) {
                break;
            }
            std::error_code ec2;
            if (!entry.is_regular_file(ec2) || entry.path().extension() != ".mcstructure") {
                continue;
            }
            const auto rel = fs::relative(entry.path(), root, ec2);
            if (ec2 || rel.empty()) {
                continue;
            }
            // Bundle dirs are listed as UNITS by collectBundles(), not as
            // individual tile files (their engine keys use the flat
            // <name>__tile form and would otherwise show up as junk keys).
            bool inside_bundle = false;
            for (const auto &part : rel) {
                const std::string comp = pathToUtf8(part);
                if (comp.size() > 7 && comp.substr(comp.size() - 7) == ".bundle") {
                    inside_bundle = true;
                    break;
                }
            }
            if (inside_bundle) {
                continue;
            }
            std::string ns = forced_ns;
            fs::path rest = rel;
            if (forced_ns.empty()) {
                // first component is the namespace folder
                bool first = true;
                rest.clear();
                for (const auto &part : rel) {
                    if (first) {
                        ns = pathToUtf8(part);
                        first = false;
                    }
                    else {
                        rest /= part;
                    }
                }
                if (rel.parent_path().empty()) {
                    ns = "mystructure";
                    rest = rel;
                }
                if (ns == "default") {
                    ns = "mystructure";  // legacy library layout
                }
            }
            std::string local = pathToUtf8(rest.replace_extension(""));
            if (local.empty()) {
                continue;
            }
            auto sz = fs::file_size(entry.path(), ec2);
            if (ec2) {
                sz = 0;
            }
            LibItem &item = upsert(ns + ":" + local, local, ns);
            if (library) {
                item.in_library = true;
                item.file = entry.path();  // library copy preferred as representative
            }
            else {
                item.in_pack = true;
                if (mirror_root && ns == "mystructure") {
                    // the plugin's own mirror (vanilla pack never ships a
                    // mystructure namespace itself) - deletable
                    item.mirror = entry.path();
                }
                if (item.file.empty()) {
                    item.file = entry.path();
                }
            }
            item.size = std::max(item.size, sz);
        }
    };

    add_dir(libraryDir(), "", true, false);
    {
        // every behavior pack (the vanilla one contains the plugin's mirror)
        std::error_code ec;
        for (auto &pack : fs::directory_iterator(behaviorPacksDir(), ec)) {
            std::error_code ec2;
            if (!pack.is_directory(ec2)) {
                continue;
            }
            const fs::path structs = pack.path() / "structures";
            add_dir(structs, "", false, structs == packStructuresDir());
        }
    }
    {
        // world database (fail-soft: owned by the running server)
        try {
            for (const auto &ws : ldb::scanStructures(worldDbDir())) {
                std::string ns = "mystructure";
                std::string local = ws.key;
                if (auto pos = ws.key.find(':'); pos != std::string::npos) {
                    ns = ws.key.substr(0, pos);
                    local = ws.key.substr(pos + 1);
                }
                if (local.empty()) {
                    continue;
                }
                LibItem &item = upsert(ws.key, local, ns);
                item.in_world = true;
                item.size = std::max(item.size, static_cast<std::uintmax_t>(ws.size));
            }
        }
        catch (const std::exception &e) {
            getLogger().warning("World database scan failed ({}); /structlist may miss saved structures.", e.what());
        }
    }

    std::sort(items.begin(), items.end(), [](const LibItem &a, const LibItem &b) { return a.key < b.key; });
    return items;
}

std::vector<StructBridgePlugin::BundleEntry> StructBridgePlugin::collectBundles() const
{
    std::vector<BundleEntry> out;
    std::error_code ec;
    for (auto &entry : fs::directory_iterator(libraryDir() / "mystructure", ec)) {
        std::error_code ec2;
        if (!entry.is_directory(ec2)) {
            continue;
        }
        if (entry.path().extension() != ".bundle") {
            continue;
        }
        BundleEntry be;
        be.name = pathToUtf8(entry.path().stem());
        be.dir = entry.path();
        be.manifest = readBundleManifest(be.dir);
        if (be.manifest == nullptr) {
            continue;  // not a valid bundle (e.g. a leftover temp dir)
        }
        out.push_back(std::move(be));
    }
    std::sort(out.begin(), out.end(), [](const BundleEntry &a, const BundleEntry &b) { return a.name < b.name; });
    return out;
}

std::shared_ptr<BundleManifest> StructBridgePlugin::findBundle(const std::string &name) const
{
    std::string local = name;
    if (auto pos = name.find(':'); pos != std::string::npos) {
        if (name.substr(0, pos) != "mystructure") {
            return nullptr;
        }
        local = name.substr(pos + 1);
    }
    if (local.size() > 7) {
        const std::string low = local.substr(local.size() - 7);
        if (low == ".bundle") {
            local = local.substr(0, local.size() - 7);
        }
    }
    std::error_code ec;
    const fs::path dir = bundleDirInLibrary(local);
    if (!fs::exists(dir, ec)) {
        return nullptr;
    }
    return readBundleManifest(dir);
}

std::shared_ptr<BundleManifest> StructBridgePlugin::resolveBundleRef(const std::string &name) const
{
    if (auto m = findBundle(name)) {
        return m;
    }
    // Path form: "somedir/castle.bundle" (relative to the server root) or an
    // absolute path - covers bundles exported through the API to a custom
    // out_dir, which live outside the plugin library.
    const bool looks_like_path =
        name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
        (name.size() > 7 && name.compare(name.size() - 7, 7, ".bundle") == 0);
    if (!looks_like_path) {
        return nullptr;
    }
    fs::path p = pathFromUtf8(name);
    if (p.is_relative()) {
        p = serverRoot() / p;
    }
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        return nullptr;
    }
    return readBundleManifest(p);
}

// ---------------------------------------------------------------------------
// /struct delete - remove ONE structure (or bundle) EVERYWHERE:
//   1. engine memory (StructureManager template) and the world database
//      (LevelDB) entry, both handled by the vanilla /structure delete command
//      (verified in the sandbox: works for in-memory templates, same-session
//      disk saves AND lazily-stored post-restart entries; survives restart).
//      After the dispatch the world database is RE-SCANNED read-only so the
//      report is deterministic instead of "see the log".
//   2. the plugin's library file (library/mystructure/<name>.mcstructure),
//   3. the mirrored copy in behavior_packs/vanilla/structures/mystructure/.
// Bundles delete every tile's engine entry + the whole .bundle directory.
// Bulk deletion is a SEPARATE subcommand (/struct clear) with confirmation.
// Only bare names and the mystructure: namespace are accepted - other
// namespaces belong to behavior packs and must not be touched.
// ---------------------------------------------------------------------------
void StructBridgePlugin::handleDelete(endstone::CommandSender &sender, const std::vector<std::string> &args)
{
    if (gate_ != GateState::Available) {
        sender.sendMessage("§cStructBridge 未适配当前服务端，结构删除暂不可用。输入 /struct status 查看详情。");
        return;
    }
    if (!checkPermission(sender)) {
        return;
    }
    if (args.empty()) {
        sender.sendMessage("§e用法: /struct delete <名称>  (批量删除请用 /struct clear，需二次确认)");
        return;
    }
    std::string name = args[0];
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        name = name.substr(1, name.size() - 2);
    }
    if (name == "全部" || name == "all" || name == "*") {
        sender.sendMessage("§e单个删除不接受 '{}'。批量删除请用 §e/struct clear§e (需二次确认)。", name);
        return;
    }
    if (!isValidStructureName(name)) {
        sender.sendMessage("§c结构名称非法: 只允许 中文等UTF-8字符/字母/数字/_/-/. 和一个可选的命名空间冒号，"
                           "例如 123、myhouse 或 中文屋。");
        return;
    }
    std::string local = name;
    if (auto pos = name.find(':'); pos != std::string::npos) {
        if (name.substr(0, pos) != "mystructure") {
            sender.sendMessage("§c只能删除 mystructure: 命名空间的结构 (裸名自动归入)。其他命名空间属于行为包，受保护。");
            return;
        }
        local = name.substr(pos + 1);
    }
    if (local.empty()) {
        sender.sendMessage("§c结构名称非法。");
        return;
    }

    // ---- Bundle branch: delete every tile + the directory. ----
    if (auto manifest = findBundle(local)) {
        for (const auto &tile : manifest->tiles) {
            getServer().dispatchCommand(getServer().getCommandSender(), "structure delete \"" + tile.key + "\"");
            std::error_code ec;
            fs::remove(packStructuresDir() / "mystructure" / pathFromUtf8(manifest->name + "__" + tile.file), ec);
        }
        std::error_code ec;
        const std::uintmax_t n = fs::remove_all(bundleDirInLibrary(manifest->name), ec);
        sender.sendMessage("§a结构束删除完成: '{}' ({} 片引擎条目 + 目录 {} 项)。", manifest->name,
                           manifest->tiles.size(), static_cast<unsigned long long>(n));
        getLogger().info("Delete bundle '{}': {} tiles, {} dir entries removed", manifest->name,
                         manifest->tiles.size(), static_cast<unsigned long long>(n));
        return;
    }

    // ---- Single structure branch. ----
    const std::string key = "mystructure:" + local;
    bool had_in_memory = false;
    if (int stage = STAGE_OK; auto *manager = getManager(getServer(), stage)) {
        had_in_memory = manager->getStructure(key) != nullptr;
    }
    getServer().dispatchCommand(getServer().getCommandSender(), "structure delete \"" + key + "\"");
    bool engine_gone = true;
    if (int stage = STAGE_OK; auto *manager = getManager(getServer(), stage)) {
        engine_gone = manager->getStructure(key) == nullptr;
    }

    // Plugin files: library copy + pack mirror.
    std::vector<fs::path> files = {
        libraryDir() / "mystructure" / pathFromUtf8(local + ".mcstructure"),
        packStructuresDir() / "mystructure" / pathFromUtf8(local + ".mcstructure"),
    };
    std::vector<std::string> removed;
    std::error_code ec;
    for (const auto &f : files) {
        if (fs::exists(f, ec) && fs::remove(f, ec)) {
            removed.push_back(pathToUtf8(f));
        }
    }

    // Deterministic LevelDB re-check (read-only scan; the engine may batch
    // its writes, so a leftover entry is reported rather than hidden).
    bool still_in_db = false;
    try {
        for (const auto &ws : ldb::scanStructures(worldDbDir())) {
            if (ws.key == key) {
                still_in_db = true;
                break;
            }
        }
    }
    catch (const std::exception &) {
        // scan failures are non-fatal for a delete report
    }

    if (!engine_gone) {
        sender.sendMessage("§c引擎仍报告结构 {} 存在于内存中，请查看服务器日志确认删除结果。", key);
        return;
    }
    if (removed.empty() && !had_in_memory && !still_in_db) {
        sender.sendMessage("§e结构 '{}' 未找到 (内存/插件文件/世界数据库均无)。", local);
        return;
    }
    sender.sendMessage("§a删除完成: '{}'。内存: 已清除; 世界数据库: {}{}。", local,
                       still_in_db ? "§c条目仍在 (引擎写入有延迟，重启后复查)" : "§a已清除",
                       removed.empty() ? "" : "；删除 " + std::to_string(removed.size()) + " 个文件");
    for (const auto &f : removed) {
        sender.sendMessage("§7  - 已删除文件: {}", f);
    }
    getLogger().info("Delete '{}': engine gone, db {}, {} plugin file(s) removed", local, still_in_db ? "left" : "gone",
                     removed.size());
}

// ---------------------------------------------------------------------------
// /struct list [all|library|world|pack] - list structures merged from the
// library, all behavior packs' structures/ dirs and the world database, with
// per-source tags, file size and engine-memory status. DEFAULT view is
// plugin-managed only (library + own mirror): pack game data clutters the
// list and is protected anyway. Bundles are listed as units.
// ---------------------------------------------------------------------------
void StructBridgePlugin::handleList(endstone::CommandSender &sender, const std::vector<std::string> &args)
{
    if (!checkPermission(sender)) {
        return;
    }
    std::string filter = "managed";  // default: plugin-managed only
    if (!args.empty()) {
        filter = args[0];
        if (filter != "all" && filter != "library" && filter != "world" && filter != "pack" && filter != "managed") {
            sender.sendMessage("§e用法: /struct list [all|library|world|pack]  (默认只显示插件管理的)");
            return;
        }
    }

    auto bundles = collectBundles();
    auto items = collectStructures();
    StructureManager *manager = nullptr;
    if (gate_ == GateState::Available) {
        int stage = STAGE_OK;
        manager = getManager(getServer(), stage);
    }

    // ---- bundles (library objects; shown in every view except world/pack) ----
    if (filter != "world" && filter != "pack") {
        for (const auto &be : bundles) {
            const auto &m = *be.manifest;
            char size_buf[32];
            const double sz = static_cast<double>(m.totalBytes(be.dir));
            if (sz >= 1024.0 * 1024.0) {
                std::snprintf(size_buf, sizeof(size_buf), "%.1f MB", sz / (1024.0 * 1024.0));
            }
            else {
                std::snprintf(size_buf, sizeof(size_buf), "%.1f KB", sz / 1024.0);
            }
            const std::string dim_note = m.dimension == "overworld" ? "" : ", 维度 " + m.dimension;
            sender.sendMessage("§7 - §e{}§7 [结构束] ({} 片, {}, 区域 {} {} {} -> {} {} {}{}{})", m.name,
                               m.tiles.size(), std::string(size_buf), m.x1, m.y1, m.z1, m.x2, m.y2, m.z2, dim_note,
                               m.partial ? "§c[partial]" : "");
        }
    }

    // ---- structures ----
    int shown = 0;
    int n_lib = 0;
    int n_pack = 0;
    int n_world = 0;
    for (const auto &item : items) {
        n_lib += item.in_library ? 1 : 0;
        n_pack += item.in_pack ? 1 : 0;
        n_world += item.in_world ? 1 : 0;
        const bool managed = item.in_library || !item.mirror.empty();
        if (filter == "managed" && !managed) {
            continue;
        }
        if (filter == "library" && !item.in_library) {
            continue;
        }
        if (filter == "world" && !item.in_world) {
            continue;
        }
        if (filter == "pack" && !item.in_pack) {
            continue;
        }
        char size_buf[32];
        double sz = static_cast<double>(item.size);
        if (sz >= 1024.0 * 1024.0) {
            std::snprintf(size_buf, sizeof(size_buf), "%.1f MB", sz / (1024.0 * 1024.0));
        }
        else if (sz >= 1024.0) {
            std::snprintf(size_buf, sizeof(size_buf), "%.1f KB", sz / 1024.0);
        }
        else {
            std::snprintf(size_buf, sizeof(size_buf), "%u B", static_cast<unsigned>(item.size));
        }
        const bool loaded = manager != nullptr && manager->getStructure(item.key) != nullptr;
        std::string sources;
        auto add_src = [&sources](const char *tag) {
            if (!sources.empty()) {
                sources += "§7/";
            }
            sources += tag;
        };
        if (item.in_library) {
            add_src("§a库");
        }
        if (item.in_pack) {
            add_src("§b包");
        }
        if (item.in_world) {
            add_src("§d世界");
        }
        // NOTE: size_buf must be wrapped in std::string - libc++ std::format
        // renders a raw char[] as a 32-char RANGE (NULs and stack garbage
        // included), which truncates console output at the first NUL byte.
        sender.sendMessage("§7 - §f{}§7 ({}§7, {}§7, {}§7)", item.key, sources, std::string(size_buf),
                           loaded ? "§a已加载" : "§7未加载");
        ++shown;
    }

    const bool show_bundles = filter != "world" && filter != "pack";
    if (shown == 0 && !(show_bundles && !bundles.empty())) {
        sender.sendMessage("§e未找到结构 (过滤: {}; /struct list all 查看全部来源)。", filter);
        return;
    }
    sender.sendMessage("§6{} 个结构 §7(§a插件库 {}§7 | §b行为包 {}§7 | §d世界存档 {}§7){}:", shown, n_lib, n_pack, n_world,
                       show_bundles && !bundles.empty() ? " + " + std::to_string(bundles.size()) + " 个结构束" : "");
    sender.sendMessage("§7来源: §a库§7=插件结构库 | §b包§7=行为包自带 (受保护) | §d世界§7=结构块或 structure save 存档。"
                       "删除: /struct delete <名称> 或 /struct clear。");
    getLogger().info("Listed {} structure(s) + {} bundle(s) (filter={}, library {}, pack {}, world {})", shown,
                     bundles.size(), filter, n_lib, n_pack, n_world);
}

// ---------------------------------------------------------------------------
// /struct clear - delete ALL plugin-managed structures and bundles in one
// go. Destructive, so it uses a two-step confirmation: the first call arms a
// 60-second server-wide window, the second call within that window executes.
// The engine delete is only dispatched for structures that actually exist in
// the engine (in_world or currently loaded) - pure library files just get
// unlinked. Behavior pack game data is never touched.
// ---------------------------------------------------------------------------
void StructBridgePlugin::handleClear(endstone::CommandSender &sender, const std::vector<std::string> &args)
{
    if (gate_ != GateState::Available) {
        sender.sendMessage("§cStructBridge 未适配当前服务端，该功能暂不可用。输入 /struct status 查看详情。");
        return;
    }
    if (!checkPermission(sender)) {
        return;
    }
    const bool confirmed = !args.empty() && (args[0] == "confirm" || args[0] == "确认");
    if (!confirmed) {
        if (!clear_pending_ ||
            std::chrono::steady_clock::now() - clear_pending_at_ > std::chrono::seconds(60)) {
            // (re)arm the confirmation window
            clear_pending_ = true;
            clear_pending_at_ = std::chrono::steady_clock::now();
            auto bundles = collectBundles();
            auto items = collectStructures();
            int managed = 0;
            for (const auto &item : items) {
                if (item.in_library || !item.mirror.empty()) {
                    ++managed;
                }
            }
            sender.sendMessage("§e即将删除 §c{} 个结构 + {} 个结构束§e (引擎内存+数据库+全部文件)。", managed,
                               bundles.size());
            sender.sendMessage("§e此操作不可撤销。确认请输入 §c/struct clear confirm§e (60 秒内有效)。");
            return;
        }
        sender.sendMessage("§e确认请输入 §c/struct clear confirm§e。");
        return;
    }
    if (!clear_pending_ || std::chrono::steady_clock::now() - clear_pending_at_ > std::chrono::seconds(60)) {
        clear_pending_ = false;
        sender.sendMessage("§e确认窗口已过期。请先执行 §c/struct clear§e 再确认。");
        return;
    }
    clear_pending_ = false;

    int structures = 0;
    int bundles_deleted = 0;
    int files = 0;
    std::error_code ec;

    // Bundles first (their tiles are separate engine entries).
    for (const auto &be : collectBundles()) {
        for (const auto &tile : be.manifest->tiles) {
            getServer().dispatchCommand(getServer().getCommandSender(), "structure delete \"" + tile.key + "\"");
            fs::remove(packStructuresDir() / "mystructure" / pathFromUtf8(be.name + "__" + tile.file), ec);
            ec.clear();
        }
        files += static_cast<int>(fs::remove_all(be.dir, ec));
        ec.clear();
        ++bundles_deleted;
    }

    auto items = collectStructures();
    for (const auto &item : items) {
        // Plugin-managed only: the library and our own pack mirror. Behavior
        // pack structures (vanilla's bastion:, minecraft:, ...) are game data
        // and stay untouched; world-only saves are not plugin-managed either
        // (delete those by name).
        if (!item.in_library && item.mirror.empty()) {
            continue;
        }
        // Only dispatch the engine command when the engine knows the key:
        // in_world (LevelDB entry) or loaded right now. Pure library files
        // would just produce "structure not found" noise in the log.
        bool engine_knows = item.in_world;
        if (!engine_knows && gate_ == GateState::Available) {
            int stage = STAGE_OK;
            if (auto *manager = getManager(getServer(), stage)) {
                engine_knows = manager->getStructure(item.key) != nullptr;
            }
        }
        if (engine_knows) {
            getServer().dispatchCommand(getServer().getCommandSender(), "structure delete \"" + item.key + "\"");
        }
        ++structures;
        for (const auto &f : {libraryDir() / pathFromUtf8(item.ns) / pathFromUtf8(item.local + ".mcstructure"),
                              item.mirror}) {
            if (fs::exists(f, ec) && fs::remove(f, ec)) {
                ++files;
            }
            ec.clear();
        }
    }
    sender.sendMessage("§a清理完成: {} 个结构 + {} 个结构束，共删除 {} 个文件。行为包自带与世界独有的结构不受影响。",
                       structures, bundles_deleted, files);
    getLogger().info("Clear-all: {} structures, {} bundles, {} files removed", structures, bundles_deleted, files);
}

// ---------------------------------------------------------------------------
// Public API core - the same operations the commands perform, but with
// structured stage-code results instead of CommandSender messages. The
// command paths above are untouched; these implementations reuse the same
// verified primitives (getManager / importMcstructureBytes /
// buildMcstructureBytes / collectStructures / writeStructureFile).
// ---------------------------------------------------------------------------
int StructBridgePlugin::apiImport(const std::string &name_or_path, std::string &msg)
{
    if (gate_ != GateState::Available) {
        msg = "StructBridge 未通过兼容性探针，功能不可用 (/struct status 查看详情)";
        return STAGE_API_GATE;
    }
    std::string arg = name_or_path;
    if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
        arg = arg.substr(1, arg.size() - 2);
    }
    fs::path file;
    std::string name;
    bool maybe_bundle = false;
    if (arg.find('/') != std::string::npos || arg.find('\\') != std::string::npos) {
        file = pathFromUtf8(arg);
        name = pathToUtf8(file.stem());
        maybe_bundle = file.extension() == ".bundle";
    }
    else {
        name = arg;
        if (name.size() > 12) {
            std::string low = name.substr(name.size() - 12);
            for (char &c : low) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (low == ".mcstructure") {
                name = name.substr(0, name.size() - 12);
            }
        }
        if (name.size() > 7) {
            std::string low = name.substr(name.size() - 7);
            for (char &c : low) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (low == ".bundle") {
                name = name.substr(0, name.size() - 7);
                maybe_bundle = true;
            }
        }
        file = maybe_bundle ? bundleDirInLibrary(name) : libraryDir() / sanitizeRelativePath(name);
    }
    if (!isValidStructureName(name)) {
        msg = "结构名称非法: " + name;
        return STAGE_API_ARG;
    }
    if (!fs::exists(file)) {
        if (file.is_relative()) {
            const fs::path alt = serverRoot() / file;
            if (fs::exists(alt)) {
                file = alt;
            }
        }
    }
    if (maybe_bundle && fs::exists(file) && fs::is_regular_file(file)) {
        file = file.parent_path();
    }
    if (!fs::exists(file)) {
        msg = "文件不存在: " + pathToUtf8(file);
        return STAGE_API_NOT_FOUND;
    }

    // ---- Bundle branch: register every tile under its engine key. ----
    if (fs::is_directory(file)) {
        auto manifest = readBundleManifest(file);
        if (manifest == nullptr) {
            msg = "不是有效的结构束 (manifest.json 缺失或损坏): " + pathToUtf8(file);
            return STAGE_BUNDLE_MANIFEST;
        }
        int stage = STAGE_OK;
        auto *manager = getManager(getServer(), stage);
        if (manager == nullptr) {
            msg = std::string("获取 StructureManager 失败 (阶段: ") + stageName(stage) + ")";
            return stage;
        }
        for (const auto &tile : manifest->tiles) {
            std::ifstream in(file / pathFromUtf8(tile.file), std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
            if (!in || !nbt_check::validate(bytes, "format_version")) {
                msg = "结构束分片缺失或损坏: " + tile.file;
                return STAGE_BUNDLE_MANIFEST;
            }
            const int rc = importMcstructureBytes(manager, tile.key, bytes);
            if (rc != STAGE_OK) {
                msg = std::string("分片导入失败 (阶段: ") + stageName(rc) + "): " + tile.file;
                return rc;
            }
        }
        if (bundleDirInLibrary(manifest->name) != fs::absolute(file)) {
            std::error_code ec;
            fs::remove_all(bundleDirInLibrary(manifest->name), ec);
            fs::copy(file, bundleDirInLibrary(manifest->name), fs::copy_options::recursive, ec);
        }
        msg = "结构束导入成功: " + manifest->name + " (" + std::to_string(manifest->tiles.size()) + " 片)";
        getLogger().info("[api] Imported bundle '{}' ({} tiles) from {}", manifest->name, manifest->tiles.size(),
                         pathToUtf8(file));
        return STAGE_OK;
    }

    // ---- Single .mcstructure branch. ----
    std::ifstream in(file, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!nbt_check::validate(bytes, "format_version")) {
        msg = "文件不是有效的 .mcstructure (NBT 校验失败，缺少 format_version): " + pathToUtf8(file);
        return STAGE_IMPORT_PARSE;
    }
    int stage = STAGE_OK;
    auto *manager = getManager(getServer(), stage);
    if (manager == nullptr) {
        msg = std::string("获取 StructureManager 失败 (阶段: ") + stageName(stage) + ")";
        return stage;
    }
    const std::string key = name.find(':') == std::string::npos ? "mystructure:" + name : name;
    const int rc = importMcstructureBytes(manager, key, bytes);
    if (rc != STAGE_OK) {
        msg = std::string("导入失败 (阶段: ") + stageName(rc) + ")";
        return rc;
    }
    writeStructureFile(libraryDir() / sanitizeRelativePath(name), bytes);
    writeStructureFile(packStructuresDir() / sanitizeRelativePath(name), bytes);
    msg = "导入成功: " + name + " (" + std::to_string(bytes.size()) + " 字节, key=" + key + ")";
    getLogger().info("[api] Imported '{}' ({} bytes) from {}", name, bytes.size(), pathToUtf8(file));
    return STAGE_OK;
}

int StructBridgePlugin::apiDump(const std::string &name, std::string &msg)
{
    if (gate_ != GateState::Available) {
        msg = "StructBridge 未通过兼容性探针，功能不可用";
        return STAGE_API_GATE;
    }
    std::string clean = name;
    if (clean.size() >= 2 && clean.front() == '"' && clean.back() == '"') {
        clean = clean.substr(1, clean.size() - 2);
    }
    if (!isValidStructureName(clean)) {
        msg = "结构名称非法: " + clean;
        return STAGE_API_ARG;
    }
    int stage = STAGE_OK;
    auto *manager = getManager(getServer(), stage);
    if (manager == nullptr) {
        msg = std::string("获取 StructureManager 失败 (阶段: ") + stageName(stage) + ")";
        return stage;
    }
    StructureTemplate *tpl = manager->getStructure(clean);
    if (tpl == nullptr && clean.find(':') == std::string::npos) {
        tpl = manager->getStructure("mystructure:" + clean);
    }
    if (tpl == nullptr) {
        // Lazily-stored disk structure: poke it into memory with a
        // zero-side-effect vanilla load (integrity 0.0 places no blocks).
        const std::string key = clean.find(':') == std::string::npos ? "mystructure:" + clean : clean;
        getServer().dispatchCommand(getServer().getCommandSender(),
                                    "structure load \"" + key + "\" 0 64 0 0_degrees none false true false 0.0");
        tpl = manager->getStructure(clean);
        if (tpl == nullptr && clean.find(':') == std::string::npos) {
            tpl = manager->getStructure("mystructure:" + clean);
        }
    }
    if (tpl == nullptr) {
        msg = "未找到结构: " + clean;
        return STAGE_API_NOT_FOUND;
    }
    std::vector<std::uint8_t> bytes;
    int bstage = STAGE_OK;
    if (!buildMcstructureBytes(tpl, bytes, bstage)) {
        msg = std::string("序列化失败 (阶段: ") + stageName(bstage) + ")";
        return bstage;
    }
    auto out_file = libraryDir() / sanitizeRelativePath(clean);
    std::error_code ec;
    fs::create_directories(out_file.parent_path(), ec);
    {
        std::ofstream f(out_file, std::ios::binary);
        if (!f) {
            msg = "无法写入文件: " + pathToUtf8(out_file);
            return STAGE_FILE;
        }
        f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        f.close();
        if (!f.good()) {
            msg = "无法写入文件: " + pathToUtf8(out_file);
            return STAGE_FILE;
        }
    }
    writeStructureFile(packStructuresDir() / sanitizeRelativePath(clean), bytes);
    msg = pathToUtf8(out_file);
    getLogger().info("[api] Dumped '{}' ({} bytes) to {}", clean, bytes.size(), pathToUtf8(out_file));
    return STAGE_OK;
}

int StructBridgePlugin::apiDelete(const char *name_or_null, std::string &msg)
{
    if (gate_ != GateState::Available) {
        msg = "StructBridge 未通过兼容性探针，功能不可用";
        return STAGE_API_GATE;
    }
    auto dispatch_engine_delete = [&](const std::string &key) {
        getServer().dispatchCommand(getServer().getCommandSender(), "structure delete \"" + key + "\"");
    };

    if (name_or_null == nullptr) {
        // Delete ALL plugin-managed structures AND bundles (library + own
        // pack mirror only - behavior pack game data and world-only saves are
        // not plugin-managed and stay).
        int structures = 0;
        int bundles_deleted = 0;
        int files = 0;
        for (const auto &be : collectBundles()) {
            for (const auto &tile : be.manifest->tiles) {
                dispatch_engine_delete(tile.key);
                std::error_code ec;
                fs::remove(packStructuresDir() / "mystructure" / pathFromUtf8(be.name + "__" + tile.file), ec);
            }
            std::error_code ec;
            files += static_cast<int>(fs::remove_all(be.dir, ec));
            ++bundles_deleted;
        }
        auto items = collectStructures();
        for (const auto &item : items) {
            if (!item.in_library && item.mirror.empty()) {
                continue;
            }
            bool engine_knows = item.in_world;
            if (!engine_knows) {
                int stage = STAGE_OK;
                if (auto *manager = getManager(getServer(), stage)) {
                    engine_knows = manager->getStructure(item.key) != nullptr;
                }
            }
            if (engine_knows) {
                dispatch_engine_delete(item.key);
            }
            ++structures;
            for (const auto &f : {libraryDir() / pathFromUtf8(item.ns) / pathFromUtf8(item.local + ".mcstructure"),
                                  item.mirror}) {
                std::error_code ec;
                if (fs::exists(f, ec) && fs::remove(f, ec)) {
                    ++files;
                }
            }
        }
        if (structures == 0 && bundles_deleted == 0) {
            msg = "没有插件管理的结构可删除";
            return STAGE_OK;
        }
        msg = "已删除全部 " + std::to_string(structures) + " 个结构 + " + std::to_string(bundles_deleted) +
              " 个结构束, 清理 " + std::to_string(files) + " 个文件";
        getLogger().info("[api] Delete-all: {} structures, {} bundles, {} files", structures, bundles_deleted, files);
        return STAGE_OK;
    }

    std::string name = name_or_null;
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        name = name.substr(1, name.size() - 2);
    }
    if (!isValidStructureName(name)) {
        msg = "结构名称非法: " + name;
        return STAGE_API_ARG;
    }
    std::string local = name;
    if (auto pos = name.find(':'); pos != std::string::npos) {
        if (name.substr(0, pos) != "mystructure") {
            msg = "只能删除 mystructure: 命名空间的结构 (其他命名空间受保护)";
            return STAGE_API_ARG;
        }
        local = name.substr(pos + 1);
    }
    if (local.empty()) {
        msg = "结构名称非法";
        return STAGE_API_ARG;
    }
    const std::string key = "mystructure:" + local;

    // Bundle branch: delete every tile's engine entry + the directory.
    if (auto manifest = findBundle(local)) {
        for (const auto &tile : manifest->tiles) {
            dispatch_engine_delete(tile.key);
            std::error_code ec;
            fs::remove(packStructuresDir() / "mystructure" / pathFromUtf8(manifest->name + "__" + tile.file), ec);
        }
        std::error_code ec;
        const auto n = fs::remove_all(bundleDirInLibrary(manifest->name), ec);
        msg = "结构束已删除: " + manifest->name + " (" + std::to_string(manifest->tiles.size()) + " 片, " +
              std::to_string(static_cast<unsigned long long>(n)) + " 目录项)";
        getLogger().info("[api] Delete bundle '{}'", manifest->name);
        return STAGE_OK;
    }

    dispatch_engine_delete(key);
    std::vector<fs::path> files = {
        libraryDir() / "mystructure" / pathFromUtf8(local + ".mcstructure"),
        packStructuresDir() / "mystructure" / pathFromUtf8(local + ".mcstructure"),
    };
    int removed = 0;
    std::error_code ec;
    for (const auto &f : files) {
        if (fs::exists(f, ec) && fs::remove(f, ec)) {
            ++removed;
        }
    }
    msg = "已删除 '" + local + "' (引擎内存 + 世界数据库" +
          (removed > 0 ? ", " + std::to_string(removed) + " 个文件" : "") + ")";
    getLogger().info("[api] Delete '{}': engine command dispatched, {} file(s) removed", local, removed);
    return STAGE_OK;
}

std::uint64_t StructBridgePlugin::apiExportRegion(const std::string &name, int x1, int y1, int z1, int x2, int y2,
                                                  int z2, const char *dimension, bool entities, bool blocks,
                                                  void (*on_done)(void *ctx, int stage, const char *message),
                                                  void *ctx, void *py_on_done)
{
    // v1 shim: same job machinery, library output, overworld-only (the console
    // origin Endstone binds for API callers is the overworld; dimensioned
    // exports go through apiExportRegionTo with an explicit tp_player).
    if (dimension != nullptr && normalizeDimension(dimension) != "overworld") {
        return 0;
    }
    return apiExportRegionTo(name, x1, y1, z1, x2, y2, z2, dimension, entities, blocks, nullptr, nullptr, false,
                             on_done, ctx, py_on_done);
}

std::uint64_t StructBridgePlugin::apiExportRegionTo(const std::string &name, int x1, int y1, int z1, int x2, int y2,
                                                    int z2, const char *dimension, bool entities, bool blocks,
                                                    const char *out_dir, const char *tp_player, bool force_bundle,
                                                    void (*on_done)(void *ctx, int stage, const char *message),
                                                    void *ctx, void *py_on_done)
{
    if (gate_ != GateState::Available) {
        return 0;
    }
    if (!isValidStructureName(name)) {
        return 0;
    }
    std::string dim = normalizeDimension(dimension != nullptr ? dimension : "");
    // Non-overworld exports need a PLAYER origin (the console origin is bound
    // to the overworld). The designated player is the explicit tp_player
    // parameter - never a random online player.
    if (dim != "overworld" && (tp_player == nullptr || tp_player[0] == '\0')) {
        return 0;
    }

    const int lx1 = std::min(x1, x2), ly1 = std::min(y1, y2), lz1 = std::min(z1, z2);
    const int lx2 = std::max(x1, x2), ly2 = std::max(y1, y2), lz2 = std::max(z1, z2);
    if (ly2 - ly1 > 384) {
        return 0;  // Y cannot be tiled: the engine's per-template height limit
                   // is 384 and the world itself is only 384 blocks tall
    }

    auto job = std::make_shared<ExportJob>();
    job->name = name;
    job->x1 = lx1;
    job->y1 = ly1;
    job->z1 = lz1;
    job->x2 = lx2;
    job->y2 = ly2;
    job->z2 = lz2;
    job->entities = entities;
    job->blocks = blocks;
    job->c_on_done = on_done;
    job->c_ctx = ctx;
    job->py_on_done = py_on_done;
    if (dim != "overworld") {
        job->via_player = true;
        job->player_name = tp_player;
    }
    if (tp_player != nullptr && tp_player[0] != '\0') {
        job->tp_player_name = tp_player;
    }
    if (out_dir != nullptr && out_dir[0] != '\0') {
        fs::path dir = pathFromUtf8(out_dir);
        if (dir.is_relative()) {
            dir = serverRoot() / dir;
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            return 0;
        }
        job->out_dir = dir;
    }

    const bool oversize = (lx2 - lx1 + 1) > 64 || (lz2 - lz1 + 1) > 64;
    if (oversize || force_bundle) {
        job->bundle = planBundle(name, lx1, ly1, lz1, lx2, ly2, lz2, dim, entities, blocks);
        // Custom out_dir redirects the bundle directory itself.
        if (job->out_dir.empty()) {
            job->bundle->src_dir = bundleDirInLibrary(name);
            writeBundleManifest(*job->bundle, bundleDirInLibrary(name));
        }
        else {
            const fs::path bdir = job->out_dir / pathFromUtf8(name + ".bundle");
            std::error_code ec;
            fs::create_directories(bdir, ec);
            job->bundle_dir_override = bdir;
            job->bundle->src_dir = bdir;
        }
    }
    startExportJob(job);
    return ++api_job_counter_;
}

std::uint64_t StructBridgePlugin::apiPlaceStructure(const std::string &name, int x, int y, int z, bool original,
                                                    const char *dimension, bool entities, bool blocks,
                                                    const char *tp_player,
                                                    void (*on_done)(void *ctx, int stage, const char *message),
                                                    void *ctx, void *py_on_done)
{
    if (gate_ != GateState::Available) {
        return 0;
    }
    // Path-form bundle references (relative to the server root, or absolute)
    // are valid even though they are not plain structure names.
    auto bundle_ref = resolveBundleRef(name);
    if (bundle_ref == nullptr && !isValidStructureName(name)) {
        return 0;
    }
    std::string dim = normalizeDimension(dimension != nullptr ? dimension : "");

    auto job = std::make_shared<PlaceJob>();
    job->name = name;
    job->x = x;
    job->y = y;
    job->z = z;
    job->original = original;
    job->entities = entities;
    job->blocks = blocks;
    job->c_on_done = on_done;
    job->c_ctx = ctx;
    job->py_on_done = py_on_done;
    if (tp_player != nullptr && tp_player[0] != '\0') {
        // The ONLY player this job may ever teleport.
        job->tp_player_name = tp_player;
    }

    if (bundle_ref) {
        job->bundle = bundle_ref;
        if (original) {
            job->x = bundle_ref->x1;
            job->y = bundle_ref->y1;
            job->z = bundle_ref->z1;
            dim = bundle_ref->dimension;
        }
        else if (dim.empty()) {
            dim = "overworld";
        }
    }
    else {
        if (original) {
            return 0;  // single structures carry no original position
        }
        const std::string full_key = name.find(':') == std::string::npos ? "mystructure:" + name : name;
        bool found = false;
        int stage = STAGE_OK;
        auto *manager = getManager(getServer(), stage);
        if (manager != nullptr && manager->getStructure(full_key) != nullptr) {
            found = true;
        }
        if (!found) {
            for (const auto &item : collectStructures()) {
                if (item.key == full_key) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            return 0;
        }
        if (dim.empty()) {
            dim = "overworld";
        }
    }
    // Non-overworld placement needs a player origin; require tp_player.
    if (normalizeDimension(dim) != "overworld" && job->tp_player_name.empty()) {
        return 0;
    }
    job->dimension = dim;
    startPlaceJob(job);
    return ++api_job_counter_;
}

// ---------------------------------------------------------------------------
// /structapi - verify the exported C ABI the way a CONSUMER plugin would:
// dlopen our own library (dladdr finds the loaded path), dlsym the factory,
// then call through the interface. This proves symbol visibility, the
// factory and the virtual dispatch without writing a second plugin.
// ---------------------------------------------------------------------------
void StructBridgePlugin::handleApiSelfTest(endstone::CommandSender &sender)
{
    if (!checkPermission(sender)) {
        return;
    }
#if !defined(_WIN32)
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&structbridge_get_api), &info) == 0 || info.dli_fname == nullptr) {
        sender.sendMessage("§cAPI 自检失败: 无法解析自身库路径 (dladdr)。");
        return;
    }
    void *h = dlopen(info.dli_fname, RTLD_NOW);
    if (h == nullptr) {
        sender.sendMessage("§cAPI 自检失败: dlopen: {}", dlerror());
        return;
    }
    auto ver_fn = reinterpret_cast<structbridge_api_version_fn>(dlsym(h, "structbridge_api_version"));
    auto get_fn = reinterpret_cast<structbridge_get_api_fn>(dlsym(h, "structbridge_get_api"));
    if (ver_fn == nullptr || get_fn == nullptr) {
        sender.sendMessage("§cAPI 自检失败: 导出符号未找到 (dlsym)。");
        return;
    }
    if (ver_fn() != STRUCTBRIDGE_API_VERSION) {
        sender.sendMessage("§cAPI 自检失败: 头文件版本 {} != 库版本 {}", STRUCTBRIDGE_API_VERSION, ver_fn());
        return;
    }
    auto *api = get_fn();
    if (api == nullptr) {
        sender.sendMessage("§cAPI 自检失败: structbridge_get_api() 返回空 (插件未完成加载)。");
        return;
    }
    sender.sendMessage("§aAPI 自检通过: 库已加载，符号可解析，接口可调用。");
    sender.sendMessage("§7 - 库: {}", info.dli_fname);
    sender.sendMessage("§7 - API 版本: {} (插件 v{}), 可用: {}", api->apiVersion(), api->version(),
                       api->isAvailable() ? "是" : "否");
    const int count = api->listStructures(nullptr, nullptr);
    sender.sendMessage("§7 - listStructures() -> {} 个结构", count);
    getLogger().info("[api] Self-test passed via {}", info.dli_fname);
#else
    sender.sendMessage("§eAPI 自检 (dlopen/dlsym) 仅支持 Linux。");
#endif
}

// ---------------------------------------------------------------------------
// /structprobe
// ---------------------------------------------------------------------------
void StructBridgePlugin::handleProbe(endstone::CommandSender &sender)
{
    if (!checkPermission(sender)) {
        return;
    }
    if (getServer().getLevel() == nullptr) {
        sender.sendMessage("§c世界尚未加载完成，无法探测。稍后重试。");
        return;
    }
    gate_ = GateState::NotProbed;
    probe_stage_ = -1;
    probe_detail_.clear();
    runProbe(true);
    if (gate_ == GateState::Available) {
        sender.sendMessage("§a探测通过，功能已启用: {}", probe_detail_);
    }
    else {
        sender.sendMessage("§c探测未通过 (阶段: {}, {})。功能保持禁用。", stageName(probe_stage_), probe_detail_);
    }
}

// ---------------------------------------------------------------------------
// /struct status - gate state, layout, retry budgets and in-flight jobs.
// ---------------------------------------------------------------------------
void StructBridgePlugin::handleStatus(endstone::CommandSender &sender, bool rerun_probe)
{
    if (rerun_probe && getServer().getLevel() != nullptr) {
        gate_ = GateState::NotProbed;
        probe_stage_ = -1;
        probe_detail_.clear();
        runProbe(true);
    }
    sender.sendMessage("§7=== StructBridge 状态 ===");
    const char *state = gate_ == GateState::Available      ? "§a可用"
                        : gate_ == GateState::Unavailable ? "§c不可用"
                                                           : "§e未探测";
    sender.sendMessage("原生链路: {}{}", state,
                       probe_stage_ >= 0 ? " §7(最后探测阶段: " + std::string(stageName(probe_stage_)) + ")"
                                         : "");
    if (!probe_detail_.empty()) {
        sender.sendMessage("§7详情: {}", probe_detail_);
    }
    sender.sendMessage("§7Endstone: {} | Minecraft: {} | 插件: {} (API v{})", getServer().getVersion(),
                       getServer().getMinecraftVersion(), kPluginVersion, STRUCTBRIDGE_API_VERSION);
    sender.sendMessage("§7结构库: {}", pathToUtf8(libraryDir()));
    sender.sendMessage("§7导出: 原版 structure save (引擎捕获) + 重试 {} 次 / 每 {} tick | 导入: 引擎虚 load 链路",
                       config_.export_max_attempts, config_.export_retry_interval_ticks);
    sender.sendMessage("§7进行中的任务: 导出 {} | 放置 {} | 结构束 {} 个", export_jobs_.size(), place_jobs_.size(),
                       collectBundles().size());
#if defined(_WIN32)
    sender.sendMessage("§7平台: Windows (无 fork 沙箱，需在 config.json 显式开启)");
#else
    sender.sendMessage("§7平台: Linux (fork 沙箱实测)");
#endif
    sender.sendMessage("§7提示: 超过 64x64 的导出会自动分片为结构束; TP 辅助只传送发起操作的玩家本人。");
}

// ---------------------------------------------------------------------------
// ApiImpl - the structbridge::IApi implementation. Bound to the live plugin
// instance; created once in onLoad(). The Python-facing extras are NON-virtual
// helpers (the C++ ABI surface stays exactly as declared in structbridge_api.h).
// ---------------------------------------------------------------------------
class ApiImpl final : public structbridge::IApi {
public:
    explicit ApiImpl(StructBridgePlugin *plugin) : plugin_(plugin) {}

    // ---- IApi (C++ consumer ABI) ----
    [[nodiscard]] std::uint32_t apiVersion() const override
    {
        return STRUCTBRIDGE_API_VERSION;
    }
    [[nodiscard]] bool isAvailable() const override
    {
        return plugin_->gate_ == GateState::Available;
    }
    [[nodiscard]] const char *version() const override
    {
        return kPluginVersion;
    }
    [[nodiscard]] const char *stageMessage(int stage) const override
    {
        return stageName(stage);
    }
    std::uint64_t exportRegion(const char *name, int x1, int y1, int z1, int x2, int y2, int z2,
                               const char *dimension, bool include_entities, bool include_blocks,
                               void (*on_done)(void *ctx, int stage, const char *message), void *ctx) override
    {
        if (name == nullptr) {
            return 0;
        }
        return plugin_->apiExportRegion(name, x1, y1, z1, x2, y2, z2, dimension, include_entities,
                                        include_blocks, on_done, ctx, nullptr);
    }
    // v2: custom output dir + explicit TP player + bundle control. tp_player
    // names the ONLY player the job may teleport (dimensioned exports and
    // chunk-load assist); nullptr/"" disables teleport assist entirely.
    std::uint64_t exportRegionTo(const char *name, int x1, int y1, int z1, int x2, int y2, int z2,
                                 const char *dimension, bool include_entities, bool include_blocks,
                                 const char *out_dir, const char *tp_player, bool force_bundle,
                                 void (*on_done)(void *ctx, int stage, const char *message), void *ctx) override
    {
        if (name == nullptr) {
            return 0;
        }
        return plugin_->apiExportRegionTo(name, x1, y1, z1, x2, y2, z2, dimension, include_entities,
                                          include_blocks, out_dir, tp_player, force_bundle, on_done, ctx, nullptr);
    }
    // v2: place a structure (or bundle) into the world. original=true restores
    // a bundle at its exported position/dimension (cross-server migration).
    std::uint64_t placeStructure(const char *name, int x, int y, int z, bool original, const char *dimension,
                                 bool include_entities, bool include_blocks, const char *tp_player,
                                 void (*on_done)(void *ctx, int stage, const char *message), void *ctx) override
    {
        if (name == nullptr) {
            return 0;
        }
        return plugin_->apiPlaceStructure(name, x, y, z, original, dimension, include_entities, include_blocks,
                                          tp_player, on_done, ctx, nullptr);
    }
    int importStructure(const char *name_or_path, char *msg, unsigned long msg_cap) override
    {
        std::string m;
        const int rc = plugin_->apiImport(name_or_path != nullptr ? name_or_path : "", m);
        copyMsg(msg, msg_cap, m);
        return rc;
    }
    int dumpStructure(const char *name, char *msg, unsigned long msg_cap) override
    {
        std::string m;
        const int rc = plugin_->apiDump(name != nullptr ? name : "", m);
        copyMsg(msg, msg_cap, m);
        return rc;
    }
    int deleteStructure(const char *name_or_null, char *msg, unsigned long msg_cap) override
    {
        std::string m;
        const int rc = plugin_->apiDelete(name_or_null, m);
        copyMsg(msg, msg_cap, m);
        return rc;
    }
    int listStructures(void (*on_item)(void *ctx, const char *key, std::uint64_t size_bytes, int loaded_in_engine),
                       void *ctx) override
    {
        auto items = plugin_->collectStructures();
        StructureManager *manager = nullptr;
        if (plugin_->gate_ == GateState::Available) {
            int stage = STAGE_OK;
            manager = StructBridgePlugin::getManager(plugin_->getServer(), stage);
        }
        int n = 0;
        for (const auto &item : items) {
            const int loaded = manager != nullptr && manager->getStructure(item.key) != nullptr;
            if (on_item != nullptr) {
                on_item(ctx, item.key.c_str(), static_cast<std::uint64_t>(item.size), loaded);
            }
            ++n;
        }
        return n;
    }

#ifdef STRUCTBRIDGE_PYTHON_API
    // ---- Python-facing helpers (not part of the C++ ABI) ----
    struct ListItem {
        std::string key;
        std::uint64_t size;
        bool loaded;
        std::vector<std::string> sources;  // subset of {"library","pack","world"}
    };
    [[nodiscard]] std::vector<ListItem> listItems() const
    {
        std::vector<ListItem> out;
        auto items = plugin_->collectStructures();
        StructureManager *manager = nullptr;
        if (plugin_->gate_ == GateState::Available) {
            int stage = STAGE_OK;
            manager = StructBridgePlugin::getManager(plugin_->getServer(), stage);
        }
        for (const auto &item : items) {
            ListItem li;
            li.key = item.key;
            li.size = static_cast<std::uint64_t>(item.size);
            li.loaded = manager != nullptr && manager->getStructure(item.key) != nullptr;
            if (item.in_library) {
                li.sources.emplace_back("library");
            }
            if (item.in_pack) {
                li.sources.emplace_back("pack");
            }
            if (item.in_world) {
                li.sources.emplace_back("world");
            }
            out.push_back(std::move(li));
        }
        return out;
    }
    [[nodiscard]] int probeStage() const
    {
        return plugin_->probe_stage_;
    }
    [[nodiscard]] const char *probeDetail() const
    {
        return plugin_->probe_detail_.c_str();
    }
    [[nodiscard]] int structureCount() const
    {
        return static_cast<int>(plugin_->collectStructures().size());
    }
    [[nodiscard]] bool defaultEntities() const
    {
        return plugin_->config_.include_entities;
    }
    [[nodiscard]] bool defaultBlocks() const
    {
        return plugin_->config_.include_blocks;
    }
    int importStructureStr(const char *arg, std::string &msg)
    {
        return plugin_->apiImport(arg != nullptr ? arg : "", msg);
    }
    int dumpStructureStr(const char *name, std::string &msg)
    {
        return plugin_->apiDump(name != nullptr ? name : "", msg);
    }
    int deleteStructureStr(const char *name_or_null, std::string &msg)
    {
        return plugin_->apiDelete(name_or_null, msg);
    }
    std::uint64_t exportRegionPy(const std::string &name, int x1, int y1, int z1, int x2, int y2, int z2,
                                 const char *dimension, bool entities, bool blocks, void *py_cb)
    {
        return plugin_->apiExportRegion(name, x1, y1, z1, x2, y2, z2, dimension, entities, blocks, nullptr,
                                        nullptr, py_cb);
    }
    std::uint64_t exportRegionToPy(const std::string &name, int x1, int y1, int z1, int x2, int y2, int z2,
                                   const char *dimension, bool entities, bool blocks, const char *out_dir,
                                   const char *tp_player, bool force_bundle, void *py_cb)
    {
        return plugin_->apiExportRegionTo(name, x1, y1, z1, x2, y2, z2, dimension, entities, blocks, out_dir,
                                          tp_player, force_bundle, nullptr, nullptr, py_cb);
    }
    std::uint64_t placeStructurePy(const std::string &name, int x, int y, int z, bool original,
                                   const char *dimension, bool entities, bool blocks, const char *tp_player,
                                   void *py_cb)
    {
        return plugin_->apiPlaceStructure(name, x, y, z, original, dimension, entities, blocks, tp_player, nullptr,
                                          nullptr, py_cb);
    }
    void rerunProbe()
    {
        plugin_->runProbe(true);
    }
#endif  // STRUCTBRIDGE_PYTHON_API

private:
    static void copyMsg(char *dst, unsigned long cap, const std::string &src)
    {
        if (dst == nullptr || cap == 0) {
            return;
        }
        std::snprintf(dst, cap, "%s", src.c_str());
    }
    StructBridgePlugin *plugin_;
};

ApiImpl *createApiImpl(StructBridgePlugin *plugin)
{
    return new ApiImpl(plugin);
}

// ---------------------------------------------------------------------------
// In-process Python module "structbridge" - registered into sys.modules in
// onLoad(), so ANY Python plugin can simply `import structbridge`. All methods
// are thin wrappers over the ApiImpl above.
// ---------------------------------------------------------------------------
#ifdef STRUCTBRIDGE_PYTHON_API
namespace pyapi {

ApiImpl *api()
{
    return g_api_instance;
}

// Returning NULL from a pyapi function REQUIRES a set exception - use this
// whenever the API is not alive (plugin not loaded / being torn down).
PyObject *apiNotReady()
{
    PyErr_SetString(PyExc_RuntimeError, "structbridge API is not available (plugin not loaded?)");
    return nullptr;
}

PyObject *api_version(PyObject *, PyObject *)
{
    return PyLong_FromUnsignedLong(STRUCTBRIDGE_API_VERSION);
}

PyObject *is_available(PyObject *, PyObject *)
{
    return PyBool_FromLong(api() != nullptr && api()->isAvailable());
}

PyObject *plugin_version(PyObject *, PyObject *)
{
    return PyUnicode_FromString(api() != nullptr ? api()->version() : "");
}

PyObject *stage_message(PyObject *, PyObject *args)
{
    int stage = 0;
    if (!PyArg_ParseTuple(args, "i", &stage)) {
        return nullptr;
    }
    return PyUnicode_FromString(stageName(stage));
}

PyObject *status(PyObject *, PyObject *)
{
    if (api() == nullptr) {
        Py_RETURN_NONE;
    }
    return Py_BuildValue("{s:O,s:i,s:s,s:s,s:i,s:i}", "available", api()->isAvailable() ? Py_True : Py_False,
                         "probe_stage", api()->probeStage(), "probe_detail", api()->probeDetail(), "version",
                         api()->version(), "api_version", STRUCTBRIDGE_API_VERSION, "structures",
                         api()->structureCount());
}

PyObject *probe(PyObject *, PyObject *)
{
    if (api() == nullptr) {
        return apiNotReady();
    }
    api()->rerunProbe();  // asynchronous; results land in the log and status()
    Py_RETURN_NONE;
}

PyObject *import_structure(PyObject *, PyObject *args)
{
    const char *arg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &arg)) {
        return nullptr;
    }
    if (api() == nullptr) {
        return apiNotReady();
    }
    std::string msg;
    const int rc = api()->importStructureStr(arg, msg);
    return Py_BuildValue("{s:O,s:i,s:s}", "ok", rc == STAGE_OK ? Py_True : Py_False, "stage", rc, "message",
                         msg.c_str());
}

PyObject *dump_structure(PyObject *, PyObject *args)
{
    const char *name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name)) {
        return nullptr;
    }
    if (api() == nullptr) {
        return apiNotReady();
    }
    std::string msg;
    const int rc = api()->dumpStructureStr(name, msg);
    return Py_BuildValue("{s:O,s:i,s:s}", "ok", rc == STAGE_OK ? Py_True : Py_False, "stage", rc, "message",
                         msg.c_str());
}

PyObject *delete_structure(PyObject *, PyObject *args)
{
    PyObject *name_obj = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &name_obj)) {
        return nullptr;
    }
    if (api() == nullptr) {
        return apiNotReady();
    }
    const char *name = nullptr;
    if (name_obj != Py_None) {
        name = PyUnicode_AsUTF8(name_obj);
        if (name == nullptr) {
            return nullptr;
        }
    }
    std::string msg;
    const int rc = api()->deleteStructureStr(name, msg);
    return Py_BuildValue("{s:O,s:i,s:s}", "ok", rc == STAGE_OK ? Py_True : Py_False, "stage", rc, "message",
                         msg.c_str());
}

PyObject *list_structures(PyObject *, PyObject *)
{
    if (api() == nullptr) {
        return apiNotReady();
    }
    auto items = api()->listItems();
    PyObject *list = PyList_New(items.size());
    if (list == nullptr) {
        return nullptr;
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        PyObject *sources = PyList_New(items[i].sources.size());
        if (sources == nullptr) {
            Py_DECREF(list);
            return nullptr;
        }
        for (std::size_t j = 0; j < items[i].sources.size(); ++j) {
            PyObject *s = PyUnicode_FromString(items[i].sources[j].c_str());
            if (s == nullptr) {
                Py_DECREF(sources);
                Py_DECREF(list);
                return nullptr;
            }
            PyList_SetItem(sources, static_cast<Py_ssize_t>(j), s);  // steals s
        }
        PyObject *d = Py_BuildValue("{s:s,s:K,s:O,s:N}", "key", items[i].key.c_str(), "size",
                                    static_cast<unsigned long long>(items[i].size), "loaded",
                                    items[i].loaded ? Py_True : Py_False, "sources", sources);
        if (d == nullptr) {
            Py_DECREF(sources);  // not stolen on failure
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SetItem(list, static_cast<Py_ssize_t>(i), d);  // steals d's reference
    }
    return list;
}

PyObject *export_region(PyObject *, PyObject *args, PyObject *kw)
{
    const char *name = nullptr;
    int x1 = 0, y1 = 0, z1 = 0, x2 = 0, y2 = 0, z2 = 0;
    PyObject *dim = Py_None;
    PyObject *entities = Py_None;
    PyObject *blocks = Py_None;
    PyObject *on_done = Py_None;
    static const char *kwlist[] = {"name",       "x1",                "y1",      "z1",
                                   "x2",         "y2",                "z2",      "dimension",
                                   "include_entities", "include_blocks", "on_done", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "siiiiii|OOOO", const_cast<char **>(kwlist), &name, &x1, &y1, &z1,
                                     &x2, &y2, &z2, &dim, &entities, &blocks, &on_done)) {
        return nullptr;
    }
    if (api() == nullptr) {
        return apiNotReady();
    }
    const char *dim_c = nullptr;
    if (dim != Py_None) {
        dim_c = PyUnicode_AsUTF8(dim);
        if (dim_c == nullptr) {
            return nullptr;
        }
    }
    bool ent = api()->defaultEntities();
    if (entities != Py_None) {
        ent = PyObject_IsTrue(entities) == 1;
    }
    bool blk = api()->defaultBlocks();
    if (blocks != Py_None) {
        blk = PyObject_IsTrue(blocks) == 1;
    }
    void *py_cb = nullptr;
    if (on_done != Py_None) {
        if (!PyCallable_Check(on_done)) {
            PyErr_SetString(PyExc_TypeError, "on_done must be callable or None");
            return nullptr;
        }
        Py_INCREF(on_done);  // strong ref; finishExport() releases it after firing
        py_cb = on_done;
    }
    const std::uint64_t job = api()->exportRegionPy(name, x1, y1, z1, x2, y2, z2, dim_c, ent, blk, py_cb);
    if (job == 0 && py_cb != nullptr) {
        Py_DECREF(on_done);  // job rejected - give the reference back
    }
    return PyLong_FromUnsignedLongLong(job);
}

PyObject *export_region_to(PyObject *, PyObject *args, PyObject *kw)
{
    const char *name = nullptr;
    int x1 = 0, y1 = 0, z1 = 0, x2 = 0, y2 = 0, z2 = 0;
    PyObject *dim = Py_None;
    PyObject *entities = Py_None;
    PyObject *blocks = Py_None;
    PyObject *out_dir = Py_None;
    PyObject *tp_player = Py_None;
    PyObject *force_bundle = Py_False;
    PyObject *on_done = Py_None;
    static const char *kwlist[] = {"name",    "x1",   "y1",    "z1",
                                   "x2",      "y2",   "z2",    "dimension",
                                   "include_entities", "include_blocks", "out_dir", "tp_player",
                                   "force_bundle",     "on_done",        nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "siiiiii|OOOOOOO", const_cast<char **>(kwlist), &name, &x1, &y1,
                                     &z1, &x2, &y2, &z2, &dim, &entities, &blocks, &out_dir, &tp_player,
                                     &force_bundle, &on_done)) {
        return nullptr;
    }
    if (api() == nullptr) {
        return apiNotReady();
    }
    const char *dim_c = nullptr;
    if (dim != Py_None) {
        dim_c = PyUnicode_AsUTF8(dim);
        if (dim_c == nullptr) {
            return nullptr;
        }
    }
    bool ent = api()->defaultEntities();
    if (entities != Py_None) {
        ent = PyObject_IsTrue(entities) == 1;
    }
    bool blk = api()->defaultBlocks();
    if (blocks != Py_None) {
        blk = PyObject_IsTrue(blocks) == 1;
    }
    const char *out_c = nullptr;
    if (out_dir != Py_None) {
        out_c = PyUnicode_AsUTF8(out_dir);
        if (out_c == nullptr) {
            return nullptr;
        }
    }
    const char *tp_c = nullptr;
    if (tp_player != Py_None) {
        tp_c = PyUnicode_AsUTF8(tp_player);
        if (tp_c == nullptr) {
            return nullptr;
        }
    }
    const bool bundle = PyObject_IsTrue(force_bundle) == 1;
    void *py_cb = nullptr;
    if (on_done != Py_None) {
        if (!PyCallable_Check(on_done)) {
            PyErr_SetString(PyExc_TypeError, "on_done must be callable or None");
            return nullptr;
        }
        Py_INCREF(on_done);  // strong ref; finishExport() releases it after firing
        py_cb = on_done;
    }
    const std::uint64_t job =
        api()->exportRegionToPy(name, x1, y1, z1, x2, y2, z2, dim_c, ent, blk, out_c, tp_c, bundle, py_cb);
    if (job == 0 && py_cb != nullptr) {
        Py_DECREF(on_done);
    }
    return PyLong_FromUnsignedLongLong(job);
}

PyObject *place_structure(PyObject *, PyObject *args, PyObject *kw)
{
    const char *name = nullptr;
    int x = 0, y = 0, z = 0;
    PyObject *original = Py_False;
    PyObject *dim = Py_None;
    PyObject *entities = Py_None;
    PyObject *blocks = Py_None;
    PyObject *tp_player = Py_None;
    PyObject *on_done = Py_None;
    static const char *kwlist[] = {"name",    "x",     "y",                "z",     "original", "dimension",
                                   "include_entities", "include_blocks", "tp_player", "on_done", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "siii|OOOOOO", const_cast<char **>(kwlist), &name, &x, &y, &z,
                                     &original, &dim, &entities, &blocks, &tp_player, &on_done)) {
        return nullptr;
    }
    if (api() == nullptr) {
        return apiNotReady();
    }
    const char *dim_c = nullptr;
    if (dim != Py_None) {
        dim_c = PyUnicode_AsUTF8(dim);
        if (dim_c == nullptr) {
            return nullptr;
        }
    }
    bool ent = api()->defaultEntities();
    if (entities != Py_None) {
        ent = PyObject_IsTrue(entities) == 1;
    }
    bool blk = api()->defaultBlocks();
    if (blocks != Py_None) {
        blk = PyObject_IsTrue(blocks) == 1;
    }
    const char *tp_c = nullptr;
    if (tp_player != Py_None) {
        tp_c = PyUnicode_AsUTF8(tp_player);
        if (tp_c == nullptr) {
            return nullptr;
        }
    }
    void *py_cb = nullptr;
    if (on_done != Py_None) {
        if (!PyCallable_Check(on_done)) {
            PyErr_SetString(PyExc_TypeError, "on_done must be callable or None");
            return nullptr;
        }
        Py_INCREF(on_done);  // strong ref; finishPlace() releases it after firing
        py_cb = on_done;
    }
    const std::uint64_t job = api()->placeStructurePy(name, x, y, z, PyObject_IsTrue(original) == 1, dim_c, ent,
                                                      blk, tp_c, py_cb);
    if (job == 0 && py_cb != nullptr) {
        Py_DECREF(on_done);
    }
    return PyLong_FromUnsignedLongLong(job);
}

PyMethodDef methods[] = {
    {"api_version", api_version, METH_NOARGS, "Return the C ABI version this plugin exports."},
    {"is_available", is_available, METH_NOARGS, "True if the startup compatibility probe passed."},
    {"plugin_version", plugin_version, METH_NOARGS, "Plugin version string."},
    {"stage_message", stage_message, METH_VARARGS, "stage_message(stage) -> human-readable stage name."},
    {"status", status, METH_NOARGS, "Overall status dict."},
    {"probe", probe, METH_NOARGS, "Re-run the native compatibility probe (async)."},
    {"import_structure", import_structure, METH_VARARGS, "import_structure(name_or_path) -> result dict."},
    {"dump_structure", dump_structure, METH_VARARGS, "dump_structure(name) -> result dict."},
    {"delete_structure", delete_structure, METH_VARARGS,
     "delete_structure(name=None) -> result dict; None deletes all plugin-managed structures."},
    {"list_structures", list_structures, METH_NOARGS,
     "list_structures() -> [ {key,size,loaded,sources}, ... ]. sources: subset of "
     "['library','pack','world'] (plugin exports / behavior packs / world database)."},
    {"export_region", reinterpret_cast<PyCFunction>(reinterpret_cast<void *>(export_region)),
     METH_VARARGS | METH_KEYWORDS,
     "export_region(name, x1,y1,z1, x2,y2,z2, dimension=None, include_entities=None, include_blocks=None, "
     "on_done=None) -> job id (0 = rejected). on_done(stage:int, message:str) fires on the server thread."},
    {"export_region_to", reinterpret_cast<PyCFunction>(reinterpret_cast<void *>(export_region_to)),
     METH_VARARGS | METH_KEYWORDS,
     "export_region_to(name, x1,y1,z1, x2,y2,z2, dimension=None, include_entities=None, include_blocks=None, "
     "out_dir=None, tp_player=None, force_bundle=False, on_done=None) -> job id. out_dir: custom output directory "
     "(absolute, or relative to the server root). tp_player: the ONLY player the job may teleport for chunk "
     "loading (required for non-overworld exports). force_bundle: always emit a .bundle even for small regions."},
    {"place_structure", reinterpret_cast<PyCFunction>(reinterpret_cast<void *>(place_structure)),
     METH_VARARGS | METH_KEYWORDS,
     "place_structure(name, x, y, z, original=False, dimension=None, include_entities=None, include_blocks=None, "
     "tp_player=None, on_done=None) -> job id. Places a structure or bundle; original=True restores a bundle at "
     "its exported position/dimension. tp_player: the ONLY player the job may teleport (required for "
     "non-overworld targets)."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT, "structbridge",
    "StructBridge API: engine-native .mcstructure export/import (see structbridge_api.h for the C++ twin).",
    -1, methods, nullptr, nullptr, nullptr, nullptr,
};

}  // namespace pyapi

void StructBridgePlugin::registerPythonModule()
{
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *mod = PyModule_Create(&pyapi::module_def);
    if (mod == nullptr) {
        PyErr_Print();
        PyGILState_Release(gil);
        getLogger().warning("Failed to create the 'structbridge' Python module.");
        return;
    }
    if (PyDict_SetItemString(PyImport_GetModuleDict(), "structbridge", mod) != 0) {
        PyErr_Print();
    }
    else {
        getLogger().info("Python API ready: any Python plugin can now 'import structbridge' "
                         "(API version {}).",
                         STRUCTBRIDGE_API_VERSION);
    }
    Py_DECREF(mod);
    PyGILState_Release(gil);
}
#endif  // STRUCTBRIDGE_PYTHON_API

}  // namespace structbridge

// ---------------------------------------------------------------------------
// Exported C ABI: the entry points OTHER plugins resolve with dlsym.
// ---------------------------------------------------------------------------
extern "C" STRUCTBRIDGE_EXPORT unsigned structbridge_api_version(void)
{
    return STRUCTBRIDGE_API_VERSION;
}

extern "C" STRUCTBRIDGE_EXPORT structbridge::IApi *structbridge_get_api(void)
{
    return structbridge::g_api_instance;
}

ENDSTONE_PLUGIN("structbridge", "0.1.0", structbridge::StructBridgePlugin)
{
    description = "Server-side export/import of official .mcstructure files, with a crash-safe native probe.";
    website = "https://github.com/gxh6438/structbridge";
    authors = {"gxh6438"};
    prefix = "StructBridge";
    // Commands are declared in the description so the server registers them
    // itself - the plugin never touches the (non-exported) endstone internals.
    //
    // ONE unified command. The engine-side enum gives free tab completion and
    // rejects typos before the plugin sees anything; the message tail carries
    // the subcommand's own arguments (split in onCommand).
    command("struct")
        .description("Structure toolkit: export/import/place/delete official .mcstructure files and .bundle bundles")
        .usages("/struct (export|import|place|dump|list|delete|clear|status|probe|api)[action: StructAction] "
                "[args: message]");
    // Legacy /structxxx aliases - deprecated, removed in the next release.
    command("structexport")
        .description("Deprecated: use /struct export")
        .usages("/structexport <name: string> <x1: int> <y1: int> <z1: int> <x2: int> <y2: int> <z2: int> "
                "[entities: bool] [blocks: bool]");
    command("structimport")
        .description("Deprecated: use /struct import")
        .usages("/structimport <file: message>");
    command("structdump").description("Deprecated: use /struct dump").usages("/structdump <name: message>");
    command("structdelete").description("Deprecated: use /struct delete").usages("/structdelete [name: message]");
    command("structlist").description("Deprecated: use /struct list").usages("/structlist");
    command("structprobe").description("Deprecated: use /struct probe").usages("/structprobe");
    command("structstatus").description("Deprecated: use /struct status").usages("/structstatus");
    command("structapi").description("Deprecated: use /struct api").usages("/structapi");
}
