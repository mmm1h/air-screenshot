// Selection-mask and lightweight annotation behavior is adapted from
// xland/ScreenCapture main@1574683043fa5f64b6cd45d9ec2e0db1bafbc15b.
// This implementation was substantially rewritten for Air Screenshot.

#include "airshot/overlay.h"

#include "overlay_session.h"
#include "overlay_window.h"

namespace airshot {

struct RegionCaptureSession::Impl {
    std::unique_ptr<overlay_detail::OverlaySession> session;
};

RegionCaptureSession::RegionCaptureSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RegionCaptureSession::~RegionCaptureSession() = default;

void RegionCaptureSession::cancel() {
    if (impl_ && impl_->session) {
        impl_->session->cancel();
    }
}

bool RegionCaptureSession::active() const noexcept {
    return impl_ && impl_->session && impl_->session->active();
}

std::unique_ptr<RegionCaptureSession> start_region_capture(
    RegionRequest request, RegionCaptureCompletion completion) {
    auto impl = std::make_unique<RegionCaptureSession::Impl>();
    impl->session =
        std::make_unique<overlay_detail::OverlaySession>(std::move(request), std::move(completion));
    auto result = std::unique_ptr<RegionCaptureSession>(new RegionCaptureSession(std::move(impl)));
    if (!result->impl_->session->start()) {
        return nullptr;
    }
    return result;
}

RegionResult run_region_capture(const RegionRequest& request) {
    overlay_detail::OverlaySession session(request);
    return session.run();
}

}  // namespace airshot
