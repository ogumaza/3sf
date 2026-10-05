// SPDX-License-Identifier: MIT

#include "csar/formats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "csar/binary.h"

namespace threesf::csar
{
namespace
{

// The block table of a CTR binary file, keyed by block type: {type, offset, size} entries after a 0x14-byte header.
// Throws std::runtime_error when the file isn't the kind `magic` names.
std::map<uint16_t, std::pair<uint32_t, uint32_t>> BlockTable(const Reader& r, const char* magic)
{
    if (std::string(magic) == "CSAR" && r.Size() >= 4 && r.Magic(0, "FSAR"))
    {
        throw std::runtime_error("a Wii U or Switch sound archive (FSAR), not a 3DS one (CSAR)");
    }
    if (!r.Magic(0, magic))
    {
        throw std::runtime_error(std::string("bad magic, expected ") + magic);
    }
    if (r.U16(4) != 0xFEFF)
    {
        throw std::runtime_error("unsupported byte order");
    }

    const uint16_t count = r.U16(0x10);
    std::map<uint16_t, std::pair<uint32_t, uint32_t>> blocks;
    for (uint16_t i = 0; i < count; i++)
    {
        const std::size_t e = 0x14 + i * 12;
        blocks[r.U16(e)] = {r.U32(e + 4), r.U32(e + 8)};
    }

    return blocks;
}

// A region table entry that plays nothing.
constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

// The optional parameters of an info entry: a uint32_t bit field, then one uint32_t for each bit that's set.
struct OptionParams
{
    static OptionParams Read(const Reader& r, std::size_t off)
    {
        OptionParams p;
        const uint32_t flags = r.U32(off);
        std::size_t q = off + 4;
        for (int bit = 0; bit < 32; bit++)
        {
            if (flags >> bit & 1)
            {
                p.values[bit] = r.U32(q);
                p.value_offset[bit] = q;
                q += 4;
            }
        }

        return p;
    }

