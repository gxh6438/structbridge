// Read-only scanner for the Bedrock world LevelDB (Mojang's leveldb fork).
//
// Purpose: enumerate every `structuretemplate_<key>` entry that currently
// exists in the world database, so /struct list can show structures that were
// saved in-game but never touched by this plugin. The engine offers no
// enumeration API, so the database files are parsed directly.
//
// On-disk formats:
//   - SSTables (*.ldb): standard leveldb layout - 48-byte footer (varint
//     BlockHandles + magic 0xdb4775248b80fb57), index block, prefix-compressed
//     data blocks with restart arrays, internal keys (user key + 8-byte
//     sequence|type trailer).
//   - Block trailer: [1 byte compression type][4 bytes crc32c]. Compression
//     type 4 in Mojang's fork is raw DEFLATE (RFC 1951, no zlib header);
//     type 0 is uncompressed. Undecodable types skip the block (fail-soft).
//   - WAL (*.log): 32 KiB blocks of records ([crc32c:4][len:2][type:1][payload]),
//     records may span blocks (FIRST/MIDDLE/LAST). Payloads are WriteBatches:
//     [seq:8][count:4] then entries. Entry format: type byte (1=put, 0=delete),
//     varint key length, key, and for puts varint value length + value
//     (deletions carry no value).
//   - MANIFEST: same log record format; VersionEdit tags 1=comparator,
//     2=log number, 3=next file, 4=last sequence, 5=compact pointer,
//     6=deleted file, 7=new file, 9=prev log number. Live tables and the
//     current WAL are derived from it; on any parse failure the scanner
//     falls back to reading every *.ldb/*.sst/*.log file present (worst
//     case: a few stale keys from files awaiting compaction).
//
// Everything is read-only and fail-soft: the database is owned by the running
// server and may be mid-write; any inconsistency just skips the affected file
// or block. Keys are merged by sequence number (highest write wins; a key
// exists iff its latest entry is a put).
//
// Self-contained: only the C++ standard library is required; the built-in
// RFC 1951 inflate implementation is the only decompressor needed.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace structbridge::ldb {

// One structure found in the world database.
struct WorldStructure {
    std::string key;      // engine key with the "structuretemplate_" prefix stripped
    std::uint64_t size;   // size of the stored value (the raw NBT payload), bytes
};

// Scan a world's `db` directory for existing structuretemplate_ entries.
// Returns them sorted by key. Never throws; unreadable/unparseable files are
// skipped silently (an empty result usually means "no structures" or "db not
// found" - callers treat both the same way).
std::vector<WorldStructure> scanStructures(const std::filesystem::path &db_dir);

}  // namespace structbridge::ldb
