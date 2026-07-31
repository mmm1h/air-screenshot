#include "capture_uia.h"

#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace airshot::capture_uia {
namespace {

using Microsoft::WRL::ComPtr;

struct ServiceState {
    ServiceState() noexcept { InitializeSRWLock(&lock); }

    SRWLOCK lock{};
    HANDLE wake_event{};
    MailboxPolicy policy;
    CandidateChain cache;
    bool has_cache{};
};

[[nodiscard]] std::uint64_t elapsed_since(
    std::uint64_t now,
    std::uint64_t then) noexcept {
    return now >= then
               ? now - then
               : std::numeric_limits<std::uint64_t>::max();
}

[[nodiscard]] bool same_rect(
    const RectI& left,
    const RectI& right) noexcept {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

[[nodiscard]] bool is_airshot_window(HWND window) noexcept {
    if (!window) {
        return true;
    }
    wchar_t class_name[128]{};
    const int length = GetClassNameW(
        window,
        class_name,
        static_cast<int>(std::size(class_name)));
    constexpr wchar_t prefix[] = L"AirScreenshot.";
    return length >= static_cast<int>(std::size(prefix) - 1) &&
           _wcsnicmp(
               class_name,
               prefix,
               std::size(prefix) - 1) == 0;
}

[[nodiscard]] std::optional<RectI> virtual_desktop_bounds() noexcept {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const std::int64_t right =
        static_cast<std::int64_t>(left) + width;
    const std::int64_t bottom =
        static_cast<std::int64_t>(top) + height;
    if (width <= 0 || height <= 0 ||
        right > std::numeric_limits<int>::max() ||
        bottom > std::numeric_limits<int>::max() ||
        right < std::numeric_limits<int>::min() ||
        bottom < std::numeric_limits<int>::min()) {
        return std::nullopt;
    }
    return RectI{
        left,
        top,
        static_cast<int>(right),
        static_cast<int>(bottom),
    };
}

[[nodiscard]] bool provider_budget_available(
    const Request& request) noexcept {
    return elapsed_since(GetTickCount64(), request.submitted_at) <=
           kProviderBudgetMilliseconds;
}

[[nodiscard]] std::optional<RectI> current_element_rect(
    IUIAutomationElement* element,
    const Request& request,
    const RectI& desktop_bounds) noexcept {
    if (!element || !provider_budget_available(request)) {
        return std::nullopt;
    }
    BOOL offscreen = TRUE;
    if (FAILED(element->get_CurrentIsOffscreen(&offscreen)) || offscreen ||
        !provider_budget_available(request)) {
        return std::nullopt;
    }
    RECT bounds{};
    if (FAILED(element->get_CurrentBoundingRectangle(&bounds)) ||
        !provider_budget_available(request)) {
        return std::nullopt;
    }
    const std::int64_t width =
        static_cast<std::int64_t>(bounds.right) - bounds.left;
    const std::int64_t height =
        static_cast<std::int64_t>(bounds.bottom) - bounds.top;
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return sanitize_provider_rect(
        static_cast<double>(bounds.left),
        static_cast<double>(bounds.top),
        static_cast<double>(width),
        static_cast<double>(height),
        request.root_bounds,
        desktop_bounds,
        request.point);
}

void append_unique_candidate(
    CandidateChain& chain,
    const RectI& bounds,
    const RectI& root_bounds) noexcept {
    if (chain.count >= chain.candidates.size() ||
        same_rect(bounds, root_bounds)) {
        return;
    }
    for (std::size_t index = 0; index < chain.count; ++index) {
        if (same_rect(chain.candidates[index], bounds)) {
            return;
        }
    }
    chain.candidates[chain.count++] = bounds;
}

[[nodiscard]] CandidateChain empty_chain(
    const Request& request) noexcept {
    CandidateChain chain;
    chain.root = request.root;
    chain.point = request.point;
    chain.session = request.session;
    chain.sequence = request.sequence;
    return chain;
}

[[nodiscard]] HWND native_root_for_element(
    IUIAutomationElement* element,
    bool& has_native_window,
    HWND& native_window) noexcept {
    has_native_window = false;
    native_window = nullptr;
    if (!element) {
        return nullptr;
    }
    UIA_HWND native_handle{};
    if (FAILED(element->get_CurrentNativeWindowHandle(&native_handle)) ||
        !native_handle) {
        return nullptr;
    }
    has_native_window = true;
    native_window = reinterpret_cast<HWND>(native_handle);
    const HWND root = GetAncestor(native_window, GA_ROOT);
    return root ? root : native_window;
}

[[nodiscard]] CandidateChain parent_chain_from_point(
    IUIAutomationTreeWalker* walker,
    IUIAutomationElement* start,
    const Request& request,
    const RectI& desktop_bounds) noexcept {
    CandidateChain result = empty_chain(request);
    if (!walker || !start) {
        return result;
    }

    ComPtr<IUIAutomationElement> current(start);
    bool belongs_to_requested_root = false;
    for (std::size_t depth = 0;
         current && depth < kMaxProviderDepth &&
         provider_budget_available(request);
         ++depth) {
        bool has_native_window = false;
        HWND native_window = nullptr;
        const HWND native_root =
            native_root_for_element(
                current.Get(),
                has_native_window,
                native_window);
        if (has_native_window) {
            if (native_root != request.root) {
                // ElementFromPoint commonly sees Air Screenshot's transparent
                // overlay. Do not publish that foreign tree; bounded traversal
                // from the known underlying HWND handles this case below.
                return empty_chain(request);
            }
            belongs_to_requested_root = true;
        }

        if (const auto bounds = current_element_rect(
                current.Get(),
                request,
                desktop_bounds)) {
            append_unique_candidate(result, *bounds, request.root_bounds);
        }

        if (has_native_window && native_window == request.root) {
            break;
        }

        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(
                current.Get(),
                parent.GetAddressOf())) ||
            !parent || !provider_budget_available(request)) {
            break;
        }
        current = std::move(parent);
    }
    if (!belongs_to_requested_root) {
        return empty_chain(request);
    }
    return result;
}

