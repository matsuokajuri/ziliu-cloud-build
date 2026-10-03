#pragma once

#include "ziliu/broker/rime_source_profile.h"

namespace ziliu::broker {

// Frozen source-file precondition, NOT production runtime/user-config admission.
// DLL: official librime 1.17.0 / 33e7814 archive, SHA256
// 7478c7caa4ff6b37de86daba1f7ce4a994a4f5ba24872a820fb2b3a9b01fed15.
// Data: clean rime-ice b681a34f788795034b3b288830f4861980bc8b0d plus
// this revision's data/ziliu overlays; includes lua/lunar.db (not documentation).
// Aggregate: sorted UTF-8 POSIX path + space + lowercase file SHA256 + LF.
// Update only after an explicit dependency/overlay review, never from staged
// runtime bytes or a user-supplied manifest. See CONTEXTUAL-CANDIDATE-RANKING.md.
inline constexpr RimeSourceProfileExpected kPinnedRimeSourceProfile{
    "86b4c7357d4c6d293ce5589b234d8859ca2ac30923a03bedfa3926eeaf97fb0b",
    "5c144cbae73b450aac4a41e68d8e4e7ced0099eef9282c7273ffa5f8a556eca3",
    137};

// Independently compiled from the pinned inputs in an empty isolated profile.
// Only generated timestamp values and CRC-validated prism schema CRCs normalize;
// all behavior-bearing bytes and the exact 14-file inventory remain pinned.
// This is a startup precondition, not production admission or a live file lock.
inline constexpr RimeCompiledProfileExpected kPinnedRimeCompiledProfile{
    "4071f2221e29656e553810a688b360b16c398e9bda1952d59cad2e9cb70843ed", 14};

}  // namespace ziliu::broker
