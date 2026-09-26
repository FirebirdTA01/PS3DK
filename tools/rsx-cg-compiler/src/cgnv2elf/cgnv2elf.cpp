/*
 * cgnv2elf -- pack compiled RSX shader containers (.vpo / .fpo, the
 * CgBinaryProgram layout rsx-cg-compiler writes with --emit-container)
 * into one big-endian ELF32 relocatable shader archive.
 *
 * Usage:
 *   cgnv2elf [-qsuehv][--quiet][--no-sem][--no-unref][--help] input [output]
 *
 * `input` is one container or a folder; a folder converts every regular
 * file in it (case-insensitive name order), silently skipping files that
 * are not shader containers.  Without `output` the archive is written as
 * `<dir of input>/out<extension of input>`.
 *
 * Archive layout (all values big-endian):
 *
 *   ELF header      ELFCLASS32, ELFDATA2MSB, EI_OSABI 0x13, EI_ABIVERSION 1,
 *                   ET_REL, e_machine 0x528e, section headers at 0x34.
 *   [0]  null
 *   [1]  .shstrtab
 *   [2]  .note        one note, name "SCE cgnv2elf", desc = u32 revision
 *   [3]  .strtab      parameter names, semantics and symbol names
 *   [4]  .const       16-byte default-value vectors, appended per program
 *   [5]  .symtab      one symbol per program, named after the input file
 *   [6]  .shadertab   one 0x1c-byte entry per program (profile + header)
 *   [7+2i] .text%04d      program i's ucode, verbatim
 *   [8+2i] .paramtab%04d  program i's parameter table
 *
 * Section data follows the section header table in section order, each
 * section aligned to its sh_addralign; the file ends at the last byte of
 * the last non-empty section.
 *
 * The parameter table rebuilds the parameter tree from the container's
 * flat, composed parameter names (`light.color`, `k[1][2]`, `m[0]` for a
 * matrix row) and emits one record per tree node, a type table, a
 * resource-index table, a default table and a semantic table.
 *
 * -u / --no-unref (drop unreferenced parameters) is refused: the archive
 * layout it must produce is not specified precisely enough to reproduce.
 */

#include "version.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Bytes = std::string;

// A container that looks like a shader container but cannot be converted
// (fields out of range, offsets past the end of the file).
struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string budget_message(const char* budget, const std::string& detail, uint64_t limit) {
    return std::string("input budget exceeded: ") + budget + ": " + detail +
           " (limit " + std::to_string(limit) + ")";
}

[[noreturn]] void over_budget(const char* budget, const std::string& detail, uint64_t limit) {
    throw FormatError(budget_message(budget, detail, limit));
}

constexpr uint32_t kProfileVertex      = 7003;
constexpr uint32_t kProfileFragment    = 7004;
constexpr uint32_t kContainerRevision  = 6;
constexpr uint32_t kToolRevision       = 6365;   // .note descriptor

// Input budgets.  Each sits far above anything a real shader produces (SDK
// sample and rsx-cg-compiler containers hold at most a few hundred
// parameters, with names well under 100 bytes, a handful of components and
// a few embedded constants each) and is checked before the work it bounds,
// so hostile input is refused promptly with a clear error instead of
// exhausting memory or time.  Every refusal reads
//   input budget exceeded: <budget>: <detail> (limit <n>)
// with <budget> one of the names below.
constexpr uint64_t kMaxInputBytes   = uint64_t(16) << 20;    // file-size: one container file
constexpr uint64_t kMaxArchiveBytes = uint64_t(256) << 20;   // archive-size: all inputs together
constexpr size_t   kMaxPrograms     = 4096;   // program-count: programs per archive
constexpr uint32_t kMaxParams       = 8192;   // parameter-count: parameters per program
constexpr size_t   kMaxNameBytes    = 1024;   // name-length: one parameter name or semantic
constexpr size_t   kMaxPathDepth    = 32;     // name-depth: name components plus array indices
// embedded-constants: offsets across all parameters of one program, counted
// per reference (parameters that share one list each count it in full).
constexpr uint64_t kMaxEmbeddedConstants = 65536;
constexpr uint64_t kMaxOutputBytes  = uint64_t(512) << 20;   // output-size: the archive written

constexpr uint32_t kVarVarying  = 0x1005;
constexpr uint32_t kVarUniform  = 0x1006;
constexpr uint32_t kVarConstant = 0x1007;
constexpr uint32_t kDirOut      = 0x1002;
constexpr uint32_t kDirInOut    = 0x1003;
constexpr uint32_t kParamnoGlobal   = 0xffffffffu;
constexpr uint32_t kParamnoInternal = 0xfffffffeu;

// Record flags.
constexpr uint32_t kFlagReferenced  = 0x0010;
constexpr uint32_t kFlagShared      = 0x0020;
constexpr uint32_t kFlagGlobal      = 0x0040;
constexpr uint32_t kFlagInternal    = 0x0080;
constexpr uint32_t kFlagStruct      = 0x0100;
constexpr uint32_t kFlagArray       = 0x0200;
constexpr uint32_t kFlagStructArray = 0x0400;
constexpr uint32_t kFlagNode        = 0x1000;
constexpr uint32_t kFlagColorNormal = 0x2000;

bool is_sampler_type(uint32_t t) { return t >= 0x429 && t <= 0x42d; }

// ---------------------------------------------------------------------------
// Big-endian byte helpers.
// ---------------------------------------------------------------------------

void put8(Bytes& b, uint32_t v) { b.push_back(static_cast<char>(v & 0xff)); }