[[nodiscard]] std::uint64_t rect_area(const RectI& rect) noexcept {
    return static_cast<std::uint64_t>(rect.right - rect.left) *
           static_cast<std::uint64_t>(rect.bottom - rect.top);
}

[[nodiscard]] CandidateChain bounded_chain_from_root(
    IUIAutomation* automation,
    IUIAutomationTreeWalker* walker,
    const Request& request,
    const RectI& desktop_bounds) noexcept {
    CandidateChain result = empty_chain(request);
    if (!automation || !walker || !provider_budget_available(request)) {
        return result;
    }

    ComPtr<IUIAutomationElement> current;
    if (FAILED(automation->ElementFromHandle(
            request.root,
            current.GetAddressOf())) ||
        !current || !provider_budget_available(request)) {
        return result;
    }

    std::array<RectI, kMaxProviderDepth> root_to_leaf{};
    std::size_t root_to_leaf_count = 0;
    std::size_t visited = 0;
    for (std::size_t depth = 0;
         depth < kMaxProviderDepth && visited < kMaxVisitedElements &&
         provider_budget_available(request);
         ++depth) {
        ComPtr<IUIAutomationElement> child;
        if (FAILED(walker->GetFirstChildElement(
                current.Get(),
                child.GetAddressOf())) ||
            !child || !provider_budget_available(request)) {
            break;
        }

        ComPtr<IUIAutomationElement> best_child;
        std::optional<RectI> best_bounds;
        std::uint64_t best_area =
            std::numeric_limits<std::uint64_t>::max();
        while (child && visited < kMaxVisitedElements &&
               provider_budget_available(request)) {
            ++visited;
            if (const auto bounds = current_element_rect(
                    child.Get(),
                    request,
                    desktop_bounds)) {
                const std::uint64_t area = rect_area(*bounds);
                if (area < best_area) {
                    best_area = area;
                    best_bounds = *bounds;
                    best_child = child;
                }
            }

            ComPtr<IUIAutomationElement> sibling;
            if (FAILED(walker->GetNextSiblingElement(
                    child.Get(),
                    sibling.GetAddressOf())) ||
                !provider_budget_available(request)) {
                break;
            }
            child = std::move(sibling);
        }
        if (!best_child || !best_bounds) {
            break;
        }
        if (!same_rect(*best_bounds, request.root_bounds) &&
            (root_to_leaf_count == 0 ||
             !same_rect(
                 root_to_leaf[root_to_leaf_count - 1],
                 *best_bounds))) {
            root_to_leaf[root_to_leaf_count++] = *best_bounds;
        }
        current = std::move(best_child);
    }

    // Traversal naturally discovers outer-to-inner. The overlay's wheel
    // navigation consumes the opposite order: leaf first, then each parent.
    for (std::size_t index = root_to_leaf_count;
         index > 0 && result.count < result.candidates.size();
         --index) {
        append_unique_candidate(
            result,
            root_to_leaf[index - 1],
            request.root_bounds);
    }
    return result;
}

