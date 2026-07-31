#include "airshot/capture.h"
#include "capture_uia.h"

#include <dwmapi.h>

#include <chrono>
#include <iostream>
#include <limits>
#include <thread>

namespace {

bool expect(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
    }
    return condition;
}

bool test_mailbox_policy() {
    using namespace airshot::capture_uia;

    MailboxPolicy policy;
    const HWND root = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(1));
    const airshot::RectI root_bounds{0, 0, 400, 300};
    if (!expect(
            queue_latest(
                policy,
                {20, 30},
                root,
                root_bounds,
                1'000),
            L"first UIA request is queued")) {
        return false;
    }
    const auto first = take_latest(policy);
    if (!expect(
            first.has_value(),
            L"worker takes the pending UIA request")) {
        return false;
    }
    if (!expect(
            !queue_latest(
                policy,
                {22, 31},
                root,
                root_bounds,
                1'020),
            L"nearby mouse events are coalesced")) {
        return false;
    }
    if (!expect(
            queue_latest(
                policy,
                {80, 90},
                root,
                root_bounds,
                1'030),
            L"a materially newer mouse point replaces the request")) {
        return false;
    }
    const auto newest = take_latest(policy);
    if (!expect(
            newest && newest->point.x == 80 && newest->point.y == 90,
            L"worker receives only the newest coalesced point") ||
        !expect(
            !may_publish(policy, *first, 1'040),
            L"stale in-flight results are dropped") ||
        !expect(
            may_publish(policy, *newest, 1'040),
            L"current result is publishable within the provider budget") ||
        !expect(
            !may_publish(
                policy,
                *newest,
                newest->submitted_at +
                    kProviderBudgetMilliseconds + 1),
            L"late provider results are dropped")) {
        return false;
    }

    const std::uint64_t previous_session = policy.session;
    static_cast<void>(begin_session(policy));
    return expect(
               policy.session != previous_session &&
                   !policy.has_pending && !policy.has_newest,
               L"new capture invalidates requests and cached identity") &&
           expect(
               !may_publish(policy, *newest, 1'050),
               L"results from a previous screenshot cannot publish");
}

bool test_rect_policy() {
    using airshot::capture_uia::sanitize_provider_rect;
    const airshot::RectI root{10, 20, 310, 220};
    const airshot::RectI desktop{0, 0, 800, 600};
    const POINT point{50, 60};

    const auto clipped = sanitize_provider_rect(
        -100.5,
        -100.5,
        250.75,
        250.75,
        root,
        desktop,
        point);
    if (!expect(
            clipped && clipped->left == root.left &&
                clipped->top == root.top && clipped->right == 151 &&
                clipped->bottom == 151,
            L"provider rectangles are rounded and clipped to the target root")) {
        return false;
    }
    return expect(
               !sanitize_provider_rect(
                   std::numeric_limits<double>::quiet_NaN(),
                   0.0,
                   20.0,
                   20.0,
                   root,
                   desktop,
                   point),
               L"non-finite provider geometry is rejected") &&
           expect(
               !sanitize_provider_rect(
                   48.0,
                   58.0,
                   4.0,
                   4.0,
                   root,
                   desktop,
                   point),
               L"tiny UIA targets are rejected") &&
           expect(
               !sanitize_provider_rect(
                   200.0,
                   100.0,
                   20.0,
                   20.0,
                   root,
                   desktop,
                   point),
               L"rectangles that miss the pointer are rejected") &&
           expect(
               !sanitize_provider_rect(
                   -100'000.0,
                   -100'000.0,
                   200'000.0,
                   200'000.0,
                   {-100'000, -100'000, 100'000, 100'000},
                   {-100'000, -100'000, 100'000, 100'000},
                   point),
               L"oversized provider rectangles are rejected");
}

bool test_cache_policy() {
    using namespace airshot::capture_uia;
    const HWND root = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(2));
    CandidateChain cache;
    cache.root = root;
    cache.point = {50, 50};
    cache.session = 7;
    cache.sequence = 9;
    cache.completed_at = 1'000;
    cache.candidates[0] = {10, 10, 100, 100};
    cache.count = 1;

    return expect(
               may_reuse_cache(
                   cache,
                   {54, 47},
                   root,
                   7,
                   1'010),
               L"nearby points reuse a fresh UIA leaf") &&
           expect(
               !may_reuse_cache(
                   cache,
                   {55, 50},
                   root,
                   7,
                   1'010),
               L"movement inside a leaf still refreshes nested UIA targets") &&
           expect(
               !may_reuse_cache(
                   cache,
                   {50, 50},
                   root,
                   8,
                   1'010),
               L"a different capture session cannot reuse UIA cache") &&
           expect(
               !may_reuse_cache(
                   cache,
                   {50, 50},
                   root,
                   7,
                   1'000 + kCacheLifetimeMilliseconds + 1),
               L"expired UIA cache is not reused");
}

LRESULT CALLBACK fixture_window_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
    return DefWindowProcW(window, message, w_param, l_param);
}

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

int test_standard_uia_window() {
    constexpr wchar_t class_name[] = L"AirshotUiaFixture.Window";
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = fixture_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = class_name;
    if (!RegisterClassExW(&window_class)) {
        return 77;
    }

    HWND window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        class_name,
        L"UI Automation capture fixture",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        120,
        120,
        420,
        260,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!window) {
        UnregisterClassW(class_name, instance);
        return 77;
    }
    HWND button = CreateWindowExW(
        0,
        L"BUTTON",
        L"UIA target",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        48,
        64,
        180,
        48,
        window,
        nullptr,
        instance,
        nullptr);
    if (!button) {
        DestroyWindow(window);
        UnregisterClassW(class_name, instance);
        return 77;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    UpdateWindow(button);
    SetWindowPos(
        window,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    DwmFlush();

    RECT native_root{};
    RECT native_button{};
    if (!GetWindowRect(window, &native_root) ||
        !GetWindowRect(button, &native_button)) {
        DestroyWindow(window);
        UnregisterClassW(class_name, instance);
        return 77;
    }
    const airshot::RectI root_bounds =
        airshot::RectI::from_native(native_root).normalized();
    const POINT point{
        native_button.left +
            (native_button.right - native_button.left) / 2,
        native_button.top +
            (native_button.bottom - native_button.top) / 2,
    };

    airshot::capture_uia::begin_candidate_session();
    airshot::capture_uia::CandidateChain chain;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    do {
        chain = airshot::capture_uia::cached_chain_and_request(
            point,
            window,
            root_bounds);
        if (chain.count != 0) {
            break;
        }
        pump_messages();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    DestroyWindow(window);
    UnregisterClassW(class_name, instance);
    if (chain.count == 0) {
        std::wcout
            << L"UI Automation is unavailable in this desktop session.\n";
        return 77;
    }
    if (!expect(
            chain.root == window &&
                chain.count <=
                    airshot::capture_uia::kMaxCandidateCount,
            L"standard UIA window returns a bounded candidate chain") ||
        !expect(
            chain.candidates[0].contains(point),
            L"UIA leaf candidate contains the requested point")) {
        return 1;
    }
    for (std::size_t index = 0; index < chain.count; ++index) {
        const airshot::RectI& candidate = chain.candidates[index];
        if (!expect(
                candidate.left >= root_bounds.left &&
                    candidate.top >= root_bounds.top &&
                    candidate.right <= root_bounds.right &&
                    candidate.bottom <= root_bounds.bottom,
                L"UIA candidate remains inside the selected root")) {
            return 1;
        }
    }
    return 0;
}

}  // namespace

int wmain() {
    if (!test_mailbox_policy() || !test_rect_policy() ||
        !test_cache_policy()) {
        return 1;
    }
    const int integration_result = test_standard_uia_window();
    if (integration_result == 0) {
        std::wcout << L"UI Automation capture tests passed.\n";
    }
    return integration_result;
}
