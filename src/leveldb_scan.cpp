#include "leveldb_scan.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace structbridge::ldb {
namespace {

// ---------------------------------------------------------------------------
// CRC-32C (Castagnoli). LevelDB stores a masked CRC in every SSTable block
// trailer ([data][1-byte type][4-byte masked crc32c(data||type)]) and every
// WAL record header ([crc:4][len:2][type:1], crc = masked crc32c(type||
// fragment)). Verifying them turns "corrupted but parseable" input into a
// clean skip instead of a silently wrong result.
// ---------------------------------------------------------------------------
struct Crc32cTable {
    std::uint32_t t[256] = {};
    Crc32cTable()
    {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0x82F63B78u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
    }
};

const std::uint32_t *crc32cTable()
{
    static const Crc32cTable table;  // thread-safe magic static
    return table.t;
}

std::uint32_t crc32c(const std::uint8_t *data, std::size_t n)
{
    const std::uint32_t *t = crc32cTable();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c = t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// LevelDB's mask: rotate right 15 bits and add a constant. Applied to both
// SSTable block trailers and WAL record headers.
std::uint32_t crc32cMask(std::uint32_t crc)
{
    return ((crc >> 15) | (crc << 17)) + 0xA282EAD8u;
}

// SSTable trailer check: raw points at [data(len)][type(1)][crc(4)].
bool blockCrcOk(const std::uint8_t *raw, std::size_t len)
{
    if (len < 1) {
        return false;
    }
    std::uint32_t stored = 0;
    std::memcpy(&stored, raw + len + 1, 4);
    return crc32cMask(crc32c(raw, len + 1)) == stored;  // covers data + type byte
}

// WAL record check: header [crc:4][len:2][type:1][payload(len)].
// The stored CRC covers the type byte followed by the fragment payload.
bool recordCrcOk(const std::uint8_t *header, const std::uint8_t *payload, std::uint16_t len)
{
    std::uint32_t stored = 0;
    std::memcpy(&stored, header, 4);
    const std::uint32_t *t = crc32cTable();
    std::uint32_t c = 0xFFFFFFFFu;
    c = t[(c ^ header[6]) & 0xFFu] ^ (c >> 8);  // type byte
    for (std::uint16_t i = 0; i < len; ++i) {
        c = t[(c ^ payload[i]) & 0xFFu] ^ (c >> 8);
    }
    c ^= 0xFFFFFFFFu;
    return crc32cMask(c) == stored;
}

// ---------------------------------------------------------------------------
// Raw DEFLATE decoder (RFC 1951). "puff"-style canonical Huffman decoding -
// no tables beyond count/symbol arrays, deterministic and easy to audit.
// ---------------------------------------------------------------------------
constexpr int MAXBITS = 15;      // longest Huffman code
constexpr int MAXLCODES = 286;   // literal/length codes
constexpr int MAXDCODES = 30;    // distance codes
constexpr int MAXCODES = MAXLCODES + MAXDCODES;
constexpr int FIXLCODES = 288;   // fixed-table literal/length codes

struct BitReader {
    const std::uint8_t *data = nullptr;
    std::size_t len = 0;
    std::size_t pos = 0;
    int bitbuf = 0;
    int bitcnt = 0;
    bool ok = true;

    int bit()
    {
        if (bitcnt == 0) {
            if (pos >= len) {
                ok = false;
                return 0;
            }
            bitbuf = data[pos++];
            bitcnt = 8;
        }
        const int b = bitbuf & 1;
        bitbuf >>= 1;
        --bitcnt;
        return b;
    }

    int bits(int need)
    {
        int val = 0;
        for (int i = 0; i < need; ++i) {
            val |= bit() << i;
        }
        return val;
    }

    // Drop buffered bits to a byte boundary, then copy n raw bytes.
    bool raw(void *dst, std::size_t n)
    {
        bitbuf = 0;
        bitcnt = 0;
        if (pos + n > len) {
            ok = false;
            return false;
        }
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }
};

struct Huffman {
    short count[MAXBITS + 1] = {};
    short symbol[MAXCODES] = {};
};

// Decode one symbol; returns -1 on input exhaustion, -9 on code overflow.
int decodeSymbol(BitReader &s, const Huffman &h)
{
    int len = 1;
    int code = 0;
    int first = 0;
    int index = 0;
    while (len <= MAXBITS) {
        code |= s.bit();
        if (!s.ok) {
            return -1;
        }
        const int count = h.count[len];
        if (code - count < first) {
            return h.symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        ++len;
    }
    return -9;
}

// Build a canonical Huffman table from code lengths. Returns leftover codes
// (0 = complete, negative = oversubscribed - both usable, 0 is just ideal).
int constructHuffman(Huffman *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; ++len) {
        h->count[len] = 0;
    }
    for (int symbol = 0; symbol < n; ++symbol) {
        h->count[length[symbol]]++;
    }
    if (h->count[0] == n) {
        return 0;  // no codes at all
    }
    int left = 1;
    for (int len = 1; len <= MAXBITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return left;  // oversubscribed
        }
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; ++len) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (int symbol = 0; symbol < n; ++symbol) {
        if (length[symbol] != 0) {
            h->symbol[offs[length[symbol]]++] = static_cast<short>(symbol);
        }
    }
    return left;
}

const short LEN_BASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short DIST_BASE[30] = {1,    2,    3,    4,    5,    7,    9,    13,   17,   25,   33,   49,
                             65,   97,   129,  193,  257,  385,  513,  769,  1025, 1537, 2049, 3073,
                             4097, 6145, 8193, 12289, 16385, 24577};
const short DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4,  4,  5,  5,  6,
                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Output cap for decompression: a legitimate Bedrock world block is at most
// a few MB, so 64 MiB leaves a wide margin while bounding "decompression
// bomb" inputs (corrupted or hostile streams) to a known worst case.
constexpr std::size_t kInflateMaxOut = 64u * 1024 * 1024;

bool inflateRaw(const std::uint8_t *data, std::size_t len, std::vector<std::uint8_t> &out)
{
    // Fixed tables are cheap to build; rebuilding per call keeps this trivially
    // thread-safe.
    Huffman lencode_fixed;
    Huffman distcode_fixed;
    {
        short lengths[FIXLCODES];
        int symbol = 0;
        for (; symbol < 144; ++symbol) lengths[symbol] = 8;
        for (; symbol < 256; ++symbol) lengths[symbol] = 9;
        for (; symbol < 280; ++symbol) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; ++symbol) lengths[symbol] = 8;
        constructHuffman(&lencode_fixed, lengths, FIXLCODES);
        for (symbol = 0; symbol < MAXDCODES; ++symbol) lengths[symbol] = 5;
        constructHuffman(&distcode_fixed, lengths, MAXDCODES);
    }

