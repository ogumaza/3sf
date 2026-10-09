// SPDX-License-Identifier: MIT

// CSAR parser tests using small archives and banks built here. Repeated references to the same bytes must not cause
// unbounded allocations, and files embedded in a group (CGRP) must be readable without a separate location.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "check.h"
#include "common/bytes.h"
#include "csar/formats.h"

using namespace threesf;

namespace
{

// A reference: a type id, then the target's offset from `base`.
void PutRef(std::vector<uint8_t>& file, std::size_t at, uint16_t type, std::size_t target, std::size_t base)
{
    StoreLe16(&file[at], type);
    StoreLe32(&file[at + 4], static_cast<uint32_t>(target - base));
}

// The file header and its block table, with the blocks given as {type, offset, size}.
void PutHeader(std::vector<uint8_t>& file, const char* magic, const std::vector<std::array<uint32_t, 3>>& blocks)
{
    std::copy(magic, magic + 4, file.begin());
    StoreLe16(&file[4], 0xFEFF);
    StoreLe16(&file[6], 0x40);
    StoreLe32(&file[0x0C], static_cast<uint32_t>(file.size()));

    StoreLe16(&file[0x10], static_cast<uint16_t>(blocks.size()));
    for (std::size_t i = 0; i < blocks.size(); i++)
    {
        StoreLe16(&file[0x14 + i * 12], static_cast<uint16_t>(blocks[i][0]));
        StoreLe32(&file[0x18 + i * 12], blocks[i][1]);
        StoreLe32(&file[0x1C + i * 12], blocks[i][2]);
    }
}

std::size_t Align4(std::size_t size)
{
    return (size + 3) & ~std::size_t{3};
}

// A bank of `instruments` instruments that all share one key region table of `keys` ranges, each of which shares one
// velocity region table of `velocities` ranges that play nothing.
std::vector<uint8_t> MakeBank(uint32_t instruments, uint32_t keys, uint32_t velocities)
{
    const std::size_t kInfo = 0x40, kB = kInfo + 8;
    const std::size_t kWaveTable = kB + 16, kInstTable = kWaveTable + 4;
    const std::size_t instrument = kInstTable + 4 + instruments * 8, key_table = instrument + 8;
    const std::size_t key_region = key_table + 4 + Align4(keys) + keys * 8, vel_table = key_region + 8;
    const std::size_t end = vel_table + 4 + Align4(velocities) + velocities * 8;
    std::vector<uint8_t> file(end);

    PutRef(file, kB, 0x0100, kWaveTable, kB);
    PutRef(file, kB + 8, 0x0101, kInstTable, kB);
    StoreLe32(&file[kInstTable], instruments);
    for (uint32_t i = 0; i < instruments; i++)
    {
        PutRef(file, kInstTable + 4 + i * 8, 0x5900, instrument, kInstTable);
    }

    PutRef(file, instrument, 0x6001, key_table, instrument);
    StoreLe32(&file[key_table], keys);
    for (uint32_t i = 0; i < keys; i++)
    {
        file[key_table + 4 + i] = static_cast<uint8_t>(i);
        PutRef(file, key_table + 4 + Align4(keys) + i * 8, 0x5901, key_region, key_table);
    }

    PutRef(file, key_region, 0x6001, vel_table, key_region);
    StoreLe32(&file[vel_table], velocities);
    for (uint32_t i = 0; i < velocities; i++)
    {
        file[vel_table + 4 + i] = static_cast<uint8_t>(i);
        PutRef(file, vel_table + 4 + Align4(velocities) + i * 8, 0x5903, 0, 0);
    }

    PutHeader(file, "CBNK", {{0x5800, static_cast<uint32_t>(kInfo), static_cast<uint32_t>(end - kInfo)}});

    return file;
}

// A sound archive with one sequence, which uses `banks` banks. Its string table has `strings` entries that all point at
// `text`, and its name tree has `leaves` leaves, all with string 0, that name items 0x01000000 onwards.
std::vector<uint8_t> MakeArchive(const std::string& text, uint32_t strings, uint32_t leaves, uint32_t banks)
{
    const std::size_t kStrg = 0x40, kSb = kStrg + 8;
    const std::size_t kStringTable = kSb + 16, text_at = kStringTable + 4 + strings * 12;
    const std::size_t tree = Align4(text_at + text.size() + 1), strg_end = tree + 8 + leaves * 20;
    const std::size_t info = strg_end, ib = info + 8;
    const std::size_t sound_table = ib + 64, empty_table = sound_table + 12;
    const std::size_t sound = empty_table + 4, detail = sound + 24, bank_table = detail + 16;
    const std::size_t file_block = bank_table + 4 + banks * 4, end = file_block + 8;
    std::vector<uint8_t> file(end);

    PutRef(file, kSb, 0x2400, kStringTable, kSb);
    PutRef(file, kSb + 8, 0x2401, tree, kSb);
    std::copy(text.begin(), text.end(), file.begin() + static_cast<std::ptrdiff_t>(text_at));
    StoreLe32(&file[kStringTable], strings);
    for (uint32_t i = 0; i < strings; i++)
    {
        PutRef(file, kStringTable + 4 + i * 12, 0x1F01, text_at, kStringTable);
        StoreLe32(&file[kStringTable + 12 + i * 12], static_cast<uint32_t>(text.size() + 1));
    }

    StoreLe32(&file[tree + 4], leaves);
    for (uint32_t i = 0; i < leaves; i++)
    {
        StoreLe16(&file[tree + 8 + i * 20], 1);
        StoreLe32(&file[tree + 24 + i * 20], 0x01000000 | i);
    }

    // Eight tables, of which only the sound table has anything in it.
    constexpr std::array<uint16_t, 8> kTableTypes = {0x2100, 0x2101, 0x2102, 0x2103, 0x2104, 0x2105, 0x2106, 0x220B};
    for (std::size_t i = 0; i < kTableTypes.size(); i++)
    {
        PutRef(file, ib + i * 8, kTableTypes[i], i == 0 ? sound_table : empty_table, ib);
    }

    StoreLe32(&file[sound_table], 1);
    PutRef(file, sound_table + 4, 0x2200, sound, sound_table);
    PutRef(file, sound + 12, 0x2203, detail, sound);
    PutRef(file, detail, 0x0100, bank_table, detail);
    StoreLe32(&file[detail + 8], 1);

    StoreLe32(&file[bank_table], banks);
    for (uint32_t i = 0; i < banks; i++)
    {
        StoreLe32(&file[bank_table + 4 + i * 4], 0x03000000 | i);
    }

    PutHeader(file, "CSAR",
              {{0x2000, static_cast<uint32_t>(kStrg), static_cast<uint32_t>(strg_end - kStrg)},
               {0x2001, static_cast<uint32_t>(info), static_cast<uint32_t>(file_block - info)},
               {0x2002, static_cast<uint32_t>(file_block), 8}});

    return file;
}

// A sound archive with no sounds and three files. File 0 is a group (CGRP) that embeds `payload` as file 1 and lists
// file 2 without embedding it, and neither file 1 nor file 2 has a location of its own.
std::vector<uint8_t> MakeGroupArchive(const std::vector<uint8_t>& payload)
{
    constexpr std::size_t kItems = 0x48, kItem1 = kItems + 20, kItem2 = kItem1 + 16, kGroupData = kItem2 + 16;
    std::vector<uint8_t> group(kGroupData + 8 + Align4(payload.size()));

    StoreLe32(&group[kItems], 2);
    PutRef(group, kItems + 4, 0x7900, kItem1, kItems);
    PutRef(group, kItems + 12, 0x7900, kItem2, kItems);
    StoreLe32(&group[kItem1], 1);
    PutRef(group, kItem1 + 4, 0x1F00, 0, 0);
    StoreLe32(&group[kItem1 + 12], static_cast<uint32_t>(payload.size()));
    StoreLe32(&group[kItem2], 2);
    StoreLe32(&group[kItem2 + 8], 0xFFFFFFFF); // a null reference: type 0, offset -1
    std::copy(payload.begin(), payload.end(), group.begin() + kGroupData + 8);
    PutHeader(group, "CGRP",
              {{0x7800, 0x40, static_cast<uint32_t>(kGroupData - 0x40)},
               {0x7801, static_cast<uint32_t>(kGroupData), static_cast<uint32_t>(group.size() - kGroupData)}});

    constexpr std::size_t kTables = 0x48, kEmptyTable = kTables + 64, kGroupTable = kEmptyTable + 4;
    constexpr std::size_t kGroupEntry = kGroupTable + 12, kFileTable = kGroupEntry + 8, kFile0 = kFileTable + 28;
    constexpr std::size_t kFile0Location = kFile0 + 8, kFile1 = kFile0Location + 12, kFile2 = kFile1 + 8;
    constexpr std::size_t kFileBlock = kFile2 + 8;
    std::vector<uint8_t> file(kFileBlock + 8 + group.size());

    constexpr std::array<uint16_t, 8> kTableTypes = {0x2100, 0x2101, 0x2102, 0x2103, 0x2104, 0x2105, 0x2106, 0x220B};
    for (std::size_t i = 0; i < kTableTypes.size(); i++)
    {
        const uint16_t type = kTableTypes[i];
        PutRef(file, kTables + i * 8, type,
               type == 0x2105   ? kGroupTable
               : type == 0x2106 ? kFileTable
                                : kEmptyTable,
               kTables);
    }

    StoreLe32(&file[kGroupTable], 1);
    PutRef(file, kGroupTable + 4, 0x2208, kGroupEntry, kGroupTable);
    StoreLe32(&file[kFileTable], 3);
    PutRef(file, kFileTable + 4, 0x220A, kFile0, kFileTable);
    PutRef(file, kFileTable + 12, 0x220A, kFile1, kFileTable);
    PutRef(file, kFileTable + 20, 0x220A, kFile2, kFileTable);
    PutRef(file, kFile0, 0x220C, kFile0Location, kFile0);
    StoreLe32(&file[kFile0Location + 8], static_cast<uint32_t>(group.size()));
    StoreLe32(&file[kFile1 + 4], 0xFFFFFFFF);
    StoreLe32(&file[kFile2 + 4], 0xFFFFFFFF);
    std::copy(group.begin(), group.end(), file.begin() + kFileBlock + 8);
    PutHeader(file, "CSAR",
              {{0x2001, 0x40, static_cast<uint32_t>(kFileBlock - 0x40)},
               {0x2002, static_cast<uint32_t>(kFileBlock), static_cast<uint32_t>(8 + group.size())}});

    return file;
}

// The message `f` throws, or "" if it returns.
template <typename F>
std::string ErrorOf(F f)
{
    try
    {
        f();
    }
    catch (const std::exception& e)
    {
        return e.what();
    }

    return "";
}

void TestBankRegions()
{
    const std::vector<uint8_t> small = MakeBank(2, 3, 2);
    const std::vector<uint8_t> repeating = MakeBank(64, 100, 100);

    const csar::Bank bank = csar::Bank::Parse(small);
    const std::string error = ErrorOf([&] { csar::Bank::Parse(repeating); });

    THREESF_CHECK(bank.instruments.size() == 2);
    THREESF_CHECK(bank.instruments[1] && bank.instruments[1]->keys.size() == 3);
    THREESF_CHECK(bank.instruments[1] && bank.instruments[1]->keys[2].velocities.size() == 2);
    THREESF_CHECK(error == "the bank has more regions than bytes");
}

void TestArchiveTables()
{
    const std::string long_name(1000, 'a');
    const std::vector<uint8_t> small = MakeArchive("SEQ_TEST", 1, 1, 6);
    const std::vector<uint8_t> repeated_strings = MakeArchive(long_name, 5000, 1, 1);
    const std::vector<uint8_t> repeated_names = MakeArchive(long_name, 1, 5000, 1);

    const csar::SoundArchive archive = csar::SoundArchive::Load(small);
    const std::string strings_error = ErrorOf([&] { csar::SoundArchive::Load(repeated_strings); });
    const std::string names_error = ErrorOf([&] { csar::SoundArchive::Load(repeated_names); });

    THREESF_CHECK(archive.Sounds().size() == 1);
    THREESF_CHECK(archive.Sounds()[0].name == "SEQ_TEST");
    THREESF_CHECK(archive.Sounds()[0].banks.size() == 4);
    THREESF_CHECK(strings_error == "the strings don't fit in the string block");
    THREESF_CHECK(names_error == "the names don't fit in the string block");
}

// A file without a block that it needs names the block in its error. The standard library's message was "map::at".
void TestMissingBlocks()
{
    std::vector<uint8_t> bank(0x40);
    std::vector<uint8_t> sequence(0x40);
    PutHeader(bank, "CBNK", {});
    PutHeader(sequence, "CSEQ", {{0x5001, 0x20, 0x20}});

    const std::string bank_error = ErrorOf([&] { csar::Bank::Parse(bank); });
    const std::string sequence_error = ErrorOf([&] { csar::Sequence::Parse(sequence); });

    THREESF_CHECK(bank_error == "a bank (CBNK) has no INFO block");
    THREESF_CHECK(sequence_error == "a sequence (CSEQ) has no DATA block");
}

void TestGroups()
{
    const std::vector<uint8_t> kPayload = {'C', 'B', 'N', 'K', 1, 2, 3, 4, 5};
    const std::vector<uint8_t> file = MakeGroupArchive(kPayload);

    const csar::SoundArchive archive = csar::SoundArchive::Load(file);
    const auto embedded = archive.FileData(1);

    THREESF_CHECK(std::equal(embedded.begin(), embedded.end(), kPayload.begin(), kPayload.end()));
    THREESF_CHECK(archive.FileData(2).empty());
    THREESF_CHECK(archive.FileData(0).size() > kPayload.size());
}

} // namespace

int main()
{
    TestBankRegions();
    TestArchiveTables();
    TestMissingBlocks();
    TestGroups();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("csar tests passed\n");

    return 0;
}
