#include "KeyboardBackend.h"

namespace op::input {

KeyboardBackend::KeyboardBackend() : _hwnd(0), _mode(0) {
}

KeyboardBackend::~KeyboardBackend() {
    UnBind();
}

long KeyboardBackend::UnBind() {
    _hwnd = NULL;
    _mode = 0;
    return 1;
}

} // namespace op::input
