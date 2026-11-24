#include "ui.hpp"
#include "lvgl.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace qb;
using namespace qb::zero;
using local::Command;
using local::Tool;
namespace {
std::array<uint16_t,320*20> buffer{};
void flush(lv_display_t* display,const lv_area_t*,uint8_t*) { lv_display_flush_ready(display); }
std::string caption(lv_obj_t* object) {
    if(lv_obj_check_type(object,&lv_label_class)) return lv_label_get_text(object);
    std::string result;
    for(uint32_t n=0;n<lv_obj_get_child_count(object);++n) result+=caption(lv_obj_get_child(object,int32_t(n)));
    return result;
}
lv_obj_t* type(lv_obj_t* object,const lv_obj_class_t* klass) {
    if(lv_obj_check_type(object,klass)) return object;
    for(uint32_t n=0;n<lv_obj_get_child_count(object);++n)
        if(auto* child=type(lv_obj_get_child(object,int32_t(n)),klass)) return child;
    return nullptr;
}
lv_obj_t* button(lv_obj_t* object,const std::string& fragment) {
    if(lv_obj_check_type(object,&lv_button_class)) {
        auto text=caption(object);
        std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(text.find(fragment)!=std::string::npos) return object;
    }
    for(uint32_t n=0;n<lv_obj_get_child_count(object);++n)
        if(auto* child=button(lv_obj_get_child(object,int32_t(n)),fragment)) return child;
    return nullptr;
}
void visible(lv_obj_t* object) {
    assert(object); lv_obj_update_layout(lv_screen_active());
    lv_area_t bounds; lv_obj_get_coords(object,&bounds);
    assert(bounds.x1>=0 && bounds.y1>=0 && bounds.x2<320 && bounds.y2<170);
    assert(lv_obj_is_visible(object));
    for(auto* parent=lv_obj_get_parent(object);parent;parent=lv_obj_get_parent(parent)) {
        lv_area_t clip; lv_obj_get_coords(parent,&clip);
        if (!(bounds.x1>=clip.x1 && bounds.y1>=clip.y1 && bounds.x2<=clip.x2 && bounds.y2<=clip.y2))
            std::fprintf(stderr,"Control '%s' [%d,%d..%d,%d] exceeds parent [%d,%d..%d,%d], overflow=%d\n",
                caption(object).c_str(),int(bounds.x1),int(bounds.y1),int(bounds.x2),int(bounds.y2),
                int(clip.x1),int(clip.y1),int(clip.x2),int(clip.y2),
                int(lv_obj_has_flag(parent,LV_OBJ_FLAG_OVERFLOW_VISIBLE)));
        assert(bounds.x1>=clip.x1 && bounds.y1>=clip.y1 && bounds.x2<=clip.x2 && bounds.y2<=clip.y2);
    }
}
struct Fixture {
    Snapshot state;
    std::vector<local::Intent> calls;
    unsigned stops=0,quits=0,detections=0,applied=0,applied_stops=0;
    std::unique_ptr<ApplicationUi> ui;
    Fixture() {
        state.connected=true; state.simulated=true; state.view.reader_present=true;
        state.view.hf_ready=state.view.hf_unit_known=state.view.hf_writable=true;
        state.view.hf_family=local::HfFamily::Ntag213;
        state.view.hf_units=45; state.view.hf_unit=4; state.view.hf_unit_size=4;
        state.view.lf_scan_mask=state.view.lf_write_mask=255;
        std::snprintf(state.view.hf_data.data(),state.view.hf_data.size(),"00000000");
        std::snprintf(state.view.hf_value.data(),state.view.hf_value.size(),"01020304");
        Callbacks callbacks;
        callbacks.submit=[&](const local::Intent& intent){calls.push_back(intent);return true;};
        callbacks.connect=[](const ReaderConfig&){return true;};
        callbacks.stop=[&]{++stops;};
        callbacks.quit=[&]{++quits;};
        callbacks.detect=[&](const ReaderConfig&){++detections;return true;};
        ui=std::make_unique<ApplicationUi>(lv_screen_active(),std::move(callbacks)); publish();
    }
    ~Fixture(){ui.reset();}
    void publish(){++state.view.revision;ui->update(state);lv_refr_now(nullptr);}
    void press(Key key,char c=0,bool repeat=false){ui->key({key,c,true,repeat});}
    void release(Key key,char c=0){ui->key({key,c,false});}
    void apply() {
        while(applied<calls.size()) {
            switch(calls[applied++].command) {
            case Command::OpenHf:state.view.tool=Tool::Hf;break;
            case Command::OpenUhf:state.view.tool=Tool::Uhf;break;
            case Command::OpenLf:state.view.tool=Tool::Lf;break;
            case Command::Exit:state.view.tool=Tool::None;break;
            default:break;
            }
        }
        if(applied_stops!=stops){applied_stops=stops;state.view.inventory=state.view.busy=false;}
        publish();
    }
    void tap(Key key,char c=0){press(key,c);release(key,c);apply();}
    void key(char c){tap(Key::Character,c);}
    void primary(Command expected) {
        auto* control=button(lv_screen_active(),expected==Command::UhfStart?"[r] start":expected==Command::LfScan?"[r] scan":"[r] read");
        visible(control);
        auto* group=static_cast<lv_group_t*>(lv_obj_get_group(control));assert(group);
        assert(lv_group_get_focused(group)==control);
    }
    void focus(lv_obj_t* target) {
        auto* group=static_cast<lv_group_t*>(lv_obj_get_group(target));assert(group);
        for(unsigned n=0;lv_group_get_focused(group)!=target && n<64;++n)tap(Key::Tab);
        assert(lv_group_get_focused(group)==target);
    }
};
void zero_arrow_label_navigation() {
    Fixture f;
    auto* first=button(lv_screen_active(),"[1] hf / nfc");
    auto* second=button(lv_screen_active(),"[2] uhf");
    assert(first && second);
    auto* group=static_cast<lv_group_t*>(lv_obj_get_group(first));
    assert(group && lv_group_get_focused(group)==first);
    f.key('x');assert(lv_group_get_focused(group)==second);
    f.key('f');assert(lv_group_get_focused(group)==first);
    f.key('c');assert(lv_group_get_focused(group)==second);
    f.key('z');assert(lv_group_get_focused(group)==first);
    f.tap(Key::Down);assert(lv_group_get_focused(group)==second);
    f.tap(Key::Up);assert(lv_group_get_focused(group)==first);
    assert(f.calls.empty() && f.detections==0 && f.stops==0 && f.quits==0);
    f.key('s');
    auto* connections=button(lv_screen_active(),"connections");
    group=static_cast<lv_group_t*>(lv_obj_get_group(connections));
    assert(group);
    f.focus(connections);
    f.key('x');assert(lv_group_get_focused(group)!=connections);
    f.key('f');assert(lv_group_get_focused(group)==connections);
    assert(f.calls.empty() && f.detections==0);
    f.key('1');const auto before=f.calls.size();
    f.key('c');
    assert(caption(lv_screen_active()).find("HF RAM copy")!=std::string::npos);
    assert(f.calls.size()==before);
    f.key('m');f.key('n');f.key('f');
    assert(caption(lv_screen_active()).find("NDEF fields")!=std::string::npos);
    assert(f.calls.size()==before);
}
void missing_readers_and_long_results() {
    Fixture f;f.state.simulated=false;f.state.connected=false;f.publish();
    f.key('1');f.key('2');f.key('3');assert(f.calls.empty());
    f.state.connected=true;
    f.state.config.hf_i2c="/dev/i2c-1";f.state.config.uhf_serial="/dev/serial0";
    f.state.discovery.completed=true;f.state.discovery.sequence=1;
    f.state.discovery.config=f.state.config;
    f.state.discovery.devices[0]={Detection::Missing,"/dev/serial0","",""};
    f.state.discovery.devices[1]={Detection::Unsupported,"/dev/i2c-1","",""};
    f.publish();f.key('1');f.key('2');f.key('3');assert(f.calls.empty());
    f.state.discovery.devices[0].state=Detection::Detected;
    f.state.discovery.devices[1].state=Detection::Detected;
    f.state.view.identity.fill('W');f.state.view.hf_data.fill('F');f.state.view.hf_value.fill('A');
    f.publish();f.key('1');f.primary(Command::HfRead);
    visible(button(lv_screen_active(),"bytes"));visible(button(lv_screen_active(),"review"));
    assert(f.calls.size()==1 && f.calls.back().command==Command::OpenHf);
}
void manual_recovery_after_discovery() {
    Fixture f;f.state.simulated=false;
    f.state.discovery.completed=true;
    f.state.discovery.devices[0]={Detection::Blocked,"/dev/serial0","",""};
    f.state.discovery.devices[1]={Detection::Missing,"/dev/i2c-1","",""};
    f.publish();f.key('1');f.key('2');assert(f.calls.empty());
    f.state.config.hf_i2c="/dev/i2c-1";f.state.config.uhf_serial="/dev/serial0";
    f.publish();
    assert(button(lv_screen_active(),"hf / nfc  configured"));
    f.key('1');f.primary(Command::HfRead);
    assert(f.calls.size()==1 && f.calls.back().command==Command::OpenHf);
    f.key('2');f.primary(Command::UhfStart);
    assert(f.calls.size()==2 && f.calls.back().command==Command::OpenUhf);
}
void direct_workflows() {
    Fixture f;
    f.key('1');assert(f.calls.size()==1 && f.calls.back().command==Command::OpenHf);
    f.primary(Command::HfRead);
    f.key('r');assert(f.calls.size()==2 && f.calls.back().command==Command::HfRead);
    const auto count=f.calls.size();f.key('1');assert(f.calls.size()==count);
    f.tap(Key::Enter);assert(f.calls.back().command==Command::HfRead);
    f.key('2');assert(f.calls.back().command==Command::OpenUhf);
    f.primary(Command::UhfStart);f.key('r');assert(f.calls.back().command==Command::UhfStart);
    f.state.view.tag_count=1;f.state.view.tags[0].size=4;f.state.view.tags[0].reads=8;
    f.state.view.tags[0].epc={0x11,0x22,0x33,0x44};
    f.state.view.inventory=f.state.view.busy=true;f.publish();
    const auto scanning_calls=f.calls.size();f.key('1');f.key('s');f.key('h');f.key('w');
    assert(f.calls.size()==scanning_calls && f.detections==0);
    visible(button(lv_screen_active(),"stop"));
    f.key('r');assert(f.stops==1 && f.state.view.tag_count==1);
    f.key('2');assert(f.calls.size()==scanning_calls && f.state.view.tags[0].reads==8);
    f.key('p');f.primary(Command::UhfReadPower);f.key('r');
    assert(f.calls.back().command==Command::UhfReadPower);
    f.key('3');assert(f.calls.back().command==Command::OpenLf);
    f.primary(Command::LfScan);f.key('r');assert(f.calls.back().command==Command::LfScan);
    f.state.view.lf_present=true;f.state.view.lf_card_valid=false;f.publish();
    visible(button(lv_screen_active(),"[c] capture"));f.key('c');
    assert(f.calls.back().command==Command::LfCapture);
}
void editor_and_busy_isolation() {
    Fixture f;f.key('1');f.key('e');
    auto* editor=type(lv_screen_active(),&lv_textarea_class);assert(editor);
    const auto before=f.calls.size();f.tap(Key::Home);
    for(unsigned n=0;n<32;++n)f.tap(Key::Delete);
    f.key('1');f.key('2');f.key('3');f.key('4');
    assert(f.calls.size()==before && f.state.view.tool==Tool::Hf);
    assert(std::string(lv_textarea_get_text(editor))=="1234");
    f.tap(Key::Escape);assert(f.calls.size()==before);
    f.state.pending=true;f.state.view.busy=true;f.publish();
    f.key('1');f.key('2');f.key('3');f.key('e');f.key('w');f.key('s');f.key('h');
    assert(f.calls.size()==before && f.detections==0);
    visible(button(lv_screen_active(),"stop"));f.tap(Key::Stop);assert(f.stops==1);
}
void review_gate() {
    Fixture f;f.key('1');
    f.press(Key::Enter);
    f.state.view.review=true;f.state.view.review_token=100;
    std::snprintf(f.state.view.review_title.data(),f.state.view.review_title.size(),"Write HF data");
    for(auto& line:f.state.view.review_lines)line.fill('W');
    f.publish();
    auto* cancel=button(lv_screen_active(),"cancel");auto* confirm=button(lv_screen_active(),"confirm");
    visible(cancel);visible(confirm);
    auto* group=static_cast<lv_group_t*>(lv_obj_get_group(cancel));assert(group && lv_group_get_focused(group)==cancel);
    const auto before=f.calls.size();
    for(char c:std::string("123rw"))f.key(c);
    assert(f.calls.size()==before);
    f.key('c');assert(lv_group_get_focused(group)==confirm && f.calls.size()==before);
    f.key('z');assert(lv_group_get_focused(group)==cancel && f.calls.size()==before);
    f.focus(confirm);f.press(Key::Enter,0,true);f.press(Key::Enter);assert(f.calls.size()==before);
    f.release(Key::Enter);f.tap(Key::Enter);
    assert(f.calls.size()==before+1 && f.calls.back().command==Command::Confirm && f.calls.back().review_token==100);
    f.press(Key::Enter,0,true);f.release(Key::Enter);assert(f.calls.size()==before+1);
    f.state.view.review_token=101;f.publish();
    cancel=button(lv_screen_active(),"cancel");confirm=button(lv_screen_active(),"confirm");
    visible(cancel);visible(confirm);
    assert(lv_group_get_focused(static_cast<lv_group_t*>(lv_obj_get_group(cancel)))==cancel);
    f.key('w');assert(f.calls.size()==before+1);
}
}
int main() {
    lv_init();auto* display=lv_display_create(320,170);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,buffer.data(),nullptr,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    zero_arrow_label_navigation();direct_workflows();editor_and_busy_isolation();missing_readers_and_long_results();manual_recovery_after_discovery();review_gate();
    lv_display_delete(display);lv_deinit();
    std::puts("Independent acceptance: direct reader+R workflows, visible primary/review actions, same-reader result retention, editor/busy isolation and held/repeat/token write gates passed.");
}