[[nodiscard]] CandidateChain evaluate_request(
    IUIAutomation* automation,
    IUIAutomationTreeWalker* walker,
    const Request& request) noexcept {
    CandidateChain result = empty_chain(request);
    if (!automation || !walker || !IsWindow(request.root) ||
        is_airshot_window(request.root) ||
        !request.root_bounds.contains(request.point) ||
        !provider_budget_available(request)) {
        return result;
    }
    const auto desktop_bounds = virtual_desktop_bounds();
    if (!desktop_bounds || !desktop_bounds->contains(request.point)) {
        return result;
    }

    ComPtr<IUIAutomationElement> point_element;
    if (SUCCEEDED(automation->ElementFromPoint(
            request.point,
            point_element.GetAddressOf())) &&
        point_element && provider_budget_available(request)) {
        result = parent_chain_from_point(
            walker,
            point_element.Get(),
            request,
            *desktop_bounds);
        if (result.count != 0) {
            return result;
        }
    }
    if (!provider_budget_available(request)) {
        return empty_chain(request);
    }
    return bounded_chain_from_root(
        automation,
        walker,
        request,
        *desktop_bounds);
}

[[nodiscard]] bool ensure_automation(
    ComPtr<IUIAutomation>& automation,
    ComPtr<IUIAutomationTreeWalker>& walker) noexcept {
    if (automation && walker) {
        return true;
    }
    automation.Reset();
    walker.Reset();
    HRESULT result = CoCreateInstance(
        CLSID_CUIAutomation8,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(automation.GetAddressOf()));
    if (FAILED(result)) {
        result = CoCreateInstance(
            CLSID_CUIAutomation,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(automation.GetAddressOf()));
    }
    if (FAILED(result) || !automation ||
        FAILED(automation->get_ControlViewWalker(walker.GetAddressOf())) ||
        !walker) {
        automation.Reset();
        walker.Reset();
        return false;
    }
    return true;
}

DWORD WINAPI worker_entry(void* context) noexcept {
    auto* state = static_cast<ServiceState*>(context);
    if (!state || !state->wake_event) {
        return 0;
    }
    const HRESULT apartment =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) {
        return 0;
    }

    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationTreeWalker> walker;
    for (;;) {
        if (WaitForSingleObject(state->wake_event, INFINITE) !=
            WAIT_OBJECT_0) {
            break;
        }

        std::optional<Request> request;
        AcquireSRWLockExclusive(&state->lock);
        request = take_latest(state->policy);
        ReleaseSRWLockExclusive(&state->lock);
        if (!request) {
            continue;
        }

        CandidateChain result = empty_chain(*request);
        if (ensure_automation(automation, walker)) {
            result = evaluate_request(
                automation.Get(),
                walker.Get(),
                *request);
        }
        result.completed_at = GetTickCount64();

        AcquireSRWLockExclusive(&state->lock);
        if (result.count != 0 &&
            may_publish(state->policy, *request, result.completed_at)) {
            state->cache = result;
            state->has_cache = true;
        } else if (request->session == state->policy.session &&
                   request->sequence ==
                       state->policy.newest_sequence) {
            state->has_cache = false;
        }
        ReleaseSRWLockExclusive(&state->lock);
    }
    CoUninitialize();
    return 0;
}