void put16(Bytes& b, uint64_t v, const char* what) {
    if (v > 0xffff)
        throw FormatError(std::string(what) + " does not fit in 16 bits");
    put8(b, static_cast<uint32_t>(v >> 8));
    put8(b, static_cast<uint32_t>(v));
}

void put32(Bytes& b, uint64_t v, const char* what) {
    if (v > 0xffffffffu)
        throw FormatError(std::string(what) + " does not fit in 32 bits");
    for (int s = 24; s >= 0; s -= 8)
        put8(b, static_cast<uint32_t>(v >> s));
}

class Reader {
public:
    explicit Reader(const Bytes& b) : b_(b) {}

    void need(uint64_t off, uint64_t len, const char* what) const {
        if (off > b_.size() || len > b_.size() - off)
            throw FormatError(std::string(what) + " runs past the end of the file");
    }
    uint32_t u8(uint64_t off) const {
        need(off, 1, "field");
        return static_cast<unsigned char>(b_[off]);
    }
    uint32_t u16(uint64_t off) const { return (u8(off) << 8) | u8(off + 1); }
    uint32_t u32(uint64_t off) const {
        need(off, 4, "field");
        return (u8(off) << 24) | (u8(off + 1) << 16) | (u8(off + 2) << 8) | u8(off + 3);
    }
    Bytes slice(uint64_t off, uint64_t len, const char* what) const {
        need(off, len, what);
        return b_.substr(off, len);
    }
    // NUL-terminated string at `off`, at most kMaxNameBytes long; offset 0
    // means "no string".
    std::optional<std::string> cstr(uint32_t off, const char* what) const {
        if (off == 0) return std::nullopt;
        if (off >= b_.size())
            throw FormatError(std::string(what) + " offset is past the end of the file");
        size_t limit = std::min<size_t>(b_.size() - off, kMaxNameBytes + 1);
        const char* start = b_.data() + off;
        const void* nul = std::memchr(start, '\0', limit);
        if (!nul) {
            if (limit > kMaxNameBytes)
                over_budget("name-length", std::string(what) + " is longer than " +
                            std::to_string(kMaxNameBytes) + " bytes", kMaxNameBytes);
            throw FormatError(std::string(what) + " is not NUL-terminated inside the file");
        }
        return std::string(start, static_cast<const char*>(nul) - start);
    }

private:
    const Bytes& b_;
};

// ---------------------------------------------------------------------------
// Input container.
// ---------------------------------------------------------------------------

struct Comp {
    std::string name;
    std::vector<uint64_t> idx;
};

// Parses a run of `[digits]` groups starting at `pos`; returns the index
// just past the last complete group.
size_t scan_indices(const std::string& s, size_t pos, std::vector<uint64_t>* idx) {
    while (pos < s.size() && s[pos] == '[') {
        size_t k = pos + 1;
        uint64_t v = 0;
        while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
            v = v * 10 + uint64_t(s[k] - '0');
            if (v > 0xffffffffu)
                throw FormatError("array index too large in '" + s + "'");
            ++k;
        }
        if (k == pos + 1 || k >= s.size() || s[k] != ']') break;
        if (idx) idx->push_back(v);
        pos = k + 1;
    }
    return pos;
}

// `a.b[1][2].c` -> (a) (b [1,2]) (c).  Separator characters that do not
// belong to a component are skipped.
std::vector<Comp> split_path(const std::string& name) {
    auto sep = [](char c) { return c == '.' || c == '[' || c == ']'; };
    std::vector<Comp> out;
    size_t depth = 0;
    size_t i = 0;
    while (i < name.size()) {
        if (sep(name[i])) { ++i; continue; }
        size_t j = i;
        while (j < name.size() && !sep(name[j])) ++j;
        Comp c;
        c.name = name.substr(i, j - i);
        j = scan_indices(name, j, &c.idx);
        depth += 1 + c.idx.size();
        if (depth > kMaxPathDepth)
            over_budget("name-depth", "parameter name '" + name.substr(0, 64) + "...' nests deeper",
                        kMaxPathDepth);
        out.push_back(std::move(c));
        i = j;
    }
    return out;
}

// `sa[0][1].b` -> `.b`: the name with its leading base and that base's
// indices removed; names without a leading indexed base are unchanged.
std::string strip_leading_indices(const std::string& name) {
    size_t p = name.find('[');
    if (p == std::string::npos) return name;
    size_t e = scan_indices(name, p, nullptr);
    return e == p ? name : name.substr(e);
}

struct Param {
    uint32_t type = 0, res = 0, var = 0;
    int32_t  resIndex = 0;
    std::string name;
    std::optional<std::string> sem;     // never empty when present
    std::optional<Bytes> dflt;          // 16 bytes when present
    std::vector<uint32_t> ec;           // embedded-constant ucode offsets
    uint32_t dir = 0, paramno = 0, ref = 0, shared = 0;
    std::vector<size_t> rows;           // matrix row parameters, if any
    std::vector<Comp> path;             // `name` split into components
};

struct Program {
    uint32_t profile = 0;
    Bytes hdr;                          // program-specific header
    Bytes ucode;
    std::vector<Param> params;
    bool fragment() const { return profile == kProfileFragment; }
};

