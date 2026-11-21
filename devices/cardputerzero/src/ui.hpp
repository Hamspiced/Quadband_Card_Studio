#pragma once
#include "contract.hpp"
#include <memory>

struct _lv_obj_t;
namespace qb::zero {
class ApplicationUi {
public:
    ApplicationUi(_lv_obj_t* root, Callbacks);
    ~ApplicationUi();
    ApplicationUi(const ApplicationUi&) = delete;
    ApplicationUi& operator=(const ApplicationUi&) = delete;
    void update(const Snapshot&);
    void key(const KeyEvent&);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
