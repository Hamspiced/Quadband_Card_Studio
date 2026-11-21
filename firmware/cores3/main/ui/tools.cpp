#include "tools.hpp"
#include "controller.hpp"
#include "typography.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace qb::ui {
using local::Command;
using local::Field;
using local::Intent;
using local::Tool;
using local::View;
namespace {
template<size_t N> std::array<char, N> safe(const std::array<char, N>& value) {
    auto copy = value; copy[N - 1] = 0; return copy;
}
const char* lf_name(unsigned index) {
    static const char* names[]{"EM410x", "Electra", "HID Prox", "Viking",
                               "ioProx", "PAC / Stanley", "Jablotron", "Pyramid"};
    return index < 8 ? names[index] : "Unsupported";
}
bool lf_scan_ok(const View& v) {
    return v.reader_present && v.lf_format < 8 && (v.lf_scan_mask & (1U << v.lf_format));
}
bool lf_write_ok(const View& v) {
    return lf_scan_ok(v) && v.lf_format != 2 && v.lf_format != 7 &&
        (v.lf_write_mask & (1U << v.lf_format));
}
bool selected(const View& v) { return v.selected_tag < std::min(64U, v.tag_count); }
bool selectable(const View& v) {
    return selected(v) && v.tags[v.selected_tag].size >= 2 &&
        v.tags[v.selected_tag].size <= 28 && !(v.tags[v.selected_tag].size % 2);
}
const char* status_name(Status status) {
    switch (status) {
    case OK: return "Ready";
    case NO_CARD: return "No card";
    case AUTH_FAILED: return "Authentication failed";
    case STALE_GENERATION: return "Reader changed - scan again";
    case STALE_DATA: return "Data changed - read again";
    case WRITE_UNVERIFIED: return "WRITE UNVERIFIED - read again";
    case CANCELLED: return "Stopped";
    case UNSUPPORTED: return "Unsupported operation";
    case UNAVAILABLE: return "Reader unavailable";
    case PERMISSION_DENIED: return "Protected / not permitted";
    case INVALID_ARGUMENT: return "Check field values";
    case TIMEOUT: return "Reader timeout";
    default: return "Operation failed";
    }
}
std::array<char, 125> epc_text(const local::TagView& tag) {
    std::array<char, 125> text{};
    constexpr char hex[]{"0123456789ABCDEF"};
    for (unsigned i = 0; i < std::min(62U, unsigned(tag.size)); ++i) {
        text[i * 2] = hex[tag.epc[i] >> 4]; text[i * 2 + 1] = hex[tag.epc[i] & 15];
    }
    return text;
}
bool ntag(local::HfFamily family) {
    return family == local::HfFamily::Ntag213 || family == local::HfFamily::Ntag215 ||
        family == local::HfFamily::Ntag216;
}
}