// std::nullopt: not a shader container at all (too short, wrong revision).
// FormatError: a container whose contents cannot be converted.
std::optional<Program> read_container(const Bytes& b) {
    if (b.size() < 32) return std::nullopt;
    Reader r(b);
    Program p;
    p.profile = r.u32(0);
    if (r.u32(4) != kContainerRevision) return std::nullopt;
    uint32_t count   = r.u32(12);
    uint32_t parrOff = r.u32(16);
    if (count > kMaxParams)
        over_budget("parameter-count", "container declares " + std::to_string(count) + " parameters",
                    kMaxParams);
    r.need(parrOff, uint64_t(count) * 48, "parameter array");

    // Embedded-constant lists are sized, per reference, before any is copied.
    uint64_t ecTotal = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (uint32_t e = r.u32(parrOff + uint64_t(i) * 48 + 24)) {
            ecTotal += r.u32(e);
            if (ecTotal > kMaxEmbeddedConstants)
                over_budget("embedded-constants",
                            "parameters reference more than " +
                                std::to_string(kMaxEmbeddedConstants) + " ucode offsets",
                            kMaxEmbeddedConstants);
        }
    }
    uint32_t progOff = r.u32(20);
    uint32_t ucSize  = r.u32(24);
    uint32_t ucOff   = r.u32(28);

    p.hdr   = r.slice(progOff, p.fragment() ? 22 : 24, "program header");
    p.ucode = r.slice(ucOff, ucSize, "ucode");

    p.params.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t o = parrOff + uint64_t(i) * 48;
        Param q;
        q.type     = r.u32(o + 0);
        q.res      = r.u32(o + 4);
        q.var      = r.u32(o + 8);
        q.resIndex = static_cast<int32_t>(r.u32(o + 12));
        auto name  = r.cstr(r.u32(o + 16), "parameter name");
        if (!name || name->empty())
            throw FormatError("parameter " + std::to_string(i) + " has no name");
        q.name = *name;
        q.path = split_path(q.name);
        if (uint32_t d = r.u32(o + 20)) q.dflt = r.slice(d, 16, "default value");
        if (uint32_t e = r.u32(o + 24)) {
            uint32_t n = r.u32(e);
            if (n > 0xffff)
                throw FormatError("embedded-constant count does not fit in 16 bits");
            r.need(uint64_t(e) + 4, uint64_t(n) * 4, "embedded-constant list");
            for (uint32_t k = 0; k < n; ++k) q.ec.push_back(r.u32(uint64_t(e) + 4 + 4 * uint64_t(k)));
        }
        auto sem = r.cstr(r.u32(o + 28), "semantic");
        if (sem && !sem->empty()) q.sem = sem;
        q.dir     = r.u32(o + 32);
        q.paramno = r.u32(o + 36);
        q.ref     = r.u32(o + 40);
        q.shared  = r.u32(o + 44);
        p.params.push_back(std::move(q));
    }
    return p;
}

// ---------------------------------------------------------------------------
// Parameter tree.
// ---------------------------------------------------------------------------

// A matrix parameter is followed in the container by its rows, named
// `<name>[0]`, `<name>[1]`, ...; fold them into the matrix.  Returns the
// indices of the remaining (leaf) parameters in container order.
std::vector<size_t> collapse_rows(std::vector<Param>& ps) {
    std::vector<size_t> out;
    size_t i = 0;
    while (i < ps.size()) {
        std::vector<size_t> rows;
        size_t j = i + 1;
        while (j < ps.size() &&
               ps[j].name == ps[i].name + "[" + std::to_string(rows.size()) + "]")
            rows.push_back(j++);
        ps[i].rows = rows;
        out.push_back(i);
        i = j;
    }
    return out;
}

// `a[0][1].b` -> (`a`, `.b`): the array a struct-array leaf belongs to and
// its member path within an element.
std::pair<std::string, std::string> array_member_key(const std::string& name) {
    size_t p = name.find('[');
    return {name.substr(0, p), strip_leading_indices(name)};
}

// A parameter's path seen from one tree level: components [level, end).
struct Item {
    const std::vector<Comp>* path;
    size_t leaf;
};

struct Node {
    enum Kind { Leaf, Struct, Array, StructElem } kind;
    std::string name;
    std::vector<Node> children;
    size_t leaf = 0;                    // Leaf
    std::vector<uint64_t> dims;         // Array
    bool structElems = false;           // Array
    std::vector<size_t> elems;          // Array of non-struct: element leaves
};

// Groups consecutive items by their component at `level`.  Paths are
// shared, never copied, and recursion depth is bounded by kMaxPathDepth.
std::vector<Node> build_tree(const std::vector<Item>& items, size_t level) {
    std::vector<Node> nodes;
    size_t i = 0;
    auto comp = [level](const Item& it) -> const Comp& { return (*it.path)[level]; };
    while (i < items.size()) {
        if (items[i].path->size() <= level)
            throw FormatError("parameter name has an empty component");
        const std::string& base = comp(items[i]).name;
        std::vector<Item> grp;
        while (i < items.size() && items[i].path->size() > level && comp(items[i]).name == base)
            grp.push_back(items[i++]);

        const std::vector<uint64_t>& idx0 = comp(grp[0]).idx;
        Node n;
        n.name = base;
        if (idx0.empty()) {
            if (grp[0].path->size() == level + 1) {
                n.kind = Node::Leaf;
                n.leaf = grp[0].leaf;
            } else {
                n.kind = Node::Struct;
                n.children = build_tree(grp, level + 1);
            }
        } else {
            n.kind = Node::Array;
            size_t nd = idx0.size();
            n.dims.assign(nd, 0);
            for (const Item& it : grp) {
                if (comp(it).idx.size() < nd)
                    throw FormatError("array '" + base + "' is indexed with inconsistent dimensions");
                for (size_t k = 0; k < nd; ++k)
                    n.dims[k] = std::max(n.dims[k], comp(it).idx[k] + 1);
            }
            bool allLeaves = std::all_of(grp.begin(), grp.end(), [level](const Item& it) {
                return it.path->size() == level + 1;
            });
            if (allLeaves) {
                for (const Item& it : grp) n.elems.push_back(it.leaf);
            } else {
                n.structElems = true;
                std::map<std::vector<uint64_t>, size_t> seen;
                std::vector<std::pair<std::vector<uint64_t>, std::vector<Item>>> els;
                for (const Item& it : grp) {
                    const auto& key = comp(it).idx;
                    auto f = seen.find(key);
                    if (f == seen.end()) {
                        f = seen.emplace(key, els.size()).first;
                        els.push_back({key, {}});
                    }
                    els[f->second].second.push_back(it);
                }
                for (const auto& [key, members] : els) {
                    uint64_t flat = 0;
                    for (size_t k = 0; k < std::min(key.size(), n.dims.size()); ++k)
                        flat = flat * n.dims[k] + key[k];
                    Node e;
                    e.kind = Node::StructElem;
                    e.name = base + "[" + std::to_string(flat) + "]";
                    e.children = build_tree(members, level + 1);
                    n.children.push_back(std::move(e));
                }
            }
        }
        nodes.push_back(std::move(n));
    }
    return nodes;
}