    BitReader s{data, len};
    for (;;) {
        if (out.size() > kInflateMaxOut) {
            return false;  // decompression bomb / corrupted stream - refuse
        }
        const int last = s.bit();
        if (!s.ok) {
            return false;
        }
        const int type = s.bits(2);
        if (!s.ok) {
            return false;
        }

        if (type == 0) {
            // Stored block: align, LEN/NLEN, then raw bytes.
            unsigned char hdr[4];
            if (!s.raw(hdr, 4)) {
                return false;
            }
            const unsigned n = static_cast<unsigned>(hdr[0]) | (static_cast<unsigned>(hdr[1]) << 8);
            const unsigned nlen = static_cast<unsigned>(hdr[2]) | (static_cast<unsigned>(hdr[3]) << 8);
            if (nlen != (~n & 0xFFFF)) {
                return false;
            }
            if (s.pos + n > s.len) {
                return false;
            }
            out.insert(out.end(), s.data + s.pos, s.data + s.pos + n);
            s.pos += n;
        }
        else {
            Huffman lencode;
            Huffman distcode;
            if (type == 1) {
                lencode = lencode_fixed;
                distcode = distcode_fixed;
            }
            else if (type == 2) {
                short lengths[MAXCODES];
                const unsigned hlit = static_cast<unsigned>(s.bits(5)) + 257;
                const unsigned hdist = static_cast<unsigned>(s.bits(5)) + 1;
                const unsigned hclen = static_cast<unsigned>(s.bits(4)) + 4;
                if (!s.ok || hlit > MAXLCODES || hdist > MAXDCODES) {
                    return false;
                }
                static const unsigned char ORDER[19] = {16, 17, 18, 0, 8,  7, 9,  6,  10, 5,
                                                        11, 4,  12, 3, 13, 2, 14, 1, 15};
                short clen[19] = {};
                for (unsigned i = 0; i < hclen; ++i) {
                    clen[ORDER[i]] = static_cast<short>(s.bits(3));
                    if (!s.ok) {
                        return false;
                    }
                }
                Huffman clcode;
                if (constructHuffman(&clcode, clen, 19) < 0) {
                    return false;
                }
                unsigned i = 0;
                while (i < hlit + hdist) {
                    const int sym = decodeSymbol(s, clcode);
                    if (sym < 0) {
                        return false;
                    }
                    if (sym < 16) {
                        lengths[i++] = static_cast<short>(sym);
                    }
                    else {
                        int rep = 0;
                        int len = 0;
                        if (sym == 16) {
                            if (i == 0) {
                                return false;
                            }
                            len = lengths[i - 1];
                            rep = 3 + s.bits(2);
                        }
                        else if (sym == 17) {
                            rep = 3 + s.bits(3);
                        }
                        else {
                            rep = 11 + s.bits(7);
                        }
                        if (!s.ok || i + static_cast<unsigned>(rep) > hlit + hdist) {
                            return false;
                        }
                        while (rep-- > 0) {
                            lengths[i++] = static_cast<short>(len);
                        }
                    }
                }
                if (lengths[256] == 0) {
                    return false;  // no end-of-block code
                }
                if (constructHuffman(&lencode, lengths, static_cast<int>(hlit)) < 0) {
                    return false;
                }
                if (constructHuffman(&distcode, lengths + hlit, static_cast<int>(hdist)) < 0) {
                    return false;
                }
            }
            else {
                return false;  // reserved block type
            }

            for (;;) {
                int sym = decodeSymbol(s, lencode);
                if (sym < 0) {
                    return false;
                }
                if (sym < 256) {
                    out.push_back(static_cast<std::uint8_t>(sym));
                    continue;
                }
                if (sym == 256) {
                    break;  // end of block
                }
                sym -= 257;
                if (sym >= 29) {
                    return false;
                }
                const int length = LEN_BASE[sym] + s.bits(LEN_EXTRA[sym]);
                const int dsym = decodeSymbol(s, distcode);
                if (dsym < 0 || dsym >= 30) {
                    return false;
                }
                const std::uint64_t dist = static_cast<std::uint64_t>(DIST_BASE[dsym]) + s.bits(DIST_EXTRA[dsym]);
                if (!s.ok || dist == 0 || dist > out.size()) {
                    return false;
                }
                std::size_t start = out.size() - static_cast<std::size_t>(dist);
                for (int k = 0; k < length; ++k) {
                    out.push_back(out[start + static_cast<std::size_t>(k)]);
                }
                if (!s.ok) {
                    return false;
                }
            }
        }
        if (!s.ok) {
            return false;
        }
        if (last != 0) {
            return true;
        }
    }
}

