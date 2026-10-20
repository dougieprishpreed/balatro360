#include "pacing.h"

#include "../gfx/device.h"
#include "../platform/platform.h"

namespace rt {

namespace {

const double kVblank = 1.0 / 60.0;
const int kMissWindow = 60;    // frames looked at when deciding to drop to 30
const int kMissLimit = 8;      // ... and how many of them may miss 60 Hz
const int kUpWindow = 90;      // frames looked at when deciding to go back to 60
const double kUpBudget = 0.8 * kVblank;   // CPU time that leaves room for 60
const int kUpWaitMin = 90;     // 3 s at 30 fps
const int kUpWaitMax = 1800;   // 60 s

}  // namespace

FramePacer::FramePacer()
    : mode_(MODE_VSYNC), interval_(1), native_(false), lastEnd_(0), lastSwitch_(-1e9), count_(0), pos_(0),
      framesAt30_(0), upWait_(kUpWaitMin) {}

void FramePacer::clearHistory() {
    count_ = 0;
    pos_ = 0;
}

void FramePacer::setInterval(gfx::Device* dev, int n) {
    interval_ = n;
    native_ = dev->setPresentInterval(n);
    clearHistory();
    framesAt30_ = 0;
}

void FramePacer::setMode(gfx::Device* dev, Mode m) {
    mode_ = m;
    upWait_ = kUpWaitMin;
    setInterval(dev, m == MODE_30 ? 2 : 1);
}

double FramePacer::cpuMs() const {
    int n = count_ < 30 ? count_ : 30;
    if (n == 0) return 0;
    double sum = 0;
    for (int i = 1; i <= n; ++i) sum += work_[(pos_ - i + HISTORY) % HISTORY];
    return sum / n * 1000.0;
}

void FramePacer::beforePresent(gfx::Device* dev) {
    (void)dev;
    double now = plat::getTime();
    if (lastEnd_ > 0) {
        work_[pos_] = now - lastEnd_;
        frame_[pos_] = 0;
    }
    // Backends that can't present every other vblank: wait out the rest of
    // the 30 Hz frame ourselves.
    if (!native_ && interval_ > 1 && lastEnd_ > 0) {
        double target = lastEnd_ + interval_ * kVblank - 0.001;
        if (now < target) plat::sleep(target - now);
    }
}

void FramePacer::afterPresent(gfx::Device* dev) {
    double now = plat::getTime();
    if (lastEnd_ <= 0) {
        lastEnd_ = now;
        return;
    }
    frame_[pos_] = now - lastEnd_;
    lastEnd_ = now;
    pos_ = (pos_ + 1) % HISTORY;
    if (count_ < HISTORY) ++count_;
    if (mode_ != MODE_AUTO) return;

    if (interval_ == 1) {
        if (count_ < kMissWindow / 2) return;
        int n = count_ < kMissWindow ? count_ : kMissWindow;
        int missed = 0;
        for (int i = 1; i <= n; ++i)
            if (frame_[(pos_ - i + HISTORY) % HISTORY] > 1.25 * kVblank) ++missed;
        if (missed < kMissLimit) return;
        // Dropping again soon after the last attempt at 60: wait longer
        // before the next one, so a game that can't hold 60 doesn't keep
        // switching back and forth.
        if (now - lastSwitch_ < 10.0) {
            upWait_ *= 2;
            if (upWait_ > kUpWaitMax) upWait_ = kUpWaitMax;
        } else {
            upWait_ = kUpWaitMin;
        }
        plat::log("[pace] 30 fps (%d of the last %d frames missed 60 Hz, CPU %.1f ms/frame)", missed, n, cpuMs());
        setInterval(dev, 2);
        return;
    }

    if (++framesAt30_ < upWait_ || count_ < kUpWindow) return;
    int over = 0;
    for (int i = 1; i <= kUpWindow; ++i)
        if (work_[(pos_ - i + HISTORY) % HISTORY] > kUpBudget) ++over;
    if (over > 1) return;
    plat::log("[pace] 60 fps (CPU %.1f ms/frame)", cpuMs());
    lastSwitch_ = now;
    setInterval(dev, 1);
}

}  // namespace rt