void collect_leaves(const Node& n, std::vector<size_t>& out) {
    if (n.kind == Node::Leaf) {
        out.push_back(n.leaf);
    } else if (n.kind == Node::Array && !n.structElems) {
        out.insert(out.end(), n.elems.begin(), n.elems.end());
    } else {
        for (const Node& c : n.children) collect_leaves(c, out);
    }
}

std::vector<size_t> leaves_of(const Node& n) {
    std::vector<size_t> out;
    collect_leaves(n, out);
    return out;
}

uint32_t base_flags(const Param& p) {
    uint32_t f = 0;
    if (p.var == kVarUniform) f = 1;
    else if (p.var == kVarConstant) f = 2;
    if (p.dir == kDirOut) f |= 1u << 2;
    else if (p.dir == kDirInOut) f |= 2u << 2;
    if (p.shared) f |= kFlagShared;
    if (p.paramno == kParamnoGlobal) f |= kFlagGlobal;
    if (p.paramno == kParamnoInternal) f |= kFlagInternal;
    return f;
}

// ---------------------------------------------------------------------------
// Archive string table: offset 0 is the empty string; a string is reused
// wherever `s\0` already occurs, including as the tail of a longer string.
// ---------------------------------------------------------------------------

// Strings never contain NUL, so `s\0` can only occur as the tail of one
// stored string.  Stored strings are kept in a trie of their reversed
// bytes; each node remembers the earliest terminator reachable through it,
// which gives the first occurrence in time linear in the string's length.
class StringTable {
public:
    uint32_t add(const std::string& s) {
        if (b_.empty()) {
            b_.push_back('\0');
            firstEnd_.push_back(0);                 // root: the empty string at 0
        }
        uint32_t node = 0;
        bool found = true;
        for (size_t i = s.size(); found && i-- > 0;) {
            auto it = kids_.find(key(node, s[i]));
            if (it == kids_.end()) found = false;
            else node = it->second;
        }
        if (found) return firstEnd_[node] - static_cast<uint32_t>(s.size());

        if (b_.size() + s.size() + 1 > 0xffffffffu)
            throw FormatError("string table exceeds 4 GiB");
        uint32_t off = static_cast<uint32_t>(b_.size());
        uint32_t end = off + static_cast<uint32_t>(s.size());
        b_ += s;
        b_.push_back('\0');
        node = 0;
        for (size_t i = s.size(); i-- > 0;) {
            auto [it, fresh] = kids_.emplace(key(node, s[i]), 0);
            if (fresh) {
                it->second = static_cast<uint32_t>(firstEnd_.size());
                firstEnd_.push_back(end);           // older strings claimed nodes first
            }
            node = it->second;
        }
        return off;
    }
    const Bytes& bytes() const { return b_; }

private:
    static uint64_t key(uint32_t node, char c) {
        return (uint64_t(node) << 8) | static_cast<unsigned char>(c);
    }
    Bytes b_;
    std::unordered_map<uint64_t, uint32_t> kids_;
    std::vector<uint32_t> firstEnd_;
};

// ---------------------------------------------------------------------------
// Parameter table.
// ---------------------------------------------------------------------------

struct Record {
    std::string name;
    Bytes type;                          // type-table entry
    uint32_t flags = 0;
    std::optional<std::string> sem;
};

class ParamTableBuilder {
public:
    ParamTableBuilder(Program& prog, StringTable& st, Bytes& constData, bool noSem)
        : ps_(prog.params), st_(st), const_(constData), noSem_(noSem),
          fp_(prog.fragment()) {}