// ---------------------------------------------------------------------------
// Small binary readers (little-endian).
// ---------------------------------------------------------------------------
struct Reader {
    const std::uint8_t *p = nullptr;
    std::size_t n = 0;
    std::size_t off = 0;
    bool ok = true;

    bool varint(std::uint64_t &out)
    {
        if (!ok) {
            return false;
        }
        std::uint64_t v = 0;
        int shift = 0;
        for (;;) {
            if (off >= n || shift > 63) {
                ok = false;
                return false;
            }
            const std::uint8_t b = p[off++];
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                break;
            }
            shift += 7;
        }
        out = v;
        return true;
    }

    bool fixed32(std::uint32_t &out)
    {
        if (!ok || off + 4 > n) {
            ok = false;
            return false;
        }
        out = static_cast<std::uint32_t>(p[off]) | (static_cast<std::uint32_t>(p[off + 1]) << 8) |
              (static_cast<std::uint32_t>(p[off + 2]) << 16) | (static_cast<std::uint32_t>(p[off + 3]) << 24);
        off += 4;
        return true;
    }

    bool fixed64(std::uint64_t &out)
    {
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        if (!fixed32(lo) || !fixed32(hi)) {
            return false;
        }
        out = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
        return true;
    }
};

bool readFileRange(const std::filesystem::path &file, std::uint64_t offset, std::size_t size,
                   std::vector<std::uint8_t> &out)
{
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return false;
        }
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in.good()) {
            return false;
        }
        out.resize(size);
        in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
        return in.gcount() == static_cast<std::streamsize>(size);
    }
    catch (const std::exception &) {
        // e.g. libstdc++ throws __ios_failure when the path resolves to a
        // directory (a corrupted CURRENT can produce such a path via an
        // embedded NUL). Never let a stream exception escape the scanner.
        return false;
    }
}

