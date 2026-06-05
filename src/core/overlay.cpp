// Selection-mask and lightweight annotation behavior is adapted from
// xland/ScreenCapture main@1574683043fa5f64b6cd45d9ec2e0db1bafbc15b.
// This implementation was substantially rewritten for Air Screenshot.

#include "airshot/overlay.h"

#include "overlay_session.h"
#include "overlay_window.h"

namespace airshot {

RegionResult run_region_capture(const RegionRequest& request) {
    overlay_detail::OverlaySession session(request);
    RegionResult result = session.run();
    overlay_detail::release_overlay_factories();
    return result;
}

}  // namespace airshot
