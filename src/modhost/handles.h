// Kernel objects behind the HANDLEs mod code gets (files, find searches, events, semaphores,
// threads). Only touched with the x86 lock held.
#pragma once

#include "modhost/x86.h"

#include <memory>

namespace mh {

struct KObject {
    virtual ~KObject() = default;
    // Waitable objects: whether a wait on it would end now; consume() takes it (auto-reset
    // events, semaphores).
    virtual bool signaled() { return false; }
    virtual void consume() {}
};

u32 handleNew(std::shared_ptr<KObject> o);
std::shared_ptr<KObject> handleGet(u32 h);
template <typename T>
std::shared_ptr<T> handleAs(u32 h) {
    return std::dynamic_pointer_cast<T>(handleGet(h));
}
bool handleClose(u32 h);

}  // namespace mh