bool readFileAll(const std::filesystem::path &file, std::vector<std::uint8_t> &out)
{
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return false;
        }
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return !out.empty();
    }
    catch (const std::exception &) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Block decoding.
// ---------------------------------------------------------------------------
struct BlockEntry {
    std::string key;                 // full internal key (user key + 8-byte trailer)
    const std::uint8_t *value = nullptr;
    std::uint32_t value_size = 0;
};

// Decompress one block (handle-sized data + 1-byte type trailer) and split it
// into prefix-compressed entries. Returns false on any inconsistency.
bool readBlock(const std::uint8_t *data, std::size_t len, std::uint8_t compression_type,
               std::vector<std::uint8_t> &scratch, std::vector<BlockEntry> &entries)
{
    scratch.clear();
    const std::uint8_t *blk = data;
    std::size_t blk_len = len;
    if (compression_type == 0) {
        // uncompressed - use in place
    }
    else if (compression_type == 4) {
        if (!inflateRaw(data, len, scratch)) {
            return false;
        }
        blk = scratch.data();
        blk_len = scratch.size();
    }
    else {
        // Unknown codec in this BDS build (e.g. zstd): try raw deflate, then a
        // 2-byte-header zlib stream, else give up on this block.
        if (inflateRaw(data, len, scratch)) {
            blk = scratch.data();
            blk_len = scratch.size();
        }
        else if (len > 2 && inflateRaw(data + 2, len - 2, scratch)) {
            blk = scratch.data();
            blk_len = scratch.size();
        }
        else {
            return false;
        }
    }

    if (blk_len < 4) {
        return false;
    }
    const std::uint32_t num_restarts =
        static_cast<std::uint32_t>(blk[blk_len - 4]) | (static_cast<std::uint32_t>(blk[blk_len - 3]) << 8) |
        (static_cast<std::uint32_t>(blk[blk_len - 2]) << 16) | (static_cast<std::uint32_t>(blk[blk_len - 1]) << 24);
    if (num_restarts == 0 || blk_len < 4 + 4 * static_cast<std::size_t>(num_restarts)) {
        return false;
    }
    const std::size_t restarts_off = blk_len - 4 - 4 * static_cast<std::size_t>(num_restarts);

    entries.clear();
    Reader r{blk, restarts_off};
    std::string prev;
    while (r.off < restarts_off && r.ok) {
        std::uint64_t shared = 0;
        std::uint64_t non_shared = 0;
        std::uint64_t value_len = 0;
        if (!r.varint(shared) || !r.varint(non_shared) || !r.varint(value_len)) {
            return false;
        }
        if (shared > prev.size() || r.off + non_shared + value_len > restarts_off) {
            return false;
        }
        BlockEntry e;
        e.key.assign(prev, 0, static_cast<std::size_t>(shared));
        e.key.append(reinterpret_cast<const char *>(blk + r.off), static_cast<std::size_t>(non_shared));
        r.off += non_shared;
        e.value = blk + r.off;
        e.value_size = static_cast<std::uint32_t>(value_len);
        r.off += value_len;
        prev = e.key;
        if (e.key.size() < 8) {
            return false;  // internal keys always carry the 8-byte trailer
        }
        entries.push_back(std::move(e));
    }
    return r.ok && r.off == restarts_off;
}

