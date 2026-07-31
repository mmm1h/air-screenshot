#include "capture_modern.h"

#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

namespace airshot::capture_detail {
namespace {

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using Direct3DDxgiInterfaceAccess =
    ::Windows::Graphics::DirectX::Direct3D11::
        IDirect3DDxgiInterfaceAccess;

constexpr DWORD kFirstFrameTimeoutMs = 1'200;
constexpr int kModernCaptureUnknown = 0;
constexpr int kModernCaptureAvailable = 1;
constexpr int kModernCaptureUnavailable = -1;

std::atomic_int& modern_capture_state() {
    static std::atomic_int state{kModernCaptureUnknown};
    return state;
}

struct FrameWaitState {
    FrameWaitState() noexcept
        : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~FrameWaitState() {
        if (event) {
            CloseHandle(event);
        }
    }

    HANDLE event{};
    std::mutex mutex;
    Direct3D11CaptureFrame frame{nullptr};
    std::atomic_bool failed{false};
};

[[nodiscard]] IDirect3DDevice create_direct3d_device(
    winrt::com_ptr<ID3D11Device>& native_device,
    winrt::com_ptr<ID3D11DeviceContext>& native_context) {
    constexpr D3D_FEATURE_LEVEL feature_levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected_level{};
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        native_device.put(),
        &selected_level,
        native_context.put());
    if (FAILED(result)) {
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            native_device.put(),
            &selected_level,
            native_context.put());
    }
    winrt::check_hresult(result);