    Bytes build() {
        lv_ = collapse_rows(ps_);

        // Fragment: one resource-index entry per leaf (per row for a
        // matrix): i16 resIndex, u16 n, u16 ucodeOffset[n].
        hw_.assign(ps_.size(), 0);
        if (fp_) {
            for (size_t li : lv_) {
                hw_[li] = fpRi_.size() / 2;
                for (size_t qi : rows_or_self(li)) {
                    const Param& q = ps_[qi];
                    if (q.resIndex < -32768 || q.resIndex > 32767)
                        throw FormatError("resource index of '" + q.name + "' does not fit in 16 bits");
                    put16(fpRi_, static_cast<uint16_t>(q.resIndex), "resource index");
                    put16(fpRi_, q.ec.size(), "embedded-constant count");
                    for (uint32_t e : q.ec) put16(fpRi_, e, "embedded-constant offset");
                }
            }
        }

        // Fragment leaves inside a struct array share one referenced bit
        // per member across the elements of THAT array (`a[0].x`, `a[1].x`),
        // never with a same-named member of another array (`b[0].x`).
        if (fp_)
            for (size_t li : lv_)
                if (ps_[li].ref) memberRef_.insert(array_member_key(ps_[li].name));

        std::vector<Item> items;
        for (size_t li : lv_) items.push_back({&ps_[li].path, li});
        std::vector<Node> roots = build_tree(items, 0);

        std::vector<std::pair<size_t, size_t>> defs;   // (record, const word)
        for (const Node& r : roots) {
            size_t first = recs_.size();
            emit(r, std::nullopt);
            Bytes blk;
            for (size_t li : leaves_of(r))
                for (size_t qi : rows_or_self(li))
                    if (ps_[qi].dflt) blk += *ps_[qi].dflt;
            if (!blk.empty()) {
                defs.emplace_back(first, const_.size() / 4);
                const_ += blk;
            }
        }

        std::vector<std::pair<uint32_t, std::optional<uint32_t>>> offs;
        for (const Record& rec : recs_) {
            uint32_t no = st_.add(rec.name);
            std::optional<uint32_t> so;
            if (rec.sem && !noSem_) so = st_.add(*rec.sem);
            offs.emplace_back(no, so);
        }

        Bytes types;
        std::vector<size_t> typeOff;
        for (const Record& rec : recs_) {
            typeOff.push_back(types.size());
            types += rec.type;
        }
        const Bytes& ri = fp_ ? fpRi_ : vpRi_;
        size_t n = recs_.size();
        size_t semCount = 0;
        for (const auto& o : offs) semCount += o.second ? 1 : 0;
        size_t riOff  = 12 + 8 * n + types.size();
        size_t defOff = riOff + ri.size();
        size_t semOff = defOff + 4 * defs.size();

        Bytes out;
        put16(out, n, "record count");
        put16(out, riOff, "resource-index table offset");
        put16(out, defOff, "default table offset");
        put16(out, defs.size(), "default count");
        put16(out, semOff, "semantic table offset");
        put16(out, semCount, "semantic count");
        for (size_t i = 0; i < n; ++i) {
            put32(out, offs[i].first, "string offset");
            put16(out, typeOff[i], "type-table offset");
            put16(out, recs_[i].flags, "record flags");
        }
        out += types;
        out += ri;
        for (const auto& [rec, word] : defs) {
            put16(out, rec, "default record index");
            put16(out, word, "default constant index");
        }
        for (size_t i = 0; i < n; ++i) {
            if (!offs[i].second) continue;
            put16(out, i, "semantic record index");
            put16(out, 0, "pad");
            put32(out, *offs[i].second, "string offset");
        }
        return out;
    }

private:
    std::vector<size_t> rows_or_self(size_t li) const {
        const Param& p = ps_[li];
        return p.rows.empty() ? std::vector<size_t>{li} : p.rows;
    }

    uint32_t referenced(const std::vector<size_t>& ls) const {
        for (size_t li : ls)
            if (ps_[li].ref) return kFlagReferenced;
        return 0;
    }

    uint32_t leaf_resource(size_t li) const {
        const Param& p = ps_[li];
        bool direct = p.var == kVarVarying || is_sampler_type(p.type);
        if (fp_) return direct ? (p.res & 0xffff) : static_cast<uint32_t>(hw_[li]);
        return (direct ? p.res : static_cast<uint32_t>(p.resIndex)) & 0xffff;
    }

    static uint32_t semantic_flag(const Param& p) {
        if (!p.sem) return 0;
        const std::string& s = *p.sem;
        return (s.rfind("COLOR", 0) == 0 || s.rfind("NORMAL", 0) == 0) ? kFlagColorNormal : 0;
    }

    static Bytes leaf_type(uint32_t type, uint64_t res) {
        Bytes t;
        put16(t, type, "parameter type");
        put16(t, res, "parameter resource");
        return t;
    }

    // `inStructArray`: flags of the enclosing struct-array element, if any.
    void emit(const Node& n, std::optional<uint32_t> inStructArray) {
        if (n.kind == Node::Leaf) {
            const Param& p = ps_[n.leaf];
            bool ref = p.ref != 0;
            if (fp_ && inStructArray) {
                // A fragment leaf inside a struct array is referenced when
                // the same member of any element of its own array is.
                ref = memberRef_.count(array_member_key(p.name)) != 0;
            }
            recs_.push_back({n.name, leaf_type(p.type, leaf_resource(n.leaf)),
                             base_flags(p) | (ref ? kFlagReferenced : 0) | kFlagNode | semantic_flag(p),
                             p.sem});
        } else if (n.kind == Node::Struct || n.kind == Node::StructElem) {
            std::vector<size_t> ls = leaves_of(n);
            if (ls.empty()) throw FormatError("struct '" + n.name + "' has no members");
            uint32_t fl;
            if (n.kind == Node::StructElem)
                fl = (*inStructArray & ~(kFlagArray | kFlagStructArray)) | kFlagStruct;
            else
                fl = base_flags(ps_[ls[0]]) | referenced(ls) | kFlagStruct | kFlagNode;
            Bytes t;
            put16(t, n.children.size(), "struct member count");
            put16(t, 0, "pad");
            recs_.push_back({n.name, t, fl, std::nullopt});
            for (const Node& c : n.children)
                emit(c, n.kind == Node::Struct ? inStructArray : std::optional<uint32_t>(fl));
        } else {
            std::vector<size_t> ls = leaves_of(n);
            if (ls.empty()) throw FormatError("array '" + n.name + "' has no elements");
            uint32_t fl = base_flags(ps_[ls[0]]) | referenced(ls) | kFlagArray |
                          (n.structElems ? kFlagStructArray : 0);
            Bytes t;
            put16(t, n.structElems ? 1 : 0, "array kind");
            put16(t, n.dims.size(), "array dimension count");
            for (uint64_t d : n.dims) put16(t, d, "array dimension");
            while (t.size() % 4) t.push_back('\0');
            recs_.push_back({n.name, t, fl, std::nullopt});

            // Vertex: the array's registers, per element, member and row.
            size_t vstart = vpRi_.size() / 2;
            if (!fp_)
                for (size_t li : ls)
                    for (size_t qi : rows_or_self(li))
                        put16(vpRi_, static_cast<uint32_t>(ps_[qi].resIndex) & 0xffff, "register");

            if (n.structElems) {
                for (const Node& c : n.children) emit(c, fl);
            } else {
                // One template record describes every element.
                size_t e0 = n.elems[0];
                const Param& p0 = ps_[e0];
                uint64_t res = fp_ ? hw_[e0] : vstart;
                recs_.push_back({"", leaf_type(p0.type, res), base_flags(p0) | referenced(ls),
                                 std::nullopt});
            }
        }
    }

