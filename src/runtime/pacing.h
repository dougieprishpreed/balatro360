// Frame pacing for love.graphics.present().
//
// The 360 shows a new frame on a vertical blank (60 Hz). A game that needs a
// bit more than 16.7 ms per frame alternates between 16.7 and 33.3 ms frames,
// which looks worse than a steady 30. The pacer presents every vblank (60),
// every second vblank (30), or picks between the two ("auto"): it drops to 30
// when frames keep missing 60 Hz and goes back up once the CPU time per frame
// has stayed comfortably below 16.7 ms for a while.
#ifndef B360_PACING_H
#define B360_PACING_H

namespace gfx {
class Device;
}

namespace rt {

class FramePacer {
public:
    enum Mode { MODE_VSYNC = 0, MODE_60, MODE_30, MODE_AUTO };

    FramePacer();
    void setMode(gfx::Device* dev, Mode m);
    Mode mode() const { return mode_; }

    // Around the device's present().
    void beforePresent(gfx::Device* dev);
    void afterPresent(gfx::Device* dev);

    int interval() const { return interval_; }   // vblanks per frame
    double cpuMs() const;                         // recent average CPU time per frame

private:
    enum { HISTORY = 120 };
    void setInterval(gfx::Device* dev, int n);
    void clearHistory();

    Mode mode_;
    int interval_;
    bool native_;          // the device paces itself
    double lastEnd_;       // when the previous present() returned
    double lastSwitch_;    // when we last went from 30 to 60
    double work_[HISTORY]; // CPU time of recent frames (s)
    double frame_[HISTORY];// present-to-present time of recent frames (s)
    int count_, pos_;
    int framesAt30_;
    int upWait_;           // frames to stay at 30 before trying 60 again
};

}  // namespace rt

#endif