    const auto dxgi_device = native_device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(
        CreateDirect3D11DeviceFromDXGIDevice(
            dxgi_device.get(),
            inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

[[nodiscard]] Bitmap read_frame(
    const Direct3D11CaptureFrame& frame,
    ID3D11Device* device,
    ID3D11DeviceContext* context) {
    if (!frame || !device || !context) {
        return {};
    }

    const auto content_size = frame.ContentSize();
    if (content_size.Width <= 0 || content_size.Height <= 0) {
        return {};
    }

    const auto access =
        frame.Surface().as<Direct3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> source;
    winrt::check_hresult(
        access->GetInterface(
            __uuidof(ID3D11Texture2D),
            source.put_void()));

    D3D11_TEXTURE2D_DESC source_description{};
    source->GetDesc(&source_description);
    if (source_description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        static_cast<UINT>(content_size.Width) > source_description.Width ||
        static_cast<UINT>(content_size.Height) > source_description.Height) {
        return {};
    }

    D3D11_TEXTURE2D_DESC staging_description = source_description;
    staging_description.BindFlags = 0;
    staging_description.MiscFlags = 0;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_description.ArraySize = 1;
    staging_description.MipLevels = 1;

    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(
        device->CreateTexture2D(
            &staging_description,
            nullptr,
            staging.put()));
    context->CopyResource(staging.get(), source.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(
        context->Map(
            staging.get(),
            0,
            D3D11_MAP_READ,
            0,
            &mapped));

    Bitmap result(content_size.Width, content_size.Height);
    if (!result.empty() &&
        mapped.RowPitch >= result.stride_bytes()) {
        for (int row = 0; row < result.height; ++row) {
            std::memcpy(
                result.row(row).data(),
                static_cast<const std::byte*>(mapped.pData) +
                    static_cast<std::size_t>(row) * mapped.RowPitch,
                result.stride_bytes());
        }
    } else {
        result = {};
    }
    context->Unmap(staging.get(), 0);
    if (!result.empty()) {
        result.make_opaque();
    }
    return result;
}

[[nodiscard]] Bitmap capture_item(const GraphicsCaptureItem& item) {
    if (!item || !GraphicsCaptureSession::IsSupported()) {
        return {};
    }
    const auto item_size = item.Size();
    if (item_size.Width <= 0 || item_size.Height <= 0) {
        return {};
    }

    winrt::com_ptr<ID3D11Device> native_device;
    winrt::com_ptr<ID3D11DeviceContext> native_context;
    const IDirect3DDevice direct3d_device =
        create_direct3d_device(native_device, native_context);
    auto frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        direct3d_device,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        1,
        item_size);
    auto session = frame_pool.CreateCaptureSession(item);
    try {
        session.IsCursorCaptureEnabled(false);
    } catch (...) {
        // Older supported Windows builds can still capture the frame; the
        // cursor property is best-effort.
    }

    auto state = std::make_shared<FrameWaitState>();
    if (!state->event) {
        session.Close();
        frame_pool.Close();
        return {};
    }

    const auto token = frame_pool.FrameArrived(
        [state](
            const Direct3D11CaptureFramePool& sender,
            const winrt::Windows::Foundation::IInspectable&) noexcept {
            try {
                Direct3D11CaptureFrame frame =
                    sender.TryGetNextFrame();
                if (frame) {
                    std::scoped_lock lock(state->mutex);
                    if (!state->frame) {
                        state->frame = std::move(frame);
                    }
                }
            } catch (...) {
                state->failed.store(true, std::memory_order_release);
            }
            SetEvent(state->event);
        });

    session.StartCapture();
    const DWORD wait_result =
        WaitForSingleObject(state->event, kFirstFrameTimeoutMs);
    frame_pool.FrameArrived(token);
    session.Close();
    frame_pool.Close();
    if (wait_result != WAIT_OBJECT_0 ||
        state->failed.load(std::memory_order_acquire)) {
        return {};
    }

    Direct3D11CaptureFrame frame{nullptr};
    {
        std::scoped_lock lock(state->mutex);
        frame = std::move(state->frame);
    }
    return read_frame(
        frame,
        native_device.get(),
        native_context.get());
}

[[nodiscard]] GraphicsCaptureItem item_for_monitor(HMONITOR monitor) {
    if (!monitor) {
        return nullptr;
    }
    const auto interop =
        winrt::get_activation_factory<
            GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(
        interop->CreateForMonitor(
            monitor,
            winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(item)));
    return item;
}

[[nodiscard]] GraphicsCaptureItem item_for_window(HWND window) {
    if (!window || !IsWindow(window)) {
        return nullptr;
    }
    const auto interop =
        winrt::get_activation_factory<
            GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(
        interop->CreateForWindow(
            window,
            winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(item)));
    return item;
}

}  // namespace

Bitmap capture_monitor_modern(HMONITOR monitor) noexcept {
    auto& state = modern_capture_state();
    if (state.load(std::memory_order_acquire) ==
        kModernCaptureUnavailable) {
        return {};
    }
    try {
        const ScopedWinrtApartment apartment;
        if (!apartment.available()) {
            return {};
        }
        Bitmap captured =
            capture_item(item_for_monitor(monitor));
        if (!captured.empty()) {
            state.store(
                kModernCaptureAvailable,
                std::memory_order_release);
        } else {
            int expected = kModernCaptureUnknown;
            state.compare_exchange_strong(
                expected,
                kModernCaptureUnavailable,
                std::memory_order_acq_rel);
        }
        return captured;
    } catch (...) {
        int expected = kModernCaptureUnknown;
        state.compare_exchange_strong(
            expected,
            kModernCaptureUnavailable,
            std::memory_order_acq_rel);
        return {};
    }
}

Bitmap capture_window_modern(HWND window) noexcept {
    auto& state = modern_capture_state();
    if (state.load(std::memory_order_acquire) ==
        kModernCaptureUnavailable) {
        return {};
    }
    try {
        const ScopedWinrtApartment apartment;
        if (!apartment.available()) {
            return {};
        }
        Bitmap captured =
            capture_item(item_for_window(window));
        if (!captured.empty()) {
            state.store(
                kModernCaptureAvailable,
                std::memory_order_release);
        } else {
            int expected = kModernCaptureUnknown;
            state.compare_exchange_strong(
                expected,
                kModernCaptureUnavailable,
                std::memory_order_acq_rel);
        }
        return captured;
    } catch (...) {
        int expected = kModernCaptureUnknown;
        state.compare_exchange_strong(
            expected,
            kModernCaptureUnavailable,
            std::memory_order_acq_rel);
        return {};
    }
}

}  // namespace airshot::capture_detail
