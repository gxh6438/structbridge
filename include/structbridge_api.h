// structbridge public API - for OTHER Endstone plugins (C++ and Python).
//
// C++ usage (works with libc++ AND libstdc++ toolchains - the interface only
// crosses the boundary with POD/C types, so there is no STL ABI dependency):
//
//   #include "structbridge_api.h"
//   #include <dlfcn.h>
//   #include <filesystem>
//   #include <glob.h>   // or std::filesystem iteration over plugins/.local
//
//   // Endstone loads C++ plugins from plugins/.local/<name>-<hash>.so.
//   // Resolve THAT copy (dlopen-ing the original plugins/*.so instead would
//   // map a second instance of the library with its own null g_api!).
//   std::string find_structbridge_lib(const char* plugins_dir) {
//       std::string pat = std::string(plugins_dir) + "/.local/endstone_structbridge-*.so";
//       glob_t g;
//       if (glob(pat.c_str(), 0, nullptr, &g) == 0 && g.gl_pathc > 0) {
//           std::string path = g.gl_pathv[0];
//           globfree(&g);
//           return path;
//       }
//       globfree(&g);
//       return std::string(plugins_dir) + "/endstone_structbridge.so";  // fallback
//   }
//
//   void* h = dlopen(find_structbridge_lib("plugins").c_str(), RTLD_NOW);
//   if (!h) { /* structbridge is not installed */ }
//   auto ver = (structbridge_api_version_fn)dlsym(h, "structbridge_api_version");
//   auto get = (structbridge_get_api_fn)dlsym(h, "structbridge_get_api");
//   // Version check: v2 APPENDED methods after the v1 vtable prefix, so a
//   // server exporting v2 still serves v1-compiled callers; check
//   // "ver() >= STRUCTBRIDGE_API_VERSION you compiled with" (and guard the
//   // v2-only methods with ver() >= 2).
//   if (!get || ver() < STRUCTBRIDGE_API_VERSION) { /* version mismatch */ }
//   structbridge::IApi* api = (structbridge::IApi*)get();
//   if (!api->isAvailable()) { /* probe failed on this server build */ }
//
//   // synchronous import (returns a stage code; 0 = success)
//   char msg[256];
//   int stage = api->importStructure("myhouse", msg, sizeof(msg));
//
//   // asynchronous export (callback fires on the server thread when done)
//   api->exportRegion("myhouse", 100,60,100, 110,70,110, nullptr, true, true,
//                     [](void* ctx, int stage, const char* msg) {
//                         // stage == 0 -> msg is the exported file path
//                     }, nullptr);
//
//   // v2: dimensioned export with a custom output dir (the named player is
//   // the ONLY one ever teleported) and bundle-aware placement
//   api->exportRegionTo("castle", 0,64,0, 1023,120,1023, "overworld", true, true,
//                       "backups/october", "Steve", true, on_done, nullptr);
//   api->placeStructure("castle", 0, 64, 0, true, nullptr, true, true, "Steve",
//                       on_done, nullptr);  // original position + dimension
//
// Python usage (any Python plugin, in onEnable or later):
//
//   import structbridge
//   if structbridge.is_available():
//       result = structbridge.import_structure("myhouse")   # {"ok": ..., "stage": ..., "message": ...}
//       structbridge.export_region("house", 0,63,0, 7,70,7,
//                                  on_done=lambda stage, msg: print(stage, msg))
//       for item in structbridge.list_structures():
//           print(item["key"], item["size"], item["loaded"], item["sources"])
//
// Stage codes are shared with the /structstatus diagnostics; human-readable
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

#define STRUCTBRIDGE_API_VERSION 2u

namespace structbridge {

class IApi {
public:
    virtual ~IApi() = default;

    // Matches STRUCTBRIDGE_API_VERSION the plugin was built with.
    [[nodiscard]] virtual std::uint32_t apiVersion() const = 0;
    // True once the startup compatibility probe passed (fork-sandbox roundtrip
    // of the full native chain on this server build).
    [[nodiscard]] virtual bool isAvailable() const = 0;
    // Plugin version string, e.g. "0.1.0".
    [[nodiscard]] virtual const char* version() const = 0;
    // Human-readable stage name for a stage code.
    [[nodiscard]] virtual const char* stageMessage(int stage) const = 0;

    // ---- v1 surface (unchanged; v1-compiled consumers keep working) ----

    // Export a world region as .mcstructure (engine-native serialization,
    // identical to a structure block export). Asynchronous: chunks are
    // force-loaded via tickingareas and the vanilla save command is retried
    // on a timer. Returns a job id > 0, or 0 on immediate rejection
    // (bad name / oversized region / gate closed). on_done fires exactly
    // once, on the server thread; stage == 0 means success and message is
    // the written file path.
    //
    // dimension: v1 supports "overworld" (or nullptr). Player-dimension
    // exports stay available through the /struct export command.
    virtual std::uint64_t exportRegion(const char* name, int x1, int y1, int z1, int x2, int y2, int z2,
                                       const char* dimension, bool include_entities, bool include_blocks,
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

    // ---- v2 surface (APPENDED: the vtable prefix is unchanged, so v1
    // consumers remain binary compatible) ----

    // v2 export: adds a custom output directory, an explicit TP player and
    // bundle control.
    //
    // out_dir: nullptr/"" = the plugin library (like v1 exportRegion);
    // otherwise a custom directory (absolute, or relative to the server
    // root) that receives the .mcstructure - or, for bundles, a
    // <out_dir>/<name>.bundle directory with tiles + manifest.json.
    //
    // tp_player: the ONLY player this job may teleport to help load chunks
    // (tickingarea budget shortfall or nether/end exports, which need a
    // player origin). REQUIRED for non-overworld dimensions (nullptr is
    // rejected there). Never a random player - whoever the caller names.
    //
    // force_bundle: also tile small regions into a .bundle (manifest.json
    // records the original region + dimension). Regions larger than 64x64 in
    // X/Z are ALWAYS bundles (the engine's per-template limit is 64x384x64);
    // Y spans over 384 are rejected.
    virtual std::uint64_t exportRegionTo(const char* name, int x1, int y1, int z1, int x2, int y2, int z2,
                                         const char* dimension, bool include_entities, bool include_blocks,
                                         const char* out_dir, const char* tp_player, bool force_bundle,
                                         void (*on_done)(void* ctx, int stage, const char* message), void* ctx) = 0;

    // v2 place: load a structure (or a whole bundle) into the world at (x,y,z)
    // - the missing half of the roundtrip. Chunks at the TARGET are loaded
    // the same way exports do (tickingareas + tp_player assist).
    //
    // name: a structure name, a library bundle name, or a .bundle DIRECTORY
    // path (relative to the server root, or absolute) - the latter covers
    // bundles exported to a custom out_dir through exportRegionTo.
    //
    // original=true restores a BUNDLE at its exported position/dimension
    // (cross-server building migration); single structures reject it (a
    // bare .mcstructure carries no world position).
    //
    // dimension: "overworld" (default) / "nether" / "the_end".
    // tp_player: as in exportRegionTo - REQUIRED for non-overworld targets.
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
typedef unsigned (*structbridge_api_version_fn)();
STRUCTBRIDGE_EXPORT unsigned structbridge_api_version(void);
STRUCTBRIDGE_EXPORT structbridge::IApi* structbridge_get_api(void);
}
