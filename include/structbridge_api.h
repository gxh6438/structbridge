// structbridge public API - for other Endstone plugins (C++ and Python).
//
// The interface crosses the boundary with POD/C types only, so there is no
// STL ABI dependency (works with libc++ and libstdc++ toolchains alike).
//
// C++ usage:
//   1. Resolve the loaded structbridge library. Endstone loads C++ plugins
//      from plugins/.local/<name>-<hash>.so|dll - consumers must open THAT
//      copy, not the original under plugins/, or a second instance of the
//      library would be mapped.
//   2. dlopen()/LoadLibraryW() it, resolve the factory export
//        structbridge_get_api      -> structbridge::IApi*
//      and obtain the interface.
//
// Python usage (any Python plugin, in onEnable or later):
//
//   import structbridge
//   if structbridge.is_available():
//       result = structbridge.import_structure("myhouse")   # {"ok", "stage", "message"}
//       structbridge.export_region("house", 0,63,0, 7,70,7,
//                                  on_done=lambda stage, msg: print(stage, msg))
//       for item in structbridge.list_structures():
//           print(item["key"], item["size"], item["loaded"], item["sources"])
//
// Stage codes are shared with the /struct status diagnostics; human-readable
// names via stageMessage() (C++) / stage_message() (Python).

#pragma once

#include <cstdint>

#if defined(_WIN32)
#if defined(STRUCTBRIDGE_BUILDING)
#define STRUCTBRIDGE_EXPORT __declspec(dllexport)
#else
#define STRUCTBRIDGE_EXPORT __declspec(dllimport)
#endif
#else
#define STRUCTBRIDGE_EXPORT __attribute__((visibility("default")))
#endif

namespace structbridge {

class IApi {
public:
    virtual ~IApi() = default;

    // True when the plugin is loaded and operational.
    [[nodiscard]] virtual bool isAvailable() const = 0;
    // Plugin version string, e.g. "0.0.1".
    [[nodiscard]] virtual const char* version() const = 0;
    // Human-readable stage name for a stage code.
    [[nodiscard]] virtual const char* stageMessage(int stage) const = 0;

    // Export a world region as .mcstructure (engine-native serialization,
    // identical to a structure block export). Asynchronous: chunks are
    // force-loaded via tickingareas and the vanilla save command is retried
    // on a timer. Returns a job id > 0, or 0 on immediate rejection
    // (bad name / oversized region). on_done fires exactly once, on the
    // server thread; stage == 0 means success and message is the written
    // file path.
    //
    // out_dir: nullptr/"" = the plugin library; otherwise a custom directory
    // (absolute, or relative to the server root) that receives the
    // .mcstructure - or, for bundles, a <out_dir>/<name>.bundle directory
    // with tiles + manifest.json.
    //
    // tp_player: the ONLY player this job may teleport to help load chunks
    // (tickingarea budget shortfall or non-overworld exports, which need a
    // player origin). REQUIRED for non-overworld dimensions (nullptr is
    // rejected there). Never a random player - whoever the caller names.
    // The player is teleported back to their original position when the
    // job finishes.
    //
    // force_bundle: also tile small regions into a .bundle (manifest.json
    // records the original region + dimension). Regions larger than 64x64 in
    // X/Z are ALWAYS bundles (the engine's per-template limit is 64x384x64);
    // Y spans over 384 are rejected.
    virtual std::uint64_t exportRegion(const char* name, int x1, int y1, int z1, int x2, int y2, int z2,
                                       const char* dimension, bool include_entities, bool include_blocks,
                                       const char* out_dir, const char* tp_player, bool force_bundle,
                                       void (*on_done)(void* ctx, int stage, const char* message), void* ctx) = 0;

    // Import a .mcstructure into the engine (by library name or file path).
    // The structure is immediately loadable and persisted for restarts.
    // Returns a stage code; 0 = success. msg receives a human-readable detail.
    virtual int importStructure(const char* name_or_path, char* msg, unsigned long msg_cap) = 0;

    // Dump an ALREADY-SAVED structure (structure block / /structure save)
    // to a .mcstructure file, by name. Returns a stage code; 0 = success.
    virtual int dumpStructure(const char* name, char* msg, unsigned long msg_cap) = 0;

    // Delete a structure everywhere (engine memory + world database + plugin
    // files). name == nullptr deletes ALL plugin-managed structures.
    // Returns a stage code; 0 = success.
    virtual int deleteStructure(const char* name_or_null, char* msg, unsigned long msg_cap) = 0;

    // List structures visible on this server: the plugin's exports library,
    // every behavior pack's structures/ dir and the world database (LevelDB -
    // structure block / "/structure save ... disk" entries, non-ASCII names
    // included), merged and deduplicated by engine key. on_item fires once
    // per structure on the calling thread. Returns the item count.
    virtual int listStructures(void (*on_item)(void* ctx, const char* key, std::uint64_t size_bytes,
                                                int loaded_in_engine),
                               void* ctx) = 0;

    // Place a structure (or a whole bundle) into the world at (x,y,z) - the
    // missing half of the roundtrip. Chunks at the TARGET are loaded the
    // same way exports do (tickingareas + tp_player assist, teleport-back
    // on completion).
    //
    // name: a structure name, a library bundle name, or a .bundle DIRECTORY
    // path (relative to the server root, or absolute) - the latter covers
    // bundles exported to a custom out_dir through exportRegion.
    //
    // original=true restores a BUNDLE at its exported position/dimension
    // (cross-server building migration); single structures reject it (a
    // bare .mcstructure carries no world position).
    //
    // dimension: "overworld" (default) / "nether" / "the_end".
    // tp_player: as in exportRegion - REQUIRED for non-overworld targets.
    //
    // The structure must exist in the engine memory, the plugin library, a
    // behavior pack or the world database; bundles are looked up in the
    // library. Returns a job id > 0, or 0 on immediate rejection.
    virtual std::uint64_t placeStructure(const char* name, int x, int y, int z, bool original,
                                         const char* dimension, bool include_entities, bool include_blocks,
                                         const char* tp_player,
                                         void (*on_done)(void* ctx, int stage, const char* message), void* ctx) = 0;
};

}  // namespace structbridge

extern "C" {
typedef structbridge::IApi* (*structbridge_get_api_fn)();
STRUCTBRIDGE_EXPORT structbridge::IApi* structbridge_get_api(void);
}
