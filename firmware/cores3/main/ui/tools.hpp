#pragma once
#include "input.hpp"
#include "repaint.hpp"
#include "../local_model.hpp"

namespace qb::ui {
class Surface;
class ToolsUi {
public:
    void open(local::Tool);
    void close();
    bool active() const { return tool_ != local::Tool::None; }
    void paint(Surface&, const local::View&, bool force = false);
    local::Intent touch(int x, int y, const local::View&);
private:
    enum class Page : uint8_t {
        HfMemory, HfAuth, HfNtagAuth, HfCopy, NdefRecords, NdefKinds, NdefFields,
        UhfTags, UhfMemory, UhfData, UhfConfig, UhfPower, LfScan, LfId, LfFormats,
        NdefBulk, NdefQueue
    };
    enum class HitKind : uint8_t { Command, Navigate, Edit, Set, PageIndex, Choice };
    struct Hit {
        int x = 0, y = 0, w = 0, h = 0;
        HitKind kind = HitKind::Command;
        local::Command command = local::Command::None;
        local::Field field = local::Field::HfUnit;
        unsigned index = 0;
        Page page = Page::HfMemory;
        char value[32]{};
    };
    static constexpr size_t max_hits = 28;
    std::array<Hit, max_hits> hits_{};
    size_t hit_count_ = 0;
    local::Tool tool_ = local::Tool::None;
    Page page_ = Page::HfMemory;
    unsigned list_page_ = 0, field_page_ = 0;
    InputEditor editor_;
    SceneDamage damage_;
    local::Field editing_ = local::Field::HfValue;
    bool dirty_ = true, painted_ = false;
    uint64_t revision_ = 0, painted_review_token_ = 0;
    bool painted_review_ = false, painted_busy_ = false;
    int battery_ = -1;
    bool light_ = false, painted_light_ = false;
    Surface* surface_ = nullptr;
    uint32_t canvas_ = 0, panel_ = 0, text_ = 0, muted_ = 0, accent_ = 0, border_ = 0, danger_ = 0;
    void layout(Surface*, const local::View&);
    void label(int x, int y, int width, const char*, unsigned style = 1,
               bool muted = false, uint32_t background = UINT32_MAX);
    void lines(int x, int y, int width, const char*, unsigned count, unsigned style = 1);
    void fill(int x, int y, int w, int h, uint32_t);
    void button(int x, int y, int w, int h, const char*, const Hit&, bool enabled = true, bool primary = false);
    void action(int x, int y, int w, int h, const char*, local::Command, bool enabled = true, unsigned index = 0, bool primary = false);
    void nav(int x, int y, int w, int h, const char*, Page);
    void edit(int x, int y, int w, int h, const char*, local::Field, bool enabled = true);
    void set(int x, int y, int w, int h, const char*, local::Field, unsigned value, bool enabled = true);
    void pages(unsigned current, unsigned total, bool fields = false);
    void header(const local::View&);
    void keyboard();
    void hf(const local::View&);
    void uhf(const local::View&);
    void lf(const local::View&);
    void open_editor(local::Field, const local::View&);
};
}