    std::array<std::optional<uint32_t>, 32> values{};
    std::array<std::size_t, 32> value_offset{};
};

} // namespace

Wave Wave::Parse(std::span<const uint8_t> file)
{
    Reader r(file);
    auto blocks = BlockTable(r, "CWAV");
    const uint32_t info_off = blocks.at(0x7000).first;
    const auto [data_off, data_size] = blocks.at(0x7001);

    const std::size_t b = info_off + 8;
    Wave w;
    w.encoding = static_cast<WaveEncoding>(r.U8(b));
    w.loop = r.U8(b + 1) != 0;
    w.sample_rate = r.U32(b + 4);
    w.loop_start = r.U32(b + 8);
    w.loop_end = r.U32(b + 12);

    const std::size_t table = b + 0x14;
    const uint32_t nch = r.U32(table);
    for (uint32_t i = 0; i < nch; i++)
    {
        const Reference ref = ReadRef(r, table + 4 + i * 8);
        const std::size_t ci = table + ref.offset;
        const Reference sample_ref = ReadRef(r, ci);
        const Reference adpcm_ref = ReadRef(r, ci + 8);

        WaveChannel ch;
        const std::size_t sample_off = data_off + 8 + sample_ref.offset;
        ch.data = r.Sub(sample_off, data_off + data_size - sample_off);

        if (adpcm_ref.type == 0x0300)
        {
            const std::size_t a = ci + adpcm_ref.offset;
            DspAdpcmInfo info;
            for (int k = 0; k < 16; k++)
            {
                info.coefs[k] = r.S16(a + k * 2);
            }
            info.pred_scale = r.U16(a + 32);
            info.yn1 = r.S16(a + 34);
            info.yn2 = r.S16(a + 36);
            info.loop_pred_scale = r.U16(a + 38);
            info.loop_yn1 = r.S16(a + 40);
            info.loop_yn2 = r.S16(a + 42);
            ch.adpcm = info;
        }

        w.channels.push_back(ch);
    }

    return w;
}

WaveArchive WaveArchive::Parse(std::span<const uint8_t> file)
{
    Reader r(file);
    auto blocks = BlockTable(r, "CWAR");
    const uint32_t info_off = blocks.at(0x6800).first;
    const uint32_t file_off = blocks.at(0x6801).first;

    WaveArchive war;
    const uint32_t n = r.U32(info_off + 8);
    for (uint32_t i = 0; i < n; i++)
    {
        const std::size_t e = info_off + 12 + i * 12;
        const uint32_t off = r.U32(e + 4);
        const uint32_t size = r.U32(e + 8);
        war.waves.push_back(r.Sub(file_off + 8 + off, size));
    }

    return war;
}

namespace
{

VelocityRegion ReadVelocityRegion(const Reader& r, std::size_t p)
{
    // nw::snd BankFileReader (code.bin 0x48e2f8 and accessors 0x490b28..0x490cc4).
    VelocityRegion v;
    v.wave_id_index = r.U32(p);
    const OptionParams opt = OptionParams::Read(r, p + 4);
    if (opt.values[0])
    {
        v.original_key = static_cast<uint8_t>(*opt.values[0]);
    }

    if (opt.values[1])
    {
        v.volume = static_cast<uint8_t>(*opt.values[1]);
    }

    if (opt.values[2])
    {
        v.pan = static_cast<uint8_t>(*opt.values[2]);
    }

    if (opt.values[3])
    {
        v.pitch = r.F32(opt.value_offset[3]);
    }

    if (opt.values[4])
    {
        v.ignore_note_off = (*opt.values[4] & 0xff) != 0;
        v.key_group = static_cast<uint8_t>(*opt.values[4] >> 8);
        v.interpolation_type = static_cast<uint8_t>(*opt.values[4] >> 16);
    }

    if (opt.values[9])
    {
        // Reference {type, offset} located at p + value; ADSHR at the reference + offset.
        const std::size_t ref_at = p + *opt.values[9];
        const Reference ref = ReadRef(r, ref_at);
        const std::size_t a = ref_at + ref.offset;
        v.adshr = {r.U8(a), r.U8(a + 1), r.U8(a + 2), r.U8(a + 3), r.U8(a + 4)};
    }

    return v;
}

// Reads a region table (direct/range/index). Returns (lo, hi, target offset or kNpos).
std::vector<std::tuple<uint8_t, uint8_t, std::size_t>> ReadRegionTable(const Reader& r, std::size_t p)
{
    std::vector<std::tuple<uint8_t, uint8_t, std::size_t>> out;
    const Reference ref = ReadRef(r, p);
    const std::size_t q = p + ref.offset;

    const auto target = [&](std::size_t ref_off, std::size_t base) -> std::size_t
    {
        const Reference t = ReadRef(r, ref_off);
        if (t.offset == -1 || t.type == 0x5903)
        {
            return kNpos;
        }

        return base + t.offset;
    };

    switch (ref.type)
    {
    case 0x6000: // direct
        out.emplace_back(uint8_t{0}, uint8_t{127}, target(q, q));
        break;

    case 0x6001: // range: count, upper keys, refs
        {
            const uint32_t n = r.U32(q);
            const std::size_t refs = q + 4 + ((n + 3) & ~3u);
            uint8_t lo = 0;
            for (uint32_t i = 0; i < n; i++)
            {
                const uint8_t hi = r.U8(q + 4 + i);
                out.emplace_back(lo, hi, target(refs + i * 8, q));
                lo = static_cast<uint8_t>(hi + 1);
            }
            break;
        }

    case 0x6002: // index: min, max, refs
        {
            const uint8_t mn = r.U8(q), mx = r.U8(q + 1);
            for (int k = mn; k <= mx; k++)
            {
                out.emplace_back(static_cast<uint8_t>(k), static_cast<uint8_t>(k), target(q + 4 + (k - mn) * 8, q));
            }
            break;
        }

    default:
        throw std::runtime_error("unknown region table type");
    }

    return out;
}

} // namespace

const VelocityRegion* Instrument::Find(int key, int velocity) const
{
    for (const auto& k : keys)
    {
        if (key < k.lo || key > k.hi)
        {
            continue;
        }

        for (const auto& v : k.velocities)
        {
            if (velocity >= v.lo && velocity <= v.hi)
            {
                return v.region ? &*v.region : nullptr;
            }
        }
        return nullptr;
    }

    return nullptr;
}

Bank Bank::Parse(std::span<const uint8_t> file)
{
    Reader r(file);
    auto blocks = BlockTable(r, "CBNK");
    const uint32_t info_off = blocks.at(0x5800).first;

    const std::size_t b = info_off + 8;
    std::size_t wave_table = 0, inst_table = 0;
    for (int i = 0; i < 2; i++)
    {
        const Reference ref = ReadRef(r, b + i * 8);
        if (ref.type == 0x0100)
        {
            wave_table = b + ref.offset;
        }
        else if (ref.type == 0x0101)
        {
            inst_table = b + ref.offset;
        }
    }

    // A bank holds far fewer regions than bytes. A crafted bank could otherwise point its instruments and key regions
    // at the same tables over and over and take memory without limit.
    std::size_t regions = 0;
    const auto count_region = [&]
    {
        if (++regions > file.size())
        {
            throw std::runtime_error("the bank has more regions than bytes");
        }
    };

    Bank bank;
    const uint32_t nw = r.U32(wave_table);
    for (uint32_t i = 0; i < nw; i++)
    {
        bank.waves.push_back({r.U32(wave_table + 4 + i * 8), r.U32(wave_table + 8 + i * 8)});
    }

    const uint32_t ni = r.U32(inst_table);
    for (uint32_t i = 0; i < ni; i++)
    {
        const Reference ref = ReadRef(r, inst_table + 4 + i * 8);
        if (ref.type != 0x5900)
        {
            bank.instruments.emplace_back();
            continue;
        }

        Instrument inst;
        for (auto [lo, hi, key_off] : ReadRegionTable(r, inst_table + ref.offset))
        {
            count_region();

            KeyRegion k;
            k.lo = lo;
            k.hi = hi;
            if (key_off != kNpos)
            {
                for (auto [vlo, vhi, vel_off] : ReadRegionTable(r, key_off))
                {
                    count_region();

                    KeyRegion::Vel v{vlo, vhi, std::nullopt};
                    if (vel_off != kNpos)
                    {
                        v.region = ReadVelocityRegion(r, vel_off);
                    }
                    k.velocities.push_back(v);
                }
            }

            inst.keys.push_back(std::move(k));
        }

        bank.instruments.push_back(std::move(inst));
    }

    return bank;
}

Sequence Sequence::Parse(std::span<const uint8_t> file)
{
    Reader r(file);
    auto blocks = BlockTable(r, "CSEQ");
    const auto [data_off, data_size] = blocks.at(0x5000);

    Sequence s;
    s.data = r.Sub(data_off + 8, data_size - 8);

    return s;
}

SoundArchive SoundArchive::Load(std::vector<uint8_t> bytes)
{
    SoundArchive a;
    a.bytes_ = std::move(bytes);
    Reader r(a.bytes_);
    auto blocks = BlockTable(r, "CSAR");
    if (!blocks.count(0x2001) || !blocks.count(0x2002))
    {
        throw std::runtime_error("no INFO or FILE block");
    }

    const uint32_t info_off = blocks.at(0x2001).first;
    const uint32_t file_off = blocks.at(0x2002).first;

    if (r.U32(0x0C) > a.bytes_.size())
    {
        a.truncated_ = true;
    }

    // STRG: string table + patricia tree mapping names to item ids. An archive may be built without it (no names; the
    // block's offset is then 0xFFFFFFFF).
    std::vector<std::string> strings;
    std::map<uint32_t, std::string> item_names;
    const auto strg = blocks.find(0x2000);
    if (strg != blocks.end() && strg->second.first != 0xFFFFFFFF && strg->second.first < a.bytes_.size())
    {
        const std::size_t b = strg->second.first + 8;
        std::size_t st = 0, pt = 0;
        for (int i = 0; i < 2; i++)
        {
            const Reference ref = ReadRef(r, b + i * 8);
            if (ref.type == 0x2400)
            {
                st = b + ref.offset;
            }
            else if (ref.type == 0x2401)
            {
                pt = b + ref.offset;
            }
        }

        // The strings lie in the block, and each names one item, so neither the strings nor the names can add up to
        // more than the block. A crafted table could otherwise point every entry at one long string and take memory
        // without limit.
        const std::size_t block_size = strg->second.second;
        std::size_t string_bytes = 0, name_bytes = 0;
        const uint32_t n = r.U32(st);
        for (uint32_t i = 0; i < n; i++)
        {
            const std::size_t e = st + 4 + i * 12;
            strings.push_back(r.CString(st + r.U32(e + 4), r.U32(e + 8)));
            string_bytes += strings.back().size();
            if (string_bytes > block_size)
            {
                throw std::runtime_error("the strings don't fit in the string block");
            }
        }

        const uint32_t nodes = r.U32(pt + 4);
        for (uint32_t i = 0; i < nodes; i++)
        {
            const std::size_t e = pt + 8 + i * 20;
            if (r.U16(e) & 1)
            {
                const uint32_t sid = r.U32(e + 12), iid = r.U32(e + 16);
                if (sid < strings.size())
                {
                    name_bytes += strings[sid].size();
                    if (name_bytes > block_size)
                    {
                        throw std::runtime_error("the names don't fit in the string block");
                    }

                    item_names[iid] = strings[sid];
                }
            }
        }
    }

    const auto name_of = [&](uint32_t item) -> std::string
    {
        auto it = item_names.find(item);
        return it == item_names.end() ? std::string() : it->second;
    };

    const std::size_t b = info_off + 8;
    std::map<uint16_t, std::size_t> tables;
    for (int i = 0; i < 8; i++)
    {
        const Reference ref = ReadRef(r, b + i * 8);
        tables[ref.type] = b + ref.offset;
    }

    const auto for_each = [&](uint16_t type, auto fn)
    {
        const std::size_t t = tables.at(type);
        const uint32_t n = r.U32(t);
        for (uint32_t i = 0; i < n; i++)
        {
            fn(i, t + ReadRef(r, t + 4 + i * 8).offset);
        }
    };

    const auto read_sound = [&](uint32_t i, std::size_t p)
    {
        SoundInfo s;
        s.name = name_of(0x01000000 | i);
        s.file_id = r.U32(p);
        s.volume = r.U8(p + 8);
        const Reference detail = ReadRef(r, p + 12);
        const OptionParams opt = OptionParams::Read(r, p + 20);
        if (opt.values[1])
        {
            s.pan_mode = static_cast<uint8_t>(*opt.values[1]);
            s.pan_curve = static_cast<uint8_t>(*opt.values[1] >> 8);
        }

        switch (detail.type)
        {
        case 0x2201:
            s.type = SoundType::kStream;
            break;

        case 0x2202:
            s.type = SoundType::kWaveSound;
            break;

        case 0x2203:
            {
                s.type = SoundType::kSequence;
                const std::size_t q = p + detail.offset;
                const Reference banks = ReadRef(r, q);
                const std::size_t bt = q + banks.offset;
                // The sequence player uses four banks at most. A crafted archive could otherwise give every sound one
                // long list and take memory without limit.
                const uint32_t nb = std::min<uint32_t>(r.U32(bt), 4);
                for (uint32_t k = 0; k < nb; k++)
                {
                    s.banks.push_back(r.U32(bt + 4 + k * 4));
                }

                s.allocate_track_flags = r.U32(q + 8);
                const OptionParams so = OptionParams::Read(r, q + 12);
                if (so.values[0])
                {
                    s.start_offset = *so.values[0];
                }

                if (so.values[1])
                {
                    s.channel_priority = static_cast<uint8_t>(*so.values[1]);
                    s.release_priority_fix = ((*so.values[1] >> 8) & 0xff) != 0;
                }
                break;
            }

        default:
            break;
        }

        if (s.name.empty())
        {
            // An archive without a string table has no names: make them up from the index.
            const char* prefix = s.type == SoundType::kSequence    ? "SEQ_"
                                 : s.type == SoundType::kStream    ? "STRM_"
                                 : s.type == SoundType::kWaveSound ? "WSD_"
                                                                   : "SOUND_";
            s.name = prefix + std::to_string(i);
        }

        a.sounds_.push_back(std::move(s));
    };
    for_each(0x2100, read_sound);

    for_each(0x2101, [&](uint32_t i, std::size_t p) { a.banks_.push_back({name_of(0x03000000 | i), r.U32(p)}); });

    const auto read_wave_archive = [&](uint32_t i, std::size_t p)
    {
        a.wave_archives_.push_back({name_of(0x05000000 | i), r.U32(p)});
    };
    for_each(0x2103, read_wave_archive);

    const auto read_file = [&](uint32_t /*index*/, std::size_t p)
    {
        FileEntry f;
        const Reference loc = ReadRef(r, p);
        if (loc.type == 0x220C)
        {
            const std::size_t q = p + loc.offset;
            const uint32_t off = r.U32(q + 4), size = r.U32(q + 8);
            if (off != 0xFFFFFFFF)
            {
                const uint64_t start = static_cast<uint64_t>(file_off) + 8 + off;
                if (start + size <= a.bytes_.size())
                {
                    f.internal = true;
                    f.offset = static_cast<uint32_t>(start);
                    f.size = size;
                }
                else
                {
                    a.truncated_ = true; // the file lies past the end of a truncated archive
                }
            }
        }

        a.files_.push_back(f);
    };
    for_each(0x2106, read_file);

    // A group file (CGRP) embeds copies of other files, and some files are only there: their own entries have no
    // location, or an offset of 0xFFFFFFFF. Its INFO block lists each item's file id and where its bytes are.
    const auto read_group = [&](uint32_t /*index*/, std::size_t p)
    {
        const uint32_t group_file = r.U32(p);
        if (group_file >= a.files_.size() || !a.files_[group_file].internal)
        {
            return; // no group file, or one outside the archive
        }

        const FileEntry& group = a.files_[group_file];
        const Reader g(std::span<const uint8_t>(a.bytes_).subspan(group.offset, group.size));
        const auto group_blocks = BlockTable(g, "CGRP");
        const uint32_t items = group_blocks.at(0x7800).first + 8;
        const uint32_t data = group_blocks.at(0x7801).first + 8;

        const uint32_t n = g.U32(items);
        for (uint32_t i = 0; i < n; i++)
        {
            const std::size_t e = items + ReadRef(g, items + 4 + i * 8).offset;
            const uint32_t file_id = g.U32(e);
            const Reference where = ReadRef(g, e + 4);
            if (where.type != 0x1F00 || file_id >= a.files_.size() || a.files_[file_id].internal)
            {
                continue; // not embedded, or the archive holds it already
            }

            const auto bytes = g.Sub(data + where.offset, g.U32(e + 12));
            a.files_[file_id] = {static_cast<uint32_t>(bytes.data() - a.bytes_.data()),
                                 static_cast<uint32_t>(bytes.size()), true};
        }
    };
    for_each(0x2105, read_group);

    return a;
}

std::optional<uint32_t> SoundArchive::FindSound(const std::string& name) const
{
    for (uint32_t i = 0; i < sounds_.size(); i++)
    {
        if (sounds_[i].name == name)
        {
            return i;
        }
    }

    return std::nullopt;
}

std::span<const uint8_t> SoundArchive::FileData(uint32_t file_id) const
{
    if (file_id >= files_.size() || !files_[file_id].internal)
    {
        return {};
    }

    return std::span<const uint8_t>(bytes_).subspan(files_[file_id].offset, files_[file_id].size);
}

} // namespace threesf::csar