[[nodiscard]] ServiceState* create_service() noexcept {
    auto* state = new (std::nothrow) ServiceState();
    if (!state) {
        return nullptr;
    }
    state->wake_event =
        CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->wake_event) {
        return state;
    }
    HANDLE worker = CreateThread(
        nullptr,
        0,
        worker_entry,
        state,
        0,
        nullptr);
    if (!worker) {
        CloseHandle(state->wake_event);
        state->wake_event = nullptr;
        return state;
    }
    CloseHandle(worker);
    // The state and auto-reset event intentionally have process lifetime. A
    // third-party provider can hang a COM call indefinitely; joining or
    // destroying this sole worker during screenshot teardown or process exit
    // would freeze the UI. This is one bounded resource, never one per capture.
    return state;
}

[[nodiscard]] ServiceState* service() noexcept {
    static ServiceState* const instance = create_service();
    return instance;
}

}  // namespace

std::uint64_t begin_session(MailboxPolicy& policy) noexcept {
    ++policy.session;
    if (policy.session == 0) {
        policy.session = 1;
    }
    policy.has_newest = false;
    policy.has_pending = false;
    policy.newest_sequence = 0;
    policy.newest_root = nullptr;
    policy.newest_point = {};
    policy.newest_submitted_at = 0;
    policy.pending = {};
    return policy.session;
}

bool queue_latest(
    MailboxPolicy& policy,
    POINT point,
    HWND root,
    const RectI& root_bounds,
    std::uint64_t now) noexcept {
    const std::int64_t delta_x = static_cast<std::int64_t>(point.x) -
                                 policy.newest_point.x;
    const std::int64_t delta_y = static_cast<std::int64_t>(point.y) -
                                 policy.newest_point.y;
    if (policy.has_newest && policy.newest_root == root &&
        std::abs(delta_x) <= kRequestCoalesceRadius &&
        std::abs(delta_y) <= kRequestCoalesceRadius &&
        elapsed_since(now, policy.newest_submitted_at) <=
            kRequestCoalesceMilliseconds) {
        return false;
    }

    ++policy.next_sequence;
    if (policy.next_sequence == 0) {
        ++policy.next_sequence;
    }
    policy.newest_sequence = policy.next_sequence;
    policy.newest_point = point;
    policy.newest_root = root;
    policy.newest_submitted_at = now;
    policy.has_newest = true;
    policy.pending = Request{
        policy.session,
        policy.newest_sequence,
        point,
        root,
        root_bounds,
        now,
    };
    policy.has_pending = true;
    return true;
}

std::optional<Request> take_latest(MailboxPolicy& policy) noexcept {
    if (!policy.has_pending) {
        return std::nullopt;
    }
    policy.has_pending = false;
    return policy.pending;
}

bool may_publish(
    const MailboxPolicy& policy,
    const Request& request,
    std::uint64_t now) noexcept {
    return policy.has_newest && request.session == policy.session &&
           request.sequence == policy.newest_sequence &&
           elapsed_since(now, request.submitted_at) <=
               kProviderBudgetMilliseconds;
}

bool may_reuse_cache(
    const CandidateChain& cache,
    POINT point,
    HWND root,
    std::uint64_t session,
    std::uint64_t now) noexcept {
    const std::int64_t delta_x =
        static_cast<std::int64_t>(point.x) - cache.point.x;
    const std::int64_t delta_y =
        static_cast<std::int64_t>(point.y) - cache.point.y;
    return cache.root == root && cache.session == session &&
           cache.count != 0 &&
           elapsed_since(now, cache.completed_at) <=
               kCacheLifetimeMilliseconds &&
           std::abs(delta_x) <= kCacheReuseRadius &&
           std::abs(delta_y) <= kCacheReuseRadius &&
           cache.candidates[0].contains(point);
}