    std::vector<Param>& ps_;
    StringTable& st_;
    Bytes& const_;
    bool noSem_;
    bool fp_;
    std::vector<size_t> lv_;
    std::vector<size_t> hw_;
    Bytes fpRi_, vpRi_;
    std::vector<Record> recs_;
    std::set<std::pair<std::string, std::string>> memberRef_;
};

// ---------------------------------------------------------------------------
// Shader table entry (0x1c bytes).
// ---------------------------------------------------------------------------

Bytes shader_entry(const Program& prog) {
    Reader h(prog.hdr);
    Bytes e;
    // Any profile other than the fragment one is converted as a vertex
    // program and recorded as the vertex profile.
    put16(e, prog.fragment() ? kProfileFragment : kProfileVertex, "profile");
    put16(e, 0, "pad");
    if (prog.fragment()) {
        put32(e, h.u32(0), "instructionCount");
        put32(e, h.u32(4), "attributeInputMask");
        put32(e, h.u32(8), "partialTexType");
        put16(e, h.u16(12), "texCoordsInputMask");
        put16(e, h.u16(14), "texCoords2D");
        put16(e, h.u16(16), "texCoordsCentroid");
        put8(e, h.u8(18));                                   // registerCount
        put8(e, (h.u8(19) ? 1 : 0) | (h.u8(20) ? 2 : 0) | (h.u8(21) ? 4 : 0));
        put32(e, 0, "pad");
    } else {
        // Container order: instructionCount, instructionSlot,
        // registerCount, attributeInputMask, attributeOutputMask,
        // userClipMask.
        put32(e, h.u32(0), "instructionCount");
        put32(e, h.u32(12), "attributeInputMask");
        put32(e, h.u32(4), "instructionSlot");
        put32(e, h.u32(8), "registerCount");
        put32(e, h.u32(16), "attributeOutputMask");
        put32(e, h.u32(20), "userClipMask");
    }
    return e;
}

// ---------------------------------------------------------------------------
// ELF archive.
// ---------------------------------------------------------------------------

struct Section {
    uint32_t name = 0, type = 0, flags = 0;
    Bytes data;
    uint32_t align = 0, link = 0, entsize = 0;
};

