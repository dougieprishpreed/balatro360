// Reference counted base class for every object exposed to Lua
// (Image, Source, Channel, Joystick, ...). Objects may be shared between
// Lua states (love.thread), so the count is atomic.
#ifndef B360_OBJECT_H
#define B360_OBJECT_H

#include "../platform/platform.h"

namespace rt {

class Object {
public:
    Object() : refs(1) {}
    virtual ~Object() {}

    void retain() { plat::atomicInc(&refs); }
    void release() {
        if (plat::atomicDec(&refs) == 0) delete this;
    }

private:
    Object(const Object&);
    Object& operator=(const Object&);
    volatile i32 refs;
};

// Holds a reference for the lifetime of the pointer wrapper.
template <class T>
class Ref {
public:
    Ref() : p(0) {}
    Ref(T* o) : p(o) { if (p) p->retain(); }
    Ref(const Ref& o) : p(o.p) { if (p) p->retain(); }
    ~Ref() { if (p) p->release(); }
    Ref& operator=(const Ref& o) { set(o.p); return *this; }
    Ref& operator=(T* o) { set(o); return *this; }
    void set(T* o) {
        if (o) o->retain();
        if (p) p->release();
        p = o;
    }
    T* get() const { return p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
private:
    T* p;
};

}  // namespace rt

#endif