std::optional<RectI> sanitize_provider_rect(
    double left,
    double top,
    double width,
    double height,
    const RectI& root_bounds,
    const RectI& desktop_bounds,
    POINT point) noexcept {
    if (!std::isfinite(left) || !std::isfinite(top) ||
        !std::isfinite(width) || !std::isfinite(height) || width <= 0.0 ||
        height <= 0.0) {
        return std::nullopt;
    }
    const double right = left + width;
    const double bottom = top + height;
    if (!std::isfinite(right) || !std::isfinite(bottom)) {
        return std::nullopt;
    }

    const double clipped_left = std::max(
        left,
        static_cast<double>(
            std::max(root_bounds.left, desktop_bounds.left)));
    const double clipped_top = std::max(
        top,
        static_cast<double>(
            std::max(root_bounds.top, desktop_bounds.top)));
    const double clipped_right = std::min(
        right,
        static_cast<double>(
            std::min(root_bounds.right, desktop_bounds.right)));
    const double clipped_bottom = std::min(
        bottom,
        static_cast<double>(
            std::min(root_bounds.bottom, desktop_bounds.bottom)));
    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
        return std::nullopt;
    }

    const double integer_min =
        static_cast<double>(std::numeric_limits<int>::min());
    const double integer_max =
        static_cast<double>(std::numeric_limits<int>::max());
    const double rounded_left = std::floor(clipped_left);
    const double rounded_top = std::floor(clipped_top);
    const double rounded_right = std::ceil(clipped_right);
    const double rounded_bottom = std::ceil(clipped_bottom);
    if (rounded_left < integer_min || rounded_top < integer_min ||
        rounded_right > integer_max || rounded_bottom > integer_max) {
        return std::nullopt;
    }

    const RectI result{
        static_cast<int>(rounded_left),
        static_cast<int>(rounded_top),
        static_cast<int>(rounded_right),
        static_cast<int>(rounded_bottom),
    };
    const std::int64_t result_width =
        static_cast<std::int64_t>(result.right) - result.left;
    const std::int64_t result_height =
        static_cast<std::int64_t>(result.bottom) - result.top;
    if (result_width < kMinimumCandidateExtent ||
        result_height < kMinimumCandidateExtent ||
        result_width > kMaximumCandidateExtent ||
        result_height > kMaximumCandidateExtent ||
        static_cast<std::uint64_t>(result_width) *
                static_cast<std::uint64_t>(result_height) >
            kMaximumCandidateArea ||
        !result.contains(point)) {
        return std::nullopt;
    }
    return result;
}

void begin_candidate_session() noexcept {
    ServiceState* const state = service();
    if (!state) {
        return;
    }
    AcquireSRWLockExclusive(&state->lock);
    static_cast<void>(begin_session(state->policy));
    state->has_cache = false;
    state->cache = {};
    ReleaseSRWLockExclusive(&state->lock);
}

CandidateChain cached_chain_and_request(
    POINT point,
    HWND root,
    const RectI& root_bounds) noexcept {
    CandidateChain empty;
    if (!root || !IsWindow(root) || is_airshot_window(root) ||
        !root_bounds.contains(point)) {
        return empty;
    }
    ServiceState* const state = service();
    if (!state || !state->wake_event) {
        return empty;
    }

    const std::uint64_t now = GetTickCount64();
    bool wake_worker = false;
    AcquireSRWLockExclusive(&state->lock);
    if (state->has_cache && may_reuse_cache(
            state->cache,
            point,
            root,
            state->policy.session,
            now)) {
        const CandidateChain cached = state->cache;
        ReleaseSRWLockExclusive(&state->lock);
        return cached;
    }

    wake_worker = queue_latest(
        state->policy,
        point,
        root,
        root_bounds,
        now);
    if (wake_worker) {
        state->has_cache = false;
    }
    ReleaseSRWLockExclusive(&state->lock);
    if (wake_worker) {
        SetEvent(state->wake_event);
    }
    return empty;
}

}  // namespace airshot::capture_uia