Bytes write_archive(std::vector<Program>& progs, const std::vector<std::string>& names, bool noSem) {
    StringTable st;
    Bytes constData, shaderTab;
    std::vector<Bytes> paramTabs;
    std::vector<uint32_t> symOff;
    uint64_t outBytes = 0;
    for (size_t i = 0; i < progs.size(); ++i) {
        paramTabs.push_back(ParamTableBuilder(progs[i], st, constData, noSem).build());
        symOff.push_back(st.add(names[i]));
        shaderTab += shader_entry(progs[i]);
        outBytes += 0x1c + 16 + 2 * 40 + 32 + progs[i].ucode.size() + paramTabs.back().size();
        if (outBytes + st.bytes().size() + constData.size() > kMaxOutputBytes)
            over_budget("output-size", "the archive would exceed " +
                        std::to_string(kMaxOutputBytes) + " bytes", kMaxOutputBytes);
    }

    Bytes note;
    put32(note, 12, "note namesz");
    put32(note, 4, "note descsz");
    put32(note, 0, "note type");
    note += "SCE cgnv2elf";
    put32(note, kToolRevision, "note desc");

    Bytes sym(16, '\0');
    for (size_t i = 0; i < progs.size(); ++i) {
        put32(sym, symOff[i], "symbol name");
        put32(sym, i, "symbol value");
        put32(sym, 0, "symbol size");
        put8(sym, 0x1e);                                     // STB_GLOBAL, type 14
        put8(sym, 0);
        put16(sym, 6 + 2 * uint64_t(i), "symbol section index");
    }

    Bytes shstr(1, '\0');
    std::vector<Section> secs(1);
    auto add = [&](const std::string& name, uint32_t type, uint32_t flags, Bytes data,
                   uint32_t align, uint32_t link = 0, uint32_t entsize = 0) {
        Section s;
        s.name = static_cast<uint32_t>(shstr.size());
        shstr += name;
        shstr.push_back('\0');
        s.type = type; s.flags = flags; s.data = std::move(data);
        s.align = align; s.link = link; s.entsize = entsize;
        secs.push_back(std::move(s));
    };
    add(".shstrtab", 3, 0, Bytes(), 0);
    add(".note", 7, 2, note, 4);
    add(".strtab", 3, 2, st.bytes(), 1);
    add(".const", 1, 2, constData, 4);
    add(".symtab", 2, 2, sym, 1, 3, 16);
    add(".shadertab", 1, 2, shaderTab, 16, 0, 0x1c);
    for (size_t i = 0; i < progs.size(); ++i) {
        char tn[32], pn[32];
        std::snprintf(tn, sizeof tn, ".text%04zu", i);
        std::snprintf(pn, sizeof pn, ".paramtab%04zu", i);
        add(tn, 1, 6, progs[i].ucode, 16);
        add(pn, 1, 2, paramTabs[i], 4);
    }
    secs[1].data = shstr;

    const uint64_t nsec = secs.size();
    const uint64_t dataStart = 0x34 + 40 * nsec;
    uint64_t cur = dataStart;
    Bytes body, hdrs(40, '\0');
    for (size_t i = 1; i < secs.size(); ++i) {
        const Section& s = secs[i];
        uint64_t a = std::max<uint32_t>(s.align, 1);
        uint64_t off = (cur + a - 1) / a * a;
        if (!s.data.empty()) {
            body.append(off - (dataStart + body.size()), '\0');
            body += s.data;
        }
        cur = off + s.data.size();
        put32(hdrs, s.name, "sh_name");
        put32(hdrs, s.type, "sh_type");
        put32(hdrs, s.flags, "sh_flags");
        put32(hdrs, 0, "sh_addr");
        put32(hdrs, off, "sh_offset");
        put32(hdrs, s.data.size(), "sh_size");
        put32(hdrs, s.link, "sh_link");
        put32(hdrs, 0, "sh_info");
        put32(hdrs, s.align, "sh_addralign");
        put32(hdrs, s.entsize, "sh_entsize");
    }

    Bytes eh("\x7f" "ELF", 4);
    put8(eh, 1);        // ELFCLASS32
    put8(eh, 2);        // ELFDATA2MSB
    put8(eh, 1);        // EV_CURRENT
    put8(eh, 0x13);     // EI_OSABI
    put8(eh, 1);        // EI_ABIVERSION
    eh.append(7, '\0');
    put16(eh, 1, "e_type");                 // ET_REL
    put16(eh, 0x528e, "e_machine");
    put32(eh, 1, "e_version");
    put32(eh, 0, "e_entry");
    put32(eh, 0, "e_phoff");
    put32(eh, 0x34, "e_shoff");
    put32(eh, 0, "e_flags");
    put16(eh, 52, "e_ehsize");
    put16(eh, 32, "e_phentsize");
    put16(eh, 0, "e_phnum");
    put16(eh, 40, "e_shentsize");
    put16(eh, nsec, "section count");
    put16(eh, 1, "e_shstrndx");
    return eh + hdrs + body;
}

// ---------------------------------------------------------------------------
// Driver.
// ---------------------------------------------------------------------------

void usage(std::FILE* f) {
    std::fprintf(f,
        "usage: cgnv2elf [-qsuh][--quiet][--no-sem][--no-unref][--help] input [output]\n"
        "\n"
        "Packs RSX shader containers (.vpo/.fpo) into an ELF shader archive.\n"
        "input may be a single container or a folder of containers.\n"
        "\n"
        "  -q, --quiet     Print nothing on success\n"
        "  -s, --no-sem    Omit parameter semantics\n"
        "  -u, --no-unref  Omit unreferenced parameters (not supported)\n"
        "  -e              Keep the file extension in symbol names\n"
        "  -v, --version   Print version and exit\n"
        "  -h, --help      Print this help and exit\n"
        "\n"
        "Without output, the archive is written to <dir of input>/out<ext of input>.\n");
}

// The file name without its last extension; leading dots do not start one.
std::string strip_extension(const std::string& base) {
    size_t lead = base.find_first_not_of('.');
    if (lead == std::string::npos) return base;
    size_t dot = base.rfind('.');
    return (dot == std::string::npos || dot < lead) ? base : base.substr(0, dot);
}