// ---------------------------------------------------------------------------
// Merge state: user key -> highest-sequence write. A key exists iff the
// winning entry is a put.
// ---------------------------------------------------------------------------
struct KeyState {
    std::uint64_t seq = 0;
    bool put = false;
    std::uint32_t value_size = 0;
};

void noteEntry(std::map<std::string, KeyState> &merged, const std::string &internal_key, std::uint32_t value_size)
{
    if (internal_key.size() < 8) {
        return;
    }
    std::uint64_t trailer = 0;
    {
        Reader r{reinterpret_cast<const std::uint8_t *>(internal_key.data()) + internal_key.size() - 8, 8};
        r.fixed64(trailer);
    }
    const std::uint64_t seq = trailer >> 8;
    const bool is_put = (trailer & 0xFF) == 1;  // 0 = deletion
    const std::string user_key = internal_key.substr(0, internal_key.size() - 8);
    auto it = merged.find(user_key);
    if (it == merged.end()) {
        merged.emplace(user_key, KeyState{seq, is_put, value_size});
    }
    else if (it->second.seq < seq) {
        it->second.seq = seq;
        it->second.put = is_put;
        it->second.value_size = value_size;
    }
}

// ---------------------------------------------------------------------------
// SSTable scan (index-guided: only data blocks that can contain the prefix
// are decompressed).
// ---------------------------------------------------------------------------
bool scanTable(const std::filesystem::path &file, const std::string &prefix,
               std::map<std::string, KeyState> &merged)
{
    std::uint64_t file_size = 0;
    try {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (!in) {
            return false;
        }
        file_size = static_cast<std::uint64_t>(in.tellg());
    }
    catch (const std::exception &) {
        return false;
    }
    if (file_size < 48) {
        return false;
    }

    std::vector<std::uint8_t> footer;
    if (!readFileRange(file, static_cast<std::uint64_t>(file_size) - 48, 48, footer)) {
        return false;
    }
    // Footer: metaindex handle, index handle, padding, 8-byte magic.
    Reader fr{footer.data(), footer.size() - 8};
    std::uint64_t m_off = 0, m_sz = 0, i_off = 0, i_sz = 0;
    if (!fr.varint(m_off) || !fr.varint(m_sz) || !fr.varint(i_off) || !fr.varint(i_sz)) {
        return false;
    }
    if (i_off + i_sz + 5 > static_cast<std::uint64_t>(file_size)) {
        return false;
    }

    std::vector<std::uint8_t> index_raw;
    if (!readFileRange(file, i_off, static_cast<std::size_t>(i_sz) + 5, index_raw)) {
        return false;
    }
    if (!blockCrcOk(index_raw.data(), static_cast<std::size_t>(i_sz))) {
        return false;  // corrupted index block - cannot trust this table
    }
    std::vector<std::uint8_t> scratch;
    std::vector<BlockEntry> index_entries;
    if (!readBlock(index_raw.data(), static_cast<std::size_t>(i_sz), index_raw[i_sz], scratch, index_entries) ||
        index_entries.empty()) {
        return false;
    }

    // Walk data blocks in key order; skip blocks entirely before the prefix,
    // stop once a block's first key has passed the prefix range.
    bool past_prefix = false;
    for (const auto &idx : index_entries) {
        if (past_prefix) {
            break;
        }
        const std::string idx_user = idx.key.substr(0, idx.key.size() - 8);
        if (idx_user.compare(prefix) < 0) {
            continue;  // whole block sorts before the prefix
        }
        Reader hr{idx.value, idx.value_size};
        std::uint64_t b_off = 0, b_sz = 0;
        if (!hr.varint(b_off) || !hr.varint(b_sz)) {
            return false;
        }
        if (b_off + b_sz + 5 > static_cast<std::uint64_t>(file_size)) {
            return false;
        }
        std::vector<std::uint8_t> block_raw;
        if (!readFileRange(file, b_off, static_cast<std::size_t>(b_sz) + 5, block_raw)) {
            return false;
        }
        if (!blockCrcOk(block_raw.data(), static_cast<std::size_t>(b_sz))) {
            continue;  // corrupted block - fail soft
        }
        std::vector<std::uint8_t> block_scratch;
        std::vector<BlockEntry> block_entries;
        if (!readBlock(block_raw.data(), static_cast<std::size_t>(b_sz), block_raw[b_sz], block_scratch,
                       block_entries)) {
            continue;  // undecodable block (unknown codec) - fail soft
        }
        for (const auto &e : block_entries) {
            const std::string user_key = e.key.substr(0, e.key.size() - 8);
            if (user_key.compare(prefix) < 0) {
                continue;
            }
            if (user_key.size() < prefix.size() ||
                user_key.compare(0, prefix.size(), prefix) != 0) {
                past_prefix = true;  // sorted order: no further prefix keys
                break;
            }
            noteEntry(merged, e.key, e.value_size);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Log-format files (WAL and MANIFEST share the record framing).
// ---------------------------------------------------------------------------
// Reads log records, calling handle(full_payload) for each complete record.
// Returns false if the file could not be opened at all.
bool forEachLogRecord(const std::vector<std::uint8_t> &data,
                      const std::function<void(const std::uint8_t *, std::size_t)> &handle)
{
    constexpr std::size_t BLOCK = 32768;
    std::vector<std::uint8_t> pending;
    std::size_t pos = 0;
    while (pos + 7 <= data.size()) {
        std::uint32_t crc = 0;
        std::uint16_t length = 0;
        std::uint8_t type = 0;
        std::memcpy(&crc, data.data() + pos, 4);
        std::memcpy(&length, data.data() + pos + 4, 2);
        type = data[pos + 6];
        if (length == 0 || pos + 7 + length > data.size()) {
            break;  // truncated tail (mid-write) - ignore
        }
        // CRC guard: a record whose CRC does not verify is either mid-write
        // or corrupted. Dropping it (and any pending FIRST/MIDDLE chain)
        // prevents silently misparsed batches from reaching the merge map.
        if (!recordCrcOk(data.data() + pos, data.data() + pos + 7, length)) {
            pending.clear();
            pos += BLOCK - (pos % BLOCK);  // resync at next block boundary
            continue;
        }
        pos += 7;
        const std::uint8_t *payload = data.data() + pos;
        pos += length;
        switch (type) {
            case 1:  // FULL
                if (!pending.empty()) {
                    pending.clear();
                }
                handle(payload, length);
                break;
            case 2:  // FIRST
                pending.assign(payload, payload + length);
                break;
            case 3:  // MIDDLE
                pending.insert(pending.end(), payload, payload + length);
                break;
            case 4:  // LAST
                pending.insert(pending.end(), payload, payload + length);
                if (!pending.empty()) {
                    handle(pending.data(), pending.size());
                }
                pending.clear();
                break;
            default:
                pending.clear();
                break;
        }
        if ((pos % BLOCK) + 7 > BLOCK && pos < data.size()) {
            pos += BLOCK - (pos % BLOCK);  // records never span block borders
        }
    }
    return true;
}

// Apply a WriteBatch payload to the merge map. seq_offset = the batch's base
// sequence number; each entry consumes one sequence.
void applyWriteBatch(const std::uint8_t *p, std::size_t n, const std::string &prefix,
                     std::map<std::string, KeyState> &merged)
{
    if (n < 12) {
        return;
    }
    std::uint64_t seq = 0;
    std::uint32_t count = 0;
    {
        Reader r{p, 12};
        r.fixed64(seq);
        r.fixed32(count);
    }
    Reader r{p + 12, n - 12};
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!r.ok) {
            return;
        }
        const std::uint8_t type = r.p[r.off++];
        std::uint64_t klen = 0;
        if (!r.varint(klen) || r.off + klen > r.n) {
            return;
        }
        const std::string key(reinterpret_cast<const char *>(r.p + r.off), static_cast<std::size_t>(klen));
        r.off += klen;
        std::uint64_t vlen = 0;
        if (type == 0) {
            vlen = 0;  // deletion: no value bytes
        }
        else {
            if (!r.varint(vlen) || r.off + vlen > r.n) {
                return;
            }
            r.off += vlen;
        }
        if (key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0) {
            auto it = merged.find(key);
            const bool put = type != 0;
            if (it == merged.end()) {
                merged.emplace(key, KeyState{seq + i, put, static_cast<std::uint32_t>(vlen)});
            }
            else if (it->second.seq < seq + i) {
                it->second.seq = seq + i;
                it->second.put = put;
                it->second.value_size = static_cast<std::uint32_t>(vlen);
            }
        }
    }
}

// Shared tail of scanStructures: report keys whose latest write is a put.
std::vector<WorldStructure> finalizeMerged(const std::map<std::string, KeyState> &merged,
                                           const std::string &prefix)
{
    std::vector<WorldStructure> result;
    for (const auto &kv : merged) {
        if (!kv.second.put) {
            continue;
        }
        WorldStructure ws;
        ws.key = kv.first.substr(prefix.size());
        ws.size = kv.second.value_size;
        if (!ws.key.empty()) {
            result.push_back(std::move(ws));
        }
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry point.
// ---------------------------------------------------------------------------
std::vector<WorldStructure> scanStructures(const std::filesystem::path &db_dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    // Total time fuse: this runs on the server thread against a database the
    // live server owns. A healthy world scans in well under a second; if we
    // ever blow past the budget (pathological FS, hostile input) stop early
    // with whatever was collected instead of stalling the tick loop.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    auto outOfTime = [&deadline] { return std::chrono::steady_clock::now() > deadline; };
    if (!fs::exists(db_dir, ec) || !fs::is_directory(db_dir, ec)) {
        return {};
    }
    const std::string prefix = "structuretemplate_";
    std::map<std::string, KeyState> merged;

    // ---- 1) Live table files + current log number from the MANIFEST ----
    std::set<std::uint64_t> live_tables;
    bool manifest_ok = false;
    {
        std::vector<std::uint8_t> current;
        if (readFileAll(db_dir / "CURRENT", current)) {
            std::string manifest_name(current.begin(), current.end());
            while (!manifest_name.empty() &&
                   (manifest_name.back() == '\n' || manifest_name.back() == '\r' || manifest_name.back() == ' ')) {
                manifest_name.pop_back();
            }
            std::vector<std::uint8_t> manifest;
            if (!manifest_name.empty() && readFileAll(db_dir / manifest_name, manifest)) {
                std::set<std::uint64_t> added;
                std::set<std::uint64_t> deleted;
                bool parse_ok = true;
                forEachLogRecord(manifest, [&](const std::uint8_t *p, std::size_t n) {
                    if (!parse_ok) {
                        return;
                    }
                    Reader r{p, n};
                    while (r.off < r.n && r.ok) {
                        const std::uint8_t tag = r.p[r.off++];
                        switch (tag) {
                            case 1: {  // comparator (length-prefixed string)
                                std::uint64_t l = 0;
                                if (!r.varint(l) || r.off + l > r.n) {
                                    parse_ok = false;
                                    return;
                                }
                                r.off += l;
                                break;
                            }
                            case 2:   // log number
                            case 3:   // next file number
                            case 4:   // last sequence
                            case 9: {  // prev log number
                                std::uint64_t v = 0;
                                if (!r.varint(v)) {
                                    parse_ok = false;
                                    return;
                                }
                                break;
                            }
                            case 5: {  // compact pointer: level + ikey
                                std::uint64_t level = 0;
                                std::uint64_t l = 0;
                                if (!r.varint(level) || !r.varint(l) || r.off + l > r.n) {
                                    parse_ok = false;
                                    return;
                                }
                                r.off += l;
                                break;
                            }
                            case 6: {  // deleted file: level + file number
                                std::uint64_t level = 0;
                                std::uint64_t num = 0;
                                if (!r.varint(level) || !r.varint(num)) {
                                    parse_ok = false;
                                    return;
                                }
                                deleted.insert(num);
                                break;
                            }
                            case 7: {  // new file: level, number, size, smallest, largest
                                std::uint64_t level = 0;
                                std::uint64_t num = 0;
                                std::uint64_t size = 0;
                                std::uint64_t l = 0;
                                if (!r.varint(level) || !r.varint(num) || !r.varint(size) || !r.varint(l) ||
                                    r.off + l > r.n) {
                                    parse_ok = false;
                                    return;
                                }
                                r.off += l;
                                if (!r.varint(l) || r.off + l > r.n) {
                                    parse_ok = false;
                                    return;
                                }
                                r.off += l;
                                added.insert(num);
                                break;
                            }
                            default:
                                parse_ok = false;
                                return;
                        }
                    }
                    if (r.off != r.n) {
                        parse_ok = false;
                    }
                });
                if (parse_ok) {
                    for (auto num : added) {
                        if (deleted.find(num) == deleted.end()) {
                            live_tables.insert(num);
                        }
                    }
                    manifest_ok = true;
                }
            }
        }
    }

    // ---- 2) Scan live SSTables (or every table if the MANIFEST was unusable) ----
    if (manifest_ok) {
        for (auto num : live_tables) {
            if (outOfTime()) {
                return finalizeMerged(merged, prefix);
            }
            char name[32];
            std::snprintf(name, sizeof(name), "%06llu.ldb", static_cast<unsigned long long>(num));
            if (fs::exists(db_dir / name, ec)) {
                scanTable(db_dir / name, prefix, merged);
                continue;
            }
            std::snprintf(name, sizeof(name), "%06llu.sst", static_cast<unsigned long long>(num));
            if (fs::exists(db_dir / name, ec)) {
                scanTable(db_dir / name, prefix, merged);
            }
        }
    }
    else {
        for (fs::directory_iterator it(db_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (outOfTime()) {
                return finalizeMerged(merged, prefix);
            }
            const auto ext = it->path().extension().string();
            if (ext == ".ldb" || ext == ".sst") {
                scanTable(it->path(), prefix, merged);
            }
        }
    }

    // ---- 3) Apply the write-ahead logs (all of them; sequence numbers dedupe) ----
    {
        for (fs::directory_iterator it(db_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (outOfTime()) {
                return finalizeMerged(merged, prefix);
            }
            if (it->path().extension().string() != ".log") {
                continue;
            }
            std::vector<std::uint8_t> data;
            if (!readFileAll(it->path(), data)) {
                continue;
            }
            forEachLogRecord(data, [&](const std::uint8_t *p, std::size_t n) {
                applyWriteBatch(p, n, prefix, merged);
            });
        }
    }

    // ---- 4) Report keys whose latest write is a put ----
    return finalizeMerged(merged, prefix);
}

}  // namespace structbridge::ldb