void ToolsUi::open(Tool tool) {
    close();
    if (tool != Tool::Hf && tool != Tool::Uhf && tool != Tool::Lf) return;
    tool_ = tool;
    page_ = tool == Tool::Hf ? Page::HfMemory : tool == Tool::Uhf ? Page::UhfTags : Page::LfScan;
}
void ToolsUi::close() {
    editor_.clear(); local::secure_zero(hits_.data(), sizeof(hits_));
    damage_.reset();
    hit_count_ = 0; tool_ = Tool::None; page_ = Page::HfMemory;
    list_page_ = field_page_ = 0; editing_ = Field::HfValue;
    surface_ = nullptr; painted_ = false; dirty_ = true; revision_ = 0;
    painted_review_ = painted_busy_ = false; painted_review_token_ = 0;
}
void ToolsUi::fill(int x, int y, int w, int h, uint32_t color) {
    if (surface_) surface_->fill({x, y, w, h}, color);
}
void ToolsUi::label(int x, int y, int width, const char* text, unsigned style,
                    bool muted, uint32_t background) {
    if (!surface_ || !text || width <= 0) return;
    std::array<char, 257> bounded{};
    size_t n = 0; int used = 0;
    while (n < bounded.size() - 1 && text[n]) {
        const unsigned char c = static_cast<unsigned char>(text[n]);
        const char printable = c >= 32 && c <= 126 ? char(c) : '?';
        const int advance = glyph(static_cast<unsigned char>(printable), style).advance;
        if (used + advance > width) break;
        bounded[n++] = printable; used += advance;
    }
    if (n < 256 && text[n]) {
        const int dots = text_width("...", style);
        while (n && used + dots > width) used -= glyph(static_cast<unsigned char>(bounded[--n]), style).advance;
        if (used + dots <= width && n + 3 < bounded.size()) std::memcpy(bounded.data() + n, "...", 4);
        else bounded[n] = 0;
    }
    if (background == UINT32_MAX) background = canvas_;
    surface_->text(x, y, bounded.data(), style, muted ? muted_ : text_, background);
}
void ToolsUi::lines(int x, int y, int width, const char* text, unsigned count, unsigned style) {
    if (!surface_ || !text) return;
    const char* p = text;
    for (unsigned row = 0; row < count && *p; ++row) {
        std::array<char, 257> line{};
        size_t n = 0; int used = 0;
        while (n < 256 && *p && *p != '\n') {
            const unsigned char raw = static_cast<unsigned char>(*p);
            const char c = raw >= 32 && raw <= 126 ? char(raw) : '?';
            const int advance = glyph(static_cast<unsigned char>(c), style).advance;
            if (used + advance > width) break;
            line[n++] = c; used += advance; ++p;
        }
        if (*p == '\n') ++p;
        if (row + 1 == count && *p && n > 3) {
            line[n - 3] = '.'; line[n - 2] = '.'; line[n - 1] = '.';
        }
        label(x, y + int(row) * (text_height(style) + 2), width, line.data(), style);
        if (!n) break;
    }
}
void ToolsUi::button(int x, int y, int w, int h, const char* title, const Hit& source, bool enabled, bool primary) {
    const uint32_t bg = primary && enabled ? accent_ : panel_;
    fill(x, y, w, h, enabled ? border_ : panel_);
    fill(x + 1, y + 1, w - 2, h - 2, bg);
    const uint32_t old = text_;
    if (!enabled) text_ = muted_;
    else if (primary) text_ = light_ ? 0xffffff : 0x002f2d;
    label(x + 5, y + (h - text_height(1)) / 2, w - 10, title, 1, false, bg);
    text_ = old;
    if (enabled && hit_count_ < hits_.size()) {
        hits_[hit_count_] = source;
        auto& hit = hits_[hit_count_++]; hit.x = x; hit.y = y; hit.w = w; hit.h = h;
    }
}
void ToolsUi::action(int x, int y, int w, int h, const char* title, Command command, bool enabled, unsigned index, bool primary) {
    Hit hit; hit.command = command; hit.index = index;
    button(x, y, w, h, title, hit, enabled, primary);
}
void ToolsUi::nav(int x, int y, int w, int h, const char* title, Page page) {
    Hit hit; hit.kind = HitKind::Navigate; hit.page = page;
    button(x, y, w, h, title, hit, true, page_ == page);
}
void ToolsUi::edit(int x, int y, int w, int h, const char* title, Field field, bool enabled) {
    Hit hit; hit.kind = HitKind::Edit; hit.field = field;
    button(x, y, w, h, title, hit, enabled);
}
void ToolsUi::set(int x, int y, int w, int h, const char* title, Field field, unsigned value, bool enabled) {
    Hit hit; hit.kind = HitKind::Set; hit.field = field; hit.index = value;
    button(x, y, w, h, title, hit, enabled);
}
void ToolsUi::pages(unsigned current, unsigned total, bool fields) {
    Hit hit; hit.kind = HitKind::PageIndex; hit.field = fields ? Field::NdefField0 : Field::HfUnit;
    hit.index = current ? current - 1 : 0;
    button(8, 184, 48, 28, "<", hit, current > 0);
    char value[40]; std::snprintf(value, sizeof(value), "%u / %u", current + 1, std::max(1U, total));
    label(122, 190, 120, value);
    hit.index = current + 1;
    button(264, 184, 48, 28, ">", hit, current + 1 < total);
}
void ToolsUi::header(const View& view) {
    action(8, 4, 52, 26, "Back", Command::Exit);
    label(68, 8, 164, tool_ == Tool::Hf ? "HF / NFC" : tool_ == Tool::Uhf ? "UHF" : "LF / Chameleon", 1);
    char battery[20];
    if (view.battery < 0) std::snprintf(battery, sizeof(battery), "BAT --");
    else std::snprintf(battery, sizeof(battery), "BAT %d%%", std::clamp(view.battery, 0, 100));
    if ((tool_ == Tool::Uhf && view.inventory) || view.bulk_active) action(244, 4, 68, 26, "Stop", Command::Stop);
    else label(244, 8, 70, battery, 1, true);
}
void ToolsUi::keyboard() {
    label(8, 8, 304, editor_.title(), 1);
    fill(8, 35, 304, 32, panel_);
    label(14, 43, 292, editor_.display(), 1, false, panel_);
    for (size_t i = 0; i < editor_.key_count(); ++i) {
        const auto& key = editor_.keys()[i];
        fill(key.x, key.y, key.w, key.h, border_);
        fill(key.x + 1, key.y + 1, key.w - 2, key.h - 2, panel_);
        label(key.x + 4, key.y + (key.h - text_height(1)) / 2,
              key.w - 8, key.label, 1, false, panel_);
    }
}
void ToolsUi::hf(const View& v) {
    nav(8, 34, 72, 28, "Memory", Page::HfMemory);
    nav(84, 34, 64, 28, "Auth", ntag(v.hf_family) ? Page::HfNtagAuth : Page::HfAuth);
    nav(152, 34, 72, 28, "NFC", Page::NdefRecords);
    nav(228, 34, 84, 28, "RAM copy", Page::HfCopy);
    char line[100]{};
    if (page_ == Page::HfMemory) {
        label(8, 65, 304, safe(v.identity).data(), 1, true);
        action(8, 84, 48, 28, "<", Command::HfSelectUnit, v.hf_ready && v.hf_unit > 0, v.hf_unit ? v.hf_unit - 1 : 0);
        std::snprintf(line, sizeof(line), "%s %u / %u", ntag(v.hf_family) ? "Page" : "Block", v.hf_unit, v.hf_units);
        edit(60, 84, 200, 28, line, Field::HfUnit, v.hf_ready);
        action(264, 84, 48, 28, ">", Command::HfSelectUnit,
               v.hf_ready && v.hf_unit + 1 < v.hf_units, v.hf_unit + 1);
        lines(8, 116, 304, v.hf_unit_known ? safe(v.hf_data).data() : "Read card memory to inspect bytes.", 2);
        action(8, 156, 96, 36, "Read card", Command::HfRead, v.reader_present, 0, true);
        edit(108, 156, 96, 36, "Edit bytes", Field::HfValue, v.hf_unit_known && v.hf_writable);
        action(208, 156, 104, 36, "Review edit", Command::HfPrepareUnit,
               v.reader_present && v.hf_unit_known && v.hf_writable && v.hf_value[0]);
    } else if (page_ == Page::HfAuth) {
        set(8, 68, 148, 30, v.hf_key_type == 0x60 ? "Key A [selected]" : "Key A", Field::HfKeyType, 0x60);
        set(164, 68, 148, 30, v.hf_key_type == 0x61 ? "Key B [selected]" : "Key B", Field::HfKeyType, 0x61);
        Hit factory; factory.kind = HitKind::Set; factory.field = Field::HfKey;
        std::snprintf(factory.value, sizeof(factory.value), "FFFFFFFFFFFF");
        button(8, 102, 148, 30, "Factory key", factory);
        edit(164, 102, 148, 30, "Supplied key: ******", Field::HfKey);
        set(8, 138, 304, 30, v.hf_standard_keys ? "Standard NFC keys: ON" : "Standard NFC keys: OFF",
            Field::HfStandardKeys, !v.hf_standard_keys);
        nav(8, 174, 304, 28, "NTAG authentication >", Page::HfNtagAuth);
    } else if (page_ == Page::HfNtagAuth) {
        set(8, 68, 96, 30, v.ntag_mode == 0 ? "None [x]" : "None", Field::NtagMode, 0);
        set(108, 68, 96, 30, v.ntag_mode == 1 ? "PWD [x]" : "PWD", Field::NtagMode, 1);
        set(208, 68, 104, 30, v.ntag_mode == 2 ? "PWD/PACK [x]" : "PWD/PACK", Field::NtagMode, 2);
        edit(8, 104, 148, 34, "Password: ****", Field::NtagPassword);
        edit(164, 104, 148, 34, "PACK: ****", Field::NtagPack);
        lines(8, 144, 304, "Credentials remain in RAM. Known lock and protection rules still apply.", 2);
        nav(8, 180, 304, 28, "< Classic authentication", Page::HfAuth);
    } else if (page_ == Page::HfCopy) {
        label(8, 70, 304, v.hf_source ? "Source captured in RAM" : "No RAM source captured");
        lines(8, 92, 304, "Read source, capture it, then present a compatible target. Review before copying.", 3);
        action(8, 148, 148, 34, "Capture source", Command::HfCapture, v.hf_ready);
        action(164, 148, 148, 34, "Review RAM copy", Command::HfPrepareCopy,
               v.reader_present && v.hf_source && v.hf_ready && v.hf_copy_armed);
        label(8, 188, 304, v.hf_copy_armed ? "Target removal gate ready" : "Remove target before repeat copy", 1, true);
    } else if (page_ == Page::NdefRecords) {
        std::snprintf(line, sizeof(line), "%u read | %u draft | %u / %u B",
                      v.ndef_count, v.ndef_queue, v.ndef_size, v.ndef_capacity);
        label(8, 66, 304, line);
        const unsigned count = std::min(8U, v.ndef_count);
        const unsigned page = std::min(list_page_, count ? (count - 1) / 2 : 0);
        for (unsigned row = 0; row < 2; ++row) {
            const unsigned index = page * 2 + row;
            if (index < count) label(8, 86 + int(row) * 20, 304, safe(v.ndef_records[index]).data());
        }
        if (!count) label(8, 90, 304, "No decoded NDEF records", 1, true);
        nav(8, 122, 148, 26, "Bulk job (1-3)", Page::NdefBulk);
        nav(164, 122, 148, 26, "Draft order/remove", Page::NdefQueue);
        nav(8, 150, 96, 30, "New record", Page::NdefKinds);
        action(108, 150, 96, 30, "Clear draft", Command::HfClearRecords);
        action(208, 150, 104, 30, "Review NFC", Command::HfPrepareNdef, v.reader_present && v.ndef_available && v.ndef_queue > 0);
        pages(page, std::max(1U, (count + 1) / 2));
    } else if (page_ == Page::NdefBulk) {
        const char* names[]{"Unknown","Classic1K","Classic4K","NTAG213","NTAG215","NTAG216"};
        unsigned family = unsigned(v.bulk_family);
        unsigned next = family == 1 ? 3 : family >= 5 ? 1 : family+1;
        std::snprintf(line,sizeof(line),"%s | %u records / %u B",names[std::min(5u,family)],v.ndef_queue,v.bulk_bytes);
        set(8,66,304,26,line,Field::BulkFamily,next,!v.bulk_active);
        set(8,96,148,26,v.bulk_overwrite ? "Overwrite: ON" : "Overwrite: OFF",Field::BulkOverwrite,!v.bulk_overwrite,!v.bulk_active);
        set(164,96,148,26,v.bulk_formatting ? "Format: ON" : "Format: OFF",Field::BulkFormatting,!v.bulk_formatting,!v.bulk_active);
        std::snprintf(line,sizeof(line),"W%u same%u reject%u fail%u",v.bulk_written,v.bulk_identical,v.bulk_rejected,v.bulk_failed);
        label(8,126,304,line);
        label(8,144,304,v.bulk_phase == 1 ? "Remove all; Read must see No card" : v.bulk_phase == 4 ? "UNVERIFIED: Clear session required" : "Read target; fresh review per card");
        action(8,166,96,30,"Arm 1-3",Command::HfArmBulk,!v.bulk_active && v.ndef_queue > 0 && v.ndef_queue <= 3);
        action(108,166,96,30,"Read",Command::HfRead,v.reader_present && v.bulk_phase != 4);
        action(208,166,104,30,"Review",Command::HfPrepareBulk,v.bulk_active && v.hf_ready && v.bulk_phase == 2);
    } else if (page_ == Page::NdefQueue) {
        unsigned page = std::min(list_page_,v.ndef_queue ? v.ndef_queue-1 : 0);
        std::snprintf(line,sizeof(line),"Draft %u / %u | %u encoded B",v.ndef_queue ? page+1 : 0,v.ndef_queue,v.bulk_bytes);
        label(8,74,304,line);
        action(8,106,148,32,"Move up",Command::HfMoveRecordUp,!v.bulk_active && page > 0,page);
        action(164,106,148,32,"Remove draft",Command::HfRemoveRecord,!v.bulk_active && page < v.ndef_queue,page);
        nav(8,146,148,30,"New record",Page::NdefKinds);
        nav(164,146,148,30,"Bulk job",Page::NdefBulk);
        pages(page,std::max(1u,v.ndef_queue));
    } else if (page_ == Page::NdefKinds) {
        const unsigned page = std::min(list_page_, 4U);
        for (unsigned row = 0; row < 4; ++row) {
            const unsigned kind = page * 4 + row;
            if (kind < v.ndef_kinds.size())
                set(8, 66 + int(row) * 29, 304, 27, safe(v.ndef_kinds[kind]).data(), Field::NdefKind, kind);
        }
        pages(page, 5);
    } else if (page_ == Page::NdefFields) {
        const unsigned count = std::min(8U, unsigned(v.ndef_field_count));
        const unsigned page = std::min(field_page_, count ? (count - 1) / 2 : 0);
        for (unsigned row = 0; row < 2; ++row) {
            const unsigned index = page * 2 + row;
            if (index >= count) break;
            const auto& field = v.ndef_fields[index];
            const Field key = Field(unsigned(Field::NdefField0) + index);
            label(8, 66 + int(row) * 40, 304, safe(field.label).data(), 1, true);
            if (field.choice_count) {
                Hit hit; hit.kind = HitKind::Choice; hit.field = key; hit.index = index;
                button(8, 82 + int(row) * 40, 304, 24, field.secret ? "********" : safe(field.value).data(), hit);
            } else edit(8, 82 + int(row) * 40, 304, 24, field.secret ? "********" : safe(field.value).data(), key);
        }
        if (!count) label(8, 78, 304, "No fields required for this record");
        action(8, 150, 96, 30, "Add draft", Command::HfAddRecord);
        nav(108, 150, 96, 30, "Kinds", Page::NdefKinds);
        action(208, 150, 104, 30, "Review NFC", Command::HfPrepareNdef, v.ndef_available && v.ndef_queue > 0);
        pages(page, std::max(1U, (count + 1) / 2), true);
    }
}
void ToolsUi::uhf(const View& v) {
    nav(8, 34, 96, 28, "Tags", Page::UhfTags);
    nav(108, 34, 96, 28, "Memory", Page::UhfMemory);
    nav(208, 34, 104, 28, "RF power", Page::UhfPower);
    char line[150]{};
    if (page_ == Page::UhfTags) {
        const unsigned count = std::min(64U, v.tag_count);
        const unsigned page = std::min(list_page_, count ? (count - 1) / 4 : 0);
        for (unsigned row = 0; row < 4; ++row) {
            const unsigned index = page * 4 + row;
            if (index >= count) break;
            const auto& tag = v.tags[index]; const auto epc = epc_text(tag);
            std::snprintf(line, sizeof(line), "%c%u %ddBm x%u %.20s", index == v.selected_tag ? '*' : ' ',
                          index + 1, int(tag.rssi), static_cast<unsigned>(tag.reads), epc.data());
            action(8, 66 + int(row) * 28, 304, 26, line, Command::UhfSelectTag, !v.inventory, index);
        }
        if (!count) lines(8, 74, 304, "No tags yet. Start inventory to find nearby EPC tags.", 2);
        action(8, 182, 96, 30, v.inventory ? "Stop scan" : "Start scan",
               v.inventory ? Command::Stop : Command::UhfStart, v.reader_present, 0, !v.inventory);
        Hit hit; hit.kind = HitKind::PageIndex; hit.index = page ? page - 1 : 0;
        button(112, 182, 44, 30, "<", hit, page > 0);
        std::snprintf(line, sizeof(line), "%u / %u", page + 1, std::max(1U, (count + 3) / 4));
        label(164, 190, 80, line);
        hit.index = page + 1; button(264, 182, 48, 30, ">", hit, (page + 1) * 4 < count);
    } else if (page_ == Page::UhfMemory) {
        if (selected(v)) lines(8, 66, 304, epc_text(v.tags[v.selected_tag]).data(), 4);
        else label(8, 68, 304, "Stop inventory, then select a tag", 1, true);
        if (v.inventory) label(8, 132, 304, "Memory disabled while scanning", 1, true);
        else if (selected(v) && !selectable(v)) label(8, 132, 304, "Long EPC is inventory-only (>28 bytes)", 1, true);
        else {
            std::snprintf(line, sizeof(line), "Bank %u / word %u / %u words", v.uhf_bank, v.uhf_word, v.uhf_count);
            label(8, 132, 304, line, 1, true);
        }
        if (v.uhf_memory) label(8, 150, 304, safe(v.uhf_data).data());
        nav(8, 174, 96, 34, "Settings", Page::UhfConfig);
        action(108, 174, 96, 34, "Read", Command::UhfRead, v.reader_present && !v.inventory && selectable(v), 0, true);
        Hit data; data.kind = HitKind::Navigate; data.page = Page::UhfData;
        button(208, 174, 104, 34, "Data / edit", data, !v.inventory && v.uhf_memory);
    } else if (page_ == Page::UhfData) {
        const auto data = safe(v.uhf_data);
        const unsigned length = unsigned(std::strlen(data.data()));
        const unsigned total = std::max(1U, (length + 95) / 96);
        const unsigned page = std::min(field_page_, total - 1);
        std::array<char, 97> chunk{};
        const unsigned begin = page * 96;
        const unsigned amount = std::min(96U, length - std::min(begin, length));
        std::copy_n(data.data() + std::min(begin, length), amount, chunk.data());
        std::snprintf(line, sizeof(line), "Bank %u, word %u, %u words", v.uhf_bank, v.uhf_word, v.uhf_count);
        label(8, 66, 304, line, 1, true);
        lines(8, 86, 304, chunk.data(), 4);
        const bool writable = v.reader_present && !v.inventory && selectable(v) && v.uhf_memory &&
            v.uhf_bank == 3 && v.uhf_count >= 1 && v.uhf_count <= 32;
        edit(8, 150, 148, 30, "Edit User bytes", Field::UhfValue, writable);
        action(164, 150, 148, 30, "Review User edit", Command::UhfPrepareUser, writable && v.uhf_value[0]);
        pages(page, total, true);
    } else if (page_ == Page::UhfConfig) {
        static const char* banks[]{"Reserved", "EPC", "TID", "User"};
        for (unsigned bank = 0; bank < 4; ++bank) {
            std::snprintf(line, sizeof(line), "%s%s", banks[bank], v.uhf_bank == bank ? " *" : "");
            set(8 + int(bank) * 77, 68, 73, 28, line, Field::UhfBank, bank, !v.inventory);
        }
        std::snprintf(line, sizeof(line), "Word: %u", v.uhf_word);
        edit(8, 102, 148, 28, line, Field::UhfWord, !v.inventory);
        std::snprintf(line, sizeof(line), "Words: %u", v.uhf_count);
        edit(164, 102, 148, 28, line, Field::UhfCount, !v.inventory);
        edit(8, 136, 148, 30, "Password: ****", Field::UhfPassword, !v.inventory);
        edit(164, 136, 148, 30, "New EPC", Field::UhfEpc, !v.inventory && selectable(v));
        action(8, 174, 148, 32, "Review EPC rename", Command::UhfPrepareEpc,
               v.reader_present && !v.inventory && selectable(v) && v.uhf_epc[0]);
        nav(164, 174, 148, 32, "< Memory", Page::UhfMemory);
    } else if (page_ == Page::UhfPower) {
        std::snprintf(line, sizeof(line), "Power target: %u dBm", v.uhf_power);
        label(8, 72, 304, line, 2);
        lines(8, 104, 304, "18-26 dBm only. Region remains unchanged. Read power before reviewing a change.", 3);
        action(8, 162, 96, 36, "Read", Command::UhfReadPower, v.reader_present && !v.inventory);
        edit(108, 162, 96, 36, "Set dBm", Field::UhfPower, v.reader_present && !v.inventory);
        action(208, 162, 104, 36, "Review", Command::UhfPreparePower, v.uhf_power_known && v.reader_present && !v.inventory);
    }
}
void ToolsUi::lf(const View& v) {
    nav(8, 34, 96, 28, "Scan", Page::LfScan);
    nav(108, 34, 96, 28, "Custom ID", Page::LfId);
    nav(208, 34, 104, 28, "Formats", Page::LfFormats);
    char line[140]{};
    if (page_ == Page::LfScan) {
        label(8, 68, 304, lf_name(v.lf_format), 2);
        lines(8, 96, 304, v.lf_present ? safe(v.lf_actual).data() : "No LF identity captured", 2);
        action(8, 136, 148, 34, "Scan LF", Command::LfScan, lf_scan_ok(v), 0, true);
        action(164, 136, 148, 34, "Use scanned ID", Command::LfCapture, v.lf_present);
        if (v.lf_format == 2) {
            std::snprintf(line, sizeof(line), "HID format hint: %u", v.lf_hint);
            edit(8, 178, 304, 30, line, Field::LfHint);
        } else label(8, 184, 304, lf_scan_ok(v) ? "Reader capability confirmed" : "Format / reader unavailable", 1, true);
    } else if (page_ == Page::LfId) {
        label(8, 66, 304, lf_name(v.lf_format));
        lines(8, 84, 304, safe(v.lf_value).data(), 2);
        edit(8, 120, 148, 30, "Edit ID", Field::LfId);
        action(164, 120, 148, 30, "Default test ID", Command::LfTestId);
        label(8, 156, 304, "Unprotected T5577 only; password-free", 1, true);
        action(8, 178, 304, 34, "Review T5577 write", Command::LfPrepareWrite,
               lf_write_ok(v) && v.lf_write_armed && v.lf_value[0]);
    } else if (page_ == Page::LfFormats) {
        const unsigned page = std::min(list_page_, 1U);
        for (unsigned row = 0; row < 4; ++row) {
            const unsigned format = page * 4 + row;
            std::snprintf(line, sizeof(line), "%s%s%s", lf_name(format), format == v.lf_format ? " *" : "",
                          format == 2 || format == 7 || !(v.lf_write_mask & (1U << format)) ? " (read only)" : "");
            set(8, 66 + int(row) * 29, 304, 27, line, Field::LfFormat, format,
                v.reader_present && (v.lf_scan_mask & (1U << format)));
        }
        pages(page, 2);
    }
}
void ToolsUi::layout(Surface* surface, const View& view) {
    surface_ = surface; hit_count_ = 0;
    light_ = view.light_theme;
    canvas_ = light_ ? 0xf1f5fa : 0x091321; panel_ = light_ ? 0xffffff : 0x14243a;
    text_ = light_ ? 0x15243b : 0xe6edf6; muted_ = light_ ? 0x52647a : 0x98a9be;
    accent_ = light_ ? 0x087c75 : 0x5ce0c1; border_ = light_ ? 0xc7d4e3 : 0x2b435d;
    danger_ = light_ ? 0xb92742 : 0xff8ba0;
    if (surface_) fill(0, 0, 320, 240, canvas_);
    if (view.tool != tool_) {
        editor_.clear(); header(view);
        label(8, 80, 304, "Waiting for tool state", 1, true);
        surface_ = nullptr; return;
    }
    if (view.review) {
        editor_.clear();
        label(8, 10, 304, safe(view.review_title).data(), 2);
        for (unsigned row = 0; row < 4; ++row)
            label(12, 52 + int(row) * 28, 296, safe(view.review_lines[row]).data());
        action(12, 184, 140, 44, "Cancel", Command::CancelReview);
        action(168, 184, 140, 44, "Write", Command::Confirm, view.review_token != 0 && !view.busy, 0, true);
    } else if (view.busy && !view.inventory) {
        editor_.clear(); header(view);
        label(12, 56, 296, "Working", 2);
        lines(12, 92, 296, safe(view.message).data(), 3);
        const unsigned amount = view.total ? unsigned(std::min<uint64_t>(296, uint64_t(view.done) * 296 / view.total)) : 0;
        fill(12, 148, 296, 12, panel_); if (amount) fill(12, 148, int(amount), 12, accent_);
        char value[48]; std::snprintf(value, sizeof(value), "%u / %u", view.done, view.total);
        label(12, 166, 296, value, 1, true);
        action(12, 194, 296, 34, "Stop", Command::Stop);
    } else if (editor_.active()) keyboard();
    else {
        header(view);
        if (view.tool != tool_) label(8, 80, 304, "Waiting for tool state", 1, true);
        else {
            if (tool_ == Tool::Hf) hf(view);
            else if (tool_ == Tool::Uhf) uhf(view);
            else if (tool_ == Tool::Lf) lf(view);
        }
        const auto old_text = text_;
        if (view.status != OK && view.status != CANCELLED && view.status != NO_CARD) text_ = danger_;
        label(8, 220, 220, view.message[0] ? safe(view.message).data() : status_name(view.status), 1, view.status == OK);
        text_ = old_text;
        action(240, 216, 72, 24, "Clear", Command::ClearSession, !view.inventory);
    }
    surface_ = nullptr;
}
void ToolsUi::paint(Surface& surface, const View& view, bool force) {
    if (!active()) return;
    if (!force && painted_ && !dirty_ && revision_ == view.revision &&
        battery_ == view.battery && painted_light_ == view.light_theme &&
        painted_review_ == view.review && painted_busy_ == view.busy &&
        painted_review_token_ == view.review_token) return;
    const bool new_review = view.review && (!painted_review_ || painted_review_token_ != view.review_token);
    damage_.paint(surface, [this, &view](Surface& target) { layout(&target, view); }, force || new_review);
    revision_ = view.revision; battery_ = view.battery;
    painted_light_ = view.light_theme; dirty_ = false; painted_ = true;
    painted_review_ = view.review && view.tool == tool_;
    painted_review_token_ = view.review_token; painted_busy_ = view.busy;
}
void ToolsUi::open_editor(Field field, const View& v) {
    editing_ = field;
    const char* title = "Value"; InputKind kind = InputKind::Hex;
    std::array<char, 257> initial{}; size_t limit = 256; bool secret = false;
    auto copy = [&](const auto& value) {
        const auto bounded = safe(value);
        std::snprintf(initial.data(), initial.size(), "%s", bounded.data());
    };
    switch (field) {
    case Field::HfUnit: title = "Memory unit"; kind = InputKind::Number; limit = 3;
        std::snprintf(initial.data(), initial.size(), "%u", v.hf_unit); break;
    case Field::HfValue: title = "Desired memory bytes"; limit = std::min(32U, v.hf_unit_size * 2); copy(v.hf_value[0] ? v.hf_value : v.hf_data); break;
    case Field::HfKey: title = "Supplied Classic key"; limit = 12; secret = true; break;
    case Field::NtagPassword: title = "NTAG password"; limit = 8; secret = true; break;
    case Field::NtagPack: title = "NTAG PACK"; limit = 4; secret = true; break;
    case Field::UhfWord: title = "Starting word 0-65535"; kind = InputKind::Number; limit = 5;
        std::snprintf(initial.data(), initial.size(), "%u", v.uhf_word); break;
    case Field::UhfCount: title = "Read words 1-64"; kind = InputKind::Number; limit = 2;
        std::snprintf(initial.data(), initial.size(), "%u", v.uhf_count); break;
    case Field::UhfValue: title = "Desired User memory"; limit = 128;
        if (v.uhf_value[0]) copy(v.uhf_value); else copy(v.uhf_data);
        break;
    case Field::UhfPassword: title = "UHF access password"; limit = 8; secret = true; break;
    case Field::UhfEpc: title = "New EPC (2-28 bytes)"; limit = 56; copy(v.uhf_epc);
        if (!initial[0] && selectable(v)) copy(epc_text(v.tags[v.selected_tag]));
        break;
    case Field::UhfPower: title = "RF power 18-26 dBm"; kind = InputKind::Number; limit = 2;
        std::snprintf(initial.data(), initial.size(), "%u", v.uhf_power); break;
    case Field::LfHint: title = "HID hint: 0-30 or 42"; kind = InputKind::Number; limit = 2;
        std::snprintf(initial.data(), initial.size(), "%u", v.lf_hint); break;
    case Field::LfId: {
        title = "Desired LF identity"; copy(v.lf_value);
        constexpr unsigned limits[]{10, 26, 26, 8, 32, 8, 10, 32};
        limit = v.lf_format < 8 ? limits[v.lf_format] : 32;
        if (v.lf_format == 5) kind = InputKind::Text;
        break;
    }
    default:
        if (unsigned(field) < unsigned(Field::NdefField0) || unsigned(field) > unsigned(Field::NdefField7)) return;
        const auto& value = v.ndef_fields[unsigned(field) - unsigned(Field::NdefField0)];
        kind = InputKind::Text; secret = value.secret;
        if (!secret) copy(value.value);
        editor_.open(safe(value.label).data(), secret ? "" : initial.data(), limit, kind, secret);
        local::secure_zero(initial.data(), initial.size()); dirty_ = true; return;
    }
    editor_.open(title, secret ? "" : initial.data(), limit, kind, secret);
    local::secure_zero(initial.data(), initial.size()); dirty_ = true;
}
Intent ToolsUi::touch(int x, int y, const View& view) {
    Intent out;
    if (!active() || x < 0 || y < 0 || x >= 320 || y >= 240) return out;
    if (view.tool != tool_ && editor_.active()) { editor_.clear(); dirty_ = true; }
    if (editor_.active() && !view.review && !view.busy) {
        const auto result = editor_.touch(x, y);
        if (result == InputResult::Accept) {
            out.command = Command::SetField; out.field = editing_;
            std::snprintf(out.text.data(), out.text.size(), "%s", editor_.value());
            editor_.clear(); dirty_ = true;
        } else if (result == InputResult::Cancel) { editor_.clear(); dirty_ = true; }
        else if (result == InputResult::Changed) dirty_ = true;
        return out;
    }
    // Rebuild from the current authoritative projection, never from old painted hit targets.
    layout(nullptr, view);
    for (size_t i = 0; i < hit_count_; ++i) {
        const Hit hit = hits_[i];
        if (x < hit.x || y < hit.y || x >= hit.x + hit.w || y >= hit.y + hit.h) continue;
        if (hit.kind == HitKind::Navigate) {
            page_ = hit.page; list_page_ = field_page_ = 0; dirty_ = true; return out;
        }
        if (hit.kind == HitKind::PageIndex) {
            if (hit.field == Field::NdefField0) field_page_ = hit.index;
            else list_page_ = hit.index;
            dirty_ = true; return out;
        }
        if (hit.kind == HitKind::Edit) { open_editor(hit.field, view); return out; }
        if (hit.kind == HitKind::Set || hit.kind == HitKind::Choice) {
            out.command = Command::SetField; out.field = hit.field;
            if (hit.kind == HitKind::Choice) {
                const auto& field = view.ndef_fields[hit.index];
                const unsigned count = std::min(4U, unsigned(field.choice_count));
                unsigned next = 0;
                for (unsigned choice = 0; choice < count; ++choice)
                    if (std::strcmp(safe(field.value).data(), safe(field.choices[choice]).data()) == 0) next = (choice + 1) % count;
                std::snprintf(out.text.data(), out.text.size(), "%s", safe(field.choices[next]).data());
            } else if (hit.value[0]) std::snprintf(out.text.data(), out.text.size(), "%s", hit.value);
            else std::snprintf(out.text.data(), out.text.size(), "%u", hit.index);
            if (hit.field == Field::NdefKind) {
                page_ = Page::NdefFields; field_page_ = 0; dirty_ = true;
            }
            return out;
        }
        out.command = hit.command; out.index = hit.index;
        if (out.command == Command::Confirm &&
            (!painted_review_ || painted_review_token_ != view.review_token)) return Intent{};
        if (out.command == Command::Confirm || out.command == Command::CancelReview) out.review_token = view.review_token;
        if (out.command == Command::UhfSelectTag) { page_ = Page::UhfMemory; dirty_ = true; }
        if (out.command == Command::Exit) close();
        return out;
    }
    return out;
}
}