std::string extension_of(const std::string& base) {
    return base.substr(strip_extension(base).size());
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool read_file(const fs::path& p, Bytes& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// Write via a temporary file beside `out`, then rename it into place, so
// a failure never leaves a partial archive behind.
bool write_atomically(const fs::path& out, const Bytes& data, std::string& err) {
    std::random_device rd;
    char suffix[40];
    std::snprintf(suffix, sizeof suffix, ".tmp-%08x%08x", rd(), rd());
    fs::path tmp = out;
    tmp += suffix;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot create " + tmp.u8string(); return false; }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) {
            f.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            err = "cannot write " + tmp.u8string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        err = "cannot rename " + tmp.u8string() + " to " + out.u8string() + ": " + ec.message();
        return false;
    }
    return true;
}

int run(int argc, char** argv) {
    bool quiet = false, noSem = false, noUnref = false, keepExt = false;
    bool help = false, version = false;
    std::vector<std::string> pos;
    bool optsDone = false;
    std::vector<std::string> ignored;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (optsDone || a.size() < 2 || a[0] != '-') { pos.push_back(a); continue; }
        if (a == "--") { optsDone = true; continue; }
        if (a.compare(0, 2, "--") == 0) {
            if (a == "--quiet") quiet = true;
            else if (a == "--no-sem") noSem = true;
            else if (a == "--no-unref") noUnref = true;
            else if (a == "--help") help = true;
            else if (a == "--version") version = true;
            else ignored.push_back(a);
            continue;
        }
        for (size_t k = 1; k < a.size(); ++k) {
            switch (a[k]) {
            case 'q': quiet = true; break;
            case 's': noSem = true; break;
            case 'u': noUnref = true; break;
            case 'e': keepExt = true; break;
            case 'h': help = true; break;
            case 'v': case 'V': version = true; break;
            default: ignored.push_back(std::string("-") + a[k]); break;
            }
        }
    }

    if (help) { usage(stdout); return 0; }
    if (version) { std::printf("cgnv2elf %s\n", RSX_CG_COMPILER_VERSION); return 0; }
    if (noUnref) {
        std::fprintf(stderr, "cgnv2elf: -u/--no-unref is not supported\n");
        return 1;
    }
    if (pos.empty() || pos.size() > 2) { usage(stderr); return 1; }
    if (!quiet)
        for (const std::string& o : ignored)
            std::fprintf(stderr, "cgnv2elf: warning: ignoring unknown option %s\n", o.c_str());

    std::string inArg = pos[0];
    while (inArg.size() > 1 && (inArg.back() == '/' || inArg.back() == '\\')) inArg.pop_back();
    const fs::path input = fs::u8path(inArg);

    std::error_code ec;
    fs::file_status st = fs::status(input, ec);
    if (ec || !fs::exists(st)) {
        std::fprintf(stderr, "cgnv2elf: couldn't open %s\n", pos[0].c_str());
        return 1;
    }
    const bool folder = fs::is_directory(st);

    fs::path output;
    if (pos.size() == 2) {
        output = fs::u8path(pos[1]);
    } else {
        output = input.parent_path() / ("out" + extension_of(input.filename().u8string()));
    }

    std::vector<fs::path> files;
    if (folder) {
        for (fs::directory_iterator it(input, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fe;
            if (it->is_regular_file(fe)) files.push_back(it->path());
        }
        if (ec) {
            std::fprintf(stderr, "cgnv2elf: couldn't read folder %s: %s\n",
                         pos[0].c_str(), ec.message().c_str());
            return 1;
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            std::string na = a.filename().u8string(), nb = b.filename().u8string();
            std::string la = lower_ascii(na), lb = lower_ascii(nb);
            return la != lb ? la < lb : na < nb;
        });
    } else {
        files.push_back(input);
    }

    std::vector<Program> progs;
    std::vector<std::string> names;
    uint64_t totalBytes = 0;
    for (const fs::path& f : files) {
        const std::string shown = f.u8string();
        std::error_code se;
        uint64_t size = fs::file_size(f, se);
        if (se) {
            std::fprintf(stderr, "cgnv2elf: couldn't open %s\n", shown.c_str());
            return 1;
        }
        if (size > kMaxInputBytes) {
            // Too large to be a shader container; only an error if it claims to be one.
            Bytes head(8, '\0');
            std::ifstream in(f, std::ios::binary);
            in.read(&head[0], 8);
            if (in.gcount() == 8 && Reader(head).u32(4) == kContainerRevision) {
                std::fprintf(stderr, "cgnv2elf: %s: %s\n", shown.c_str(),
                             budget_message("file-size", "container is larger than " +
                                            std::to_string(kMaxInputBytes) + " bytes",
                                            kMaxInputBytes).c_str());
                return 1;
            }
            if (!quiet) std::printf("skipping %s: not a shader container\n", shown.c_str());
            continue;
        }
        totalBytes += size;
        if (totalBytes > kMaxArchiveBytes) {
            std::fprintf(stderr, "cgnv2elf: %s\n",
                         budget_message("archive-size", "inputs exceed " +
                                        std::to_string(kMaxArchiveBytes) + " bytes in total",
                                        kMaxArchiveBytes).c_str());
            return 1;
        }
        Bytes data;
        if (!read_file(f, data)) {
            std::fprintf(stderr, "cgnv2elf: couldn't open %s\n", shown.c_str());
            return 1;
        }
        std::optional<Program> prog;
        try {
            prog = read_container(data);
        } catch (const FormatError& e) {
            std::fprintf(stderr, "cgnv2elf: %s: %s\n", shown.c_str(), e.what());
            return 1;
        }
        if (!prog) {
            if (!quiet) std::printf("skipping %s: not a shader container\n", shown.c_str());
            continue;
        }
        if (progs.size() == kMaxPrograms) {
            std::fprintf(stderr, "cgnv2elf: %s\n",
                         budget_message("program-count", "more than " + std::to_string(kMaxPrograms) +
                                        " programs in one archive", kMaxPrograms).c_str());
            return 1;
        }
        std::string base = f.filename().u8string();
        names.push_back(keepExt ? base : strip_extension(base));
        if (!quiet)
            std::printf("%s: %s program '%s'\n", shown.c_str(),
                        prog->fragment() ? "fragment" : "vertex", names.back().c_str());
        progs.push_back(std::move(*prog));
    }

    Bytes archive;
    try {
        archive = write_archive(progs, names, noSem);
    } catch (const FormatError& e) {
        std::fprintf(stderr, "cgnv2elf: %s\n", e.what());
        return 1;
    }

    std::string err;
    if (!write_atomically(output, archive, err)) {
        std::fprintf(stderr, "cgnv2elf: %s\n", err.c_str());
        return 1;
    }
    if (progs.empty()) {
        // An archive with no programs is still written, but the run fails.
        std::fprintf(stderr, "cgnv2elf: no shader programs in %s; wrote an empty archive %s\n",
                     pos[0].c_str(), output.u8string().c_str());
        return 1;
    }
    if (!quiet)
        std::printf("wrote %s: %zu program(s), %zu bytes\n", output.u8string().c_str(),
                    progs.size(), archive.size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cgnv2elf: %s\n", e.what());
        return 1;
    }
}
