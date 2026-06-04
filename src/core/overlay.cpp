// Selection-mask and lightweight annotation behavior is adapted from
// xland/ScreenCapture main@1574683043fa5f64b6cd45d9ec2e0db1bafbc15b.
// This implementation was substantially rewritten for Air Screenshot.

#include "airshot/overlay.h"

#include "airshot/ocr.h"
#include "airshot/output.h"
#include "airshot/strings.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

namespace airshot {
namespace {

using Microsoft::WRL::ComPtr;

enum class Tool {
    none,
    rectangle,
    arrow,
    mosaic,
    text,
};

struct Annotation {
    Tool tool{Tool::none};
    POINT start{};
    POINT end{};
    std::vector<POINT> points;
    std::wstring text;
};

struct ToolbarButton {
    std::wstring id;
    std::wstring label;
    RectI bounds;
};

class OverlaySession;

class OverlayWindow {
public:
    OverlayWindow(OverlaySession& session, MonitorSnapshot& monitor) : session_(session), monitor_(monitor) {}
    ~OverlayWindow() { destroy(); }

    bool create();
    void destroy();
    void invalidate() const;
    void paint();
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] const RectI& bounds() const noexcept { return monitor_.bounds; }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

private:
    bool create_render_target();
    D2D1_RECT_F local_rect(const RectI& rect) const;
    void draw_annotation(const Annotation& annotation, bool preview);
    void draw_arrow(POINT start, POINT end, ID2D1Brush* brush, float width);

    OverlaySession& session_;
    MonitorSnapshot& monitor_;
    HWND hwnd_{};
    ComPtr<ID2D1HwndRenderTarget> render_target_;
    ComPtr<ID2D1Bitmap> background_;
    ComPtr<ID2D1SolidColorBrush> dim_brush_;
    ComPtr<ID2D1SolidColorBrush> blue_brush_;
    ComPtr<ID2D1SolidColorBrush> white_brush_;
    ComPtr<ID2D1SolidColorBrush> toolbar_brush_;
};

struct PromptState {
    HWND window{};
    HWND edit{};
    bool accepted{};
    std::wstring text;
};

LRESULT CALLBACK text_prompt_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<PromptState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }
    if (message == WM_CREATE) {
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE,
                                      L"EDIT",
                                      L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      12,
                                      12,
                                      380,
                                      26,
                                      window,
                                      reinterpret_cast<HMENU>(100),
                                      nullptr,
                                      nullptr);
        CreateWindowExW(0,
                        L"BUTTON",
                        strings::common_confirm.data(),
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        226,
                        54,
                        80,
                        28,
                        window,
                        reinterpret_cast<HMENU>(IDOK),
                        nullptr,
                        nullptr);
        CreateWindowExW(0,
                        L"BUTTON",
                        strings::common_cancel.data(),
                        WS_CHILD | WS_VISIBLE,
                        312,
                        54,
                        80,
                        28,
                        window,
                        reinterpret_cast<HMENU>(IDCANCEL),
                        nullptr,
                        nullptr);
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SetFocus(state->edit);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(w_param) == IDOK) {
            const int length = GetWindowTextLengthW(state->edit);
            state->text.resize(static_cast<std::size_t>(length + 1));
            if (length > 0) {
                GetWindowTextW(state->edit, state->text.data(), length + 1);
            }
            state->text.resize(static_cast<std::size_t>(length));
            state->accepted = !state->text.empty();
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(w_param) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

std::optional<std::wstring> prompt_text(HWND owner, POINT position) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = text_prompt_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"AirScreenshot.TextPrompt";
        RegisterClassExW(&window_class);
    });

    PromptState state;
    const int x = std::max(0, static_cast<int>(position.x) - 200);
    const int y = std::max(0, static_cast<int>(position.y) - 60);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.TextPrompt",
                                  strings::prompt_text_title.data(),
                                  WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  420,
                                  130,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  &state);
    if (!window) {
        return std::nullopt;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return state.accepted ? std::optional(state.text) : std::nullopt;
}

class OverlaySession {
public:
    explicit OverlaySession(RegionRequest request) : request_(std::move(request)) {}

    RegionResult run() {
        monitors_ = capture_monitors();
        if (monitors_.empty() || std::ranges::any_of(monitors_, [](const auto& monitor) { return monitor.bitmap.empty(); })) {
            return {ExitCode::operation_failed, std::wstring(strings::capture_failed)};
        }
        window_candidates_ = enumerate_window_candidates();
        virtual_bounds_ = monitors_.front().bounds;
        for (const auto& monitor : monitors_) {
            virtual_bounds_.left = std::min(virtual_bounds_.left, monitor.bounds.left);
            virtual_bounds_.top = std::min(virtual_bounds_.top, monitor.bounds.top);
            virtual_bounds_.right = std::max(virtual_bounds_.right, monitor.bounds.right);
            virtual_bounds_.bottom = std::max(virtual_bounds_.bottom, monitor.bounds.bottom);
        }

        for (auto& monitor : monitors_) {
            auto window = std::make_unique<OverlayWindow>(*this, monitor);
            if (!window->create()) {
                finish({ExitCode::operation_failed, L"无法创建截图窗口。"});
                break;
            }
            windows_.push_back(std::move(window));
        }
        if (!windows_.empty() && !done_) {
            for (const auto& window : windows_) {
                ShowWindow(window->hwnd(), SW_SHOWNOACTIVATE);
            }
            SetForegroundWindow(windows_.front()->hwnd());
            SetFocus(windows_.front()->hwnd());
        }

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        for (auto& window : windows_) {
            window->destroy();
        }
        windows_.clear();
        return result_;
    }

    void on_mouse_down(HWND source, POINT point, bool right) {
        if (right) {
            finish({ExitCode::user_cancelled, L"已取消。"});
            return;
        }
        if (selection_complete_) {
            for (const auto& button : toolbar_) {
                if (button.bounds.contains(point)) {
                    invoke(button.id, source);
                    return;
                }
            }
            if (!selection_.contains(point) || active_tool_ == Tool::none) {
                return;
            }
            POINT relative{point.x - selection_.left, point.y - selection_.top};
            if (active_tool_ == Tool::text) {
                if (auto text = prompt_text(source, point)) {
                    discard_redo();
                    annotations_.push_back({Tool::text, relative, relative, {}, std::move(*text)});
                    invalidate_all();
                }
                return;
            }
            drawing_annotation_ = true;
            preview_ = {active_tool_, relative, relative, {}, {}};
            if (active_tool_ == Tool::mosaic) {
                preview_.points.push_back(relative);
            }
            SetCapture(source);
            return;
        }

        dragging_selection_ = true;
        drag_start_ = point;
        selection_ = {point.x, point.y, point.x, point.y};
        clicked_window_ = hover_;
        SetCapture(source);
        invalidate_all();
    }

    void on_mouse_move(POINT point) {
        if (selection_complete_ && drawing_annotation_) {
            preview_.end = {point.x - selection_.left, point.y - selection_.top};
            if (preview_.tool == Tool::mosaic && selection_.contains(point)) {
                const POINT relative{point.x - selection_.left, point.y - selection_.top};
                if (preview_.points.empty() || std::abs(relative.x - preview_.points.back().x) > 2 ||
                    std::abs(relative.y - preview_.points.back().y) > 2) {
                    preview_.points.push_back(relative);
                }
            }
            invalidate_all();
            return;
        }
        if (dragging_selection_) {
            selection_ = RectI{drag_start_.x, drag_start_.y, point.x, point.y}.normalized();
            invalidate_all();
            return;
        }
        if (selection_complete_) {
            return;
        }
        RectI next{};
        for (const auto& candidate : window_candidates_) {
            if (candidate.bounds.contains(point)) {
                next = candidate.bounds;
                break;
            }
        }
        if (next.left != hover_.left || next.top != hover_.top || next.right != hover_.right ||
            next.bottom != hover_.bottom) {
            hover_ = next;
            invalidate_all();
        }
    }

    void on_mouse_up(POINT point) {
        ReleaseCapture();
        if (selection_complete_ && drawing_annotation_) {
            drawing_annotation_ = false;
            if (preview_.tool == Tool::mosaic ? preview_.points.size() > 1
                                              : (preview_.start.x != preview_.end.x ||
                                                 preview_.start.y != preview_.end.y)) {
                discard_redo();
                annotations_.push_back(preview_);
            }
            preview_ = {};
            invalidate_all();
            return;
        }
        if (!dragging_selection_) {
            return;
        }
        dragging_selection_ = false;
        const int distance = std::abs(point.x - drag_start_.x) + std::abs(point.y - drag_start_.y);
        if (distance <= 4 && !clicked_window_.empty()) {
            selection_ = clicked_window_;
        }
        selection_ = selection_.normalized();
        const auto clipped = intersect(selection_, virtual_bounds_);
        if (!clipped || clipped->width() < 2 || clipped->height() < 2) {
            selection_ = {};
            invalidate_all();
            return;
        }
        selection_ = *clipped;
        selection_complete_ = true;
        build_toolbar();
        invalidate_all();

        if (request_.action == RegionAction::clipboard) {
            complete_clipboard();
        } else if (request_.action == RegionAction::file) {
            complete_file(request_.path, nullptr);
        } else if (request_.action == RegionAction::ocr) {
            complete_ocr();
        }
    }

    void on_key_down(HWND source, WPARAM key) {
        if (key == VK_ESCAPE) {
            finish({ExitCode::user_cancelled, L"已取消。"});
            return;
        }
        if (!selection_complete_) {
            return;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && key == L'Z') {
            undo();
            return;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && key == L'Y') {
            redo();
            return;
        }
        if (request_.config.ocr_enabled && (GetKeyState(VK_SHIFT) & 0x8000) != 0 && key == L'C') {
            complete_ocr();
            return;
        }
        if (key == VK_RETURN) {
            complete_clipboard();
            return;
        }
        if (key == L'S' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            complete_file({}, source);
        }
    }

    void invalidate_all() const {
        for (const auto& window : windows_) {
            window->invalidate();
        }
    }

    [[nodiscard]] RectI display_selection() const {
        if (!selection_.empty()) {
            return selection_;
        }
        return hover_;
    }

    [[nodiscard]] const RectI& selection() const noexcept { return selection_; }
    [[nodiscard]] bool selection_complete() const noexcept { return selection_complete_; }
    [[nodiscard]] const std::vector<Annotation>& annotations() const noexcept { return annotations_; }
    [[nodiscard]] const Annotation* preview() const noexcept { return drawing_annotation_ ? &preview_ : nullptr; }
    [[nodiscard]] const std::vector<ToolbarButton>& toolbar() const noexcept { return toolbar_; }
    [[nodiscard]] Tool active_tool() const noexcept { return active_tool_; }

private:
    void build_toolbar() {
        toolbar_.clear();
        std::vector<std::pair<std::wstring, std::wstring>> items;
        if (request_.config.annotation_enabled) {
            items = {{L"rect", std::wstring(strings::toolbar_rectangle)},
                     {L"arrow", std::wstring(strings::toolbar_arrow)},
                     {L"mosaic", std::wstring(strings::toolbar_mosaic)},
                     {L"text", std::wstring(strings::toolbar_text)},
                     {L"undo", std::wstring(strings::toolbar_undo)},
                     {L"redo", std::wstring(strings::toolbar_redo)}};
        }
        if (request_.config.ocr_enabled) {
            items.emplace_back(L"ocr", strings::toolbar_ocr);
        }
        items.emplace_back(L"copy", strings::toolbar_copy);
        items.emplace_back(L"save", strings::toolbar_save);
        items.emplace_back(L"close", strings::toolbar_close);

        constexpr int button_width = 42;
        constexpr int button_height = 34;
        const int total_width = static_cast<int>(items.size()) * button_width;
        int left = std::min(selection_.right - total_width, virtual_bounds_.right - total_width);
        left = std::max(left, virtual_bounds_.left);
        int top = selection_.bottom + 6;
        if (top + button_height > virtual_bounds_.bottom) {
            top = selection_.top - button_height - 6;
        }
        top = std::max(virtual_bounds_.top, top);
        for (std::size_t index = 0; index < items.size(); ++index) {
            const int x = left + static_cast<int>(index) * button_width;
            toolbar_.push_back({items[index].first, items[index].second, {x, top, x + button_width, top + button_height}});
        }
    }

    void invoke(std::wstring_view id, HWND source) {
        if (id == L"rect") {
            active_tool_ = Tool::rectangle;
        } else if (id == L"arrow") {
            active_tool_ = Tool::arrow;
        } else if (id == L"mosaic") {
            active_tool_ = Tool::mosaic;
        } else if (id == L"text") {
            active_tool_ = Tool::text;
        } else if (id == L"undo") {
            undo();
        } else if (id == L"redo") {
            redo();
        } else if (id == L"ocr") {
            complete_ocr();
        } else if (id == L"copy") {
            complete_clipboard();
        } else if (id == L"save") {
            complete_file({}, source);
        } else if (id == L"close") {
            finish({ExitCode::user_cancelled, L"已取消。"});
        }
        invalidate_all();
    }

    void undo() {
        if (!annotations_.empty()) {
            redo_.push_back(std::move(annotations_.back()));
            annotations_.pop_back();
            invalidate_all();
        }
    }

    void redo() {
        if (!redo_.empty()) {
            annotations_.push_back(std::move(redo_.back()));
            redo_.pop_back();
            invalidate_all();
        }
    }

    void discard_redo() {
        redo_.clear();
    }

    Bitmap original_selection() const {
        return compose_selection(monitors_, selection_);
    }

    Bitmap rendered_selection() const {
        Bitmap result = original_selection();
        for (const auto& annotation : annotations_) {
            if (annotation.tool == Tool::mosaic) {
                for (const POINT point : annotation.points) {
                    pixelate_circle(result, point, 14, 8);
                }
            }
        }

        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = result.width;
        info.bmiHeader.biHeight = -result.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ previous_bitmap = SelectObject(dc, dib);
        std::memcpy(bits, result.pixels.data(), result.pixels.size());
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(22, 119, 255));
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(22, 119, 255));
        HGDIOBJ previous_pen = SelectObject(dc, pen);
        HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        HFONT font = CreateFontW(22,
                                 0,
                                 0,
                                 0,
                                 FW_NORMAL,
                                 FALSE,
                                 FALSE,
                                 FALSE,
                                 DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH,
                                 L"Microsoft YaHei");
        HGDIOBJ previous_font = SelectObject(dc, font);
        for (const auto& annotation : annotations_) {
            if (annotation.tool == Tool::rectangle) {
                Rectangle(dc, annotation.start.x, annotation.start.y, annotation.end.x, annotation.end.y);
            } else if (annotation.tool == Tool::arrow) {
                MoveToEx(dc, annotation.start.x, annotation.start.y, nullptr);
                LineTo(dc, annotation.end.x, annotation.end.y);
                const double angle = std::atan2(
                    static_cast<double>(annotation.end.y - annotation.start.y),
                    static_cast<double>(annotation.end.x - annotation.start.x));
                constexpr double length = 16.0;
                for (double offset : {2.55, -2.55}) {
                    MoveToEx(dc, annotation.end.x, annotation.end.y, nullptr);
                    LineTo(dc,
                           annotation.end.x - static_cast<int>(std::cos(angle + offset) * length),
                           annotation.end.y - static_cast<int>(std::sin(angle + offset) * length));
                }
            } else if (annotation.tool == Tool::text) {
                TextOutW(dc,
                         annotation.start.x,
                         annotation.start.y,
                         annotation.text.c_str(),
                         static_cast<int>(annotation.text.size()));
            }
        }
        std::memcpy(result.pixels.data(), bits, result.pixels.size());
        SelectObject(dc, previous_font);
        SelectObject(dc, previous_brush);
        SelectObject(dc, previous_pen);
        SelectObject(dc, previous_bitmap);
        DeleteObject(font);
        DeleteObject(pen);
        DeleteObject(dib);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return result;
    }

    void complete_clipboard() {
        std::wstring error;
        if (!copy_bitmap_to_clipboard(rendered_selection(), &error)) {
            finish({ExitCode::operation_failed, std::move(error)});
            return;
        }
        finish({ExitCode::success, L"截图已复制到剪贴板。"});
    }

    void complete_file(std::wstring_view requested_path, HWND owner) {
        std::optional<std::filesystem::path> path;
        if (requested_path.empty() && request_.action == RegionAction::interactive) {
            path = prompt_png_path(owner);
            if (!path) {
                return;
            }
        } else {
            path = resolve_output_path(requested_path);
        }
        std::wstring error;
        if (!save_png(rendered_selection(), *path, &error)) {
            finish({ExitCode::operation_failed, std::move(error)});
            return;
        }
        RegionResult result{ExitCode::success, L"截图已保存。"};
        result.path = path->wstring();
        finish(std::move(result));
    }

    void complete_ocr() {
        if (!request_.config.ocr_enabled) {
            finish({ExitCode::module_unavailable, L"OCR 模块已关闭。"});
            return;
        }
        const OcrOutput output = recognize_text(original_selection());
        if (!output.ok) {
            finish({ExitCode::operation_failed, output.error});
            return;
        }
        if (request_.copy_ocr) {
            std::wstring error;
            if (!copy_text_to_clipboard(output.text, &error)) {
                finish({ExitCode::operation_failed, std::move(error)});
                return;
            }
        }
        RegionResult result{ExitCode::success, request_.copy_ocr ? std::wstring(strings::ocr_success) : L"OCR 完成。"};
        result.text = output.text;
        finish(std::move(result));
    }

    void finish(RegionResult result) {
        if (done_) {
            return;
        }
        result_ = std::move(result);
        done_ = true;
        for (const auto& window : windows_) {
            ShowWindow(window->hwnd(), SW_HIDE);
        }
    }

    RegionRequest request_;
    RegionResult result_;
    std::vector<MonitorSnapshot> monitors_;
    std::vector<WindowCandidate> window_candidates_;
    std::vector<std::unique_ptr<OverlayWindow>> windows_;
    RectI virtual_bounds_;
    RectI selection_;
    RectI hover_;
    RectI clicked_window_;
    POINT drag_start_{};
    bool dragging_selection_{};
    bool selection_complete_{};
    bool drawing_annotation_{};
    bool done_{};
    Tool active_tool_{Tool::none};
    Annotation preview_;
    std::vector<Annotation> annotations_;
    std::vector<Annotation> redo_;
    std::vector<ToolbarButton> toolbar_;

    friend class OverlayWindow;
};

ComPtr<ID2D1Factory>& d2d_factory() {
    static ComPtr<ID2D1Factory> factory;
    if (!factory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
    }
    return factory;
}

ComPtr<IDWriteFactory>& dwrite_factory() {
    static ComPtr<IDWriteFactory> factory;
    if (!factory) {
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    }
    return factory;
}

void release_overlay_factories() {
    dwrite_factory().Reset();
    d2d_factory().Reset();
}

bool OverlayWindow::create() {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = OverlayWindow::window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        window_class.lpszClassName = L"AirScreenshot.Overlay";
        RegisterClassExW(&window_class);
    });

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                            L"AirScreenshot.Overlay",
                            L"",
                            WS_POPUP,
                            monitor_.bounds.left,
                            monitor_.bounds.top,
                            monitor_.bounds.width(),
                            monitor_.bounds.height(),
                            nullptr,
                            nullptr,
                            GetModuleHandleW(nullptr),
                            this);
    return hwnd_ && create_render_target();
}

void OverlayWindow::destroy() {
    background_.Reset();
    render_target_.Reset();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void OverlayWindow::invalidate() const {
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool OverlayWindow::create_render_target() {
    if (!hwnd_) {
        return false;
    }
    const D2D1_SIZE_U size =
        D2D1::SizeU(static_cast<UINT>(monitor_.bounds.width()), static_cast<UINT>(monitor_.bounds.height()));
    HRESULT result = d2d_factory()->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        render_target_.GetAddressOf());
    if (FAILED(result)) {
        return false;
    }
    const D2D1_BITMAP_PROPERTIES properties =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    result = render_target_->CreateBitmap(size,
                                          monitor_.bitmap.pixels.data(),
                                          static_cast<UINT>(monitor_.bitmap.stride()),
                                          properties,
                                          background_.GetAddressOf());
    if (FAILED(result)) {
        return false;
    }
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0, 0.48F), dim_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0x1677FF), blue_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), white_brush_.GetAddressOf());
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0x202124, 0.94F), toolbar_brush_.GetAddressOf());
    return true;
}

D2D1_RECT_F OverlayWindow::local_rect(const RectI& rect) const {
    return D2D1::RectF(static_cast<float>(rect.left - monitor_.bounds.left),
                       static_cast<float>(rect.top - monitor_.bounds.top),
                       static_cast<float>(rect.right - monitor_.bounds.left),
                       static_cast<float>(rect.bottom - monitor_.bounds.top));
}

void OverlayWindow::draw_arrow(POINT start, POINT end, ID2D1Brush* brush, float width) {
    const D2D1_POINT_2F first{
        static_cast<float>(start.x - monitor_.bounds.left), static_cast<float>(start.y - monitor_.bounds.top)};
    const D2D1_POINT_2F second{
        static_cast<float>(end.x - monitor_.bounds.left), static_cast<float>(end.y - monitor_.bounds.top)};
    render_target_->DrawLine(first, second, brush, width);
    const double angle = std::atan2(static_cast<double>(end.y - start.y), static_cast<double>(end.x - start.x));
    constexpr double length = 16.0;
    for (double offset : {2.55, -2.55}) {
        const POINT point{
            end.x - static_cast<int>(std::cos(angle + offset) * length),
            end.y - static_cast<int>(std::sin(angle + offset) * length),
        };
        render_target_->DrawLine(second,
                                 D2D1::Point2F(static_cast<float>(point.x - monitor_.bounds.left),
                                              static_cast<float>(point.y - monitor_.bounds.top)),
                                 brush,
                                 width);
    }
}

void OverlayWindow::draw_annotation(const Annotation& annotation, bool preview) {
    const RectI selection = session_.selection();
    POINT start{selection.left + annotation.start.x, selection.top + annotation.start.y};
    POINT end{selection.left + annotation.end.x, selection.top + annotation.end.y};
    const float width = preview ? 2.0F : 3.0F;
    if (annotation.tool == Tool::rectangle) {
        render_target_->DrawRectangle(local_rect(RectI{start.x, start.y, end.x, end.y}.normalized()), blue_brush_.Get(), width);
    } else if (annotation.tool == Tool::arrow) {
        draw_arrow(start, end, blue_brush_.Get(), width);
    } else if (annotation.tool == Tool::mosaic) {
        for (const POINT relative : annotation.points) {
            POINT point{selection.left + relative.x, selection.top + relative.y};
            RectI block{point.x - 7, point.y - 7, point.x + 7, point.y + 7};
            render_target_->FillRectangle(local_rect(block), dim_brush_.Get());
        }
    } else if (annotation.tool == Tool::text) {
        ComPtr<IDWriteTextFormat> format;
        dwrite_factory()->CreateTextFormat(L"Microsoft YaHei",
                                           nullptr,
                                           DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL,
                                           22.0F,
                                           L"zh-CN",
                                           format.GetAddressOf());
        const D2D1_RECT_F bounds =
            D2D1::RectF(static_cast<float>(start.x - monitor_.bounds.left),
                        static_cast<float>(start.y - monitor_.bounds.top),
                        static_cast<float>(monitor_.bounds.width()),
                        static_cast<float>(monitor_.bounds.height()));
        render_target_->DrawTextW(annotation.text.c_str(),
                                  static_cast<UINT32>(annotation.text.size()),
                                  format.Get(),
                                  bounds,
                                  blue_brush_.Get());
    }
}

void OverlayWindow::paint() {
    if (!render_target_ && !create_render_target()) {
        return;
    }
    PAINTSTRUCT paint{};
    BeginPaint(hwnd_, &paint);
    render_target_->BeginDraw();
    render_target_->DrawBitmap(background_.Get());

    const RectI selected = session_.display_selection();
    if (selected.empty()) {
        render_target_->FillRectangle(
            D2D1::RectF(0, 0, static_cast<float>(monitor_.bounds.width()), static_cast<float>(monitor_.bounds.height())),
            dim_brush_.Get());
    } else if (const auto visible = intersect(selected, monitor_.bounds)) {
        const RectI local{
            visible->left - monitor_.bounds.left,
            visible->top - monitor_.bounds.top,
            visible->right - monitor_.bounds.left,
            visible->bottom - monitor_.bounds.top,
        };
        const float width = static_cast<float>(monitor_.bounds.width());
        const float height = static_cast<float>(monitor_.bounds.height());
        render_target_->FillRectangle(D2D1::RectF(0, 0, width, static_cast<float>(local.top)), dim_brush_.Get());
        render_target_->FillRectangle(
            D2D1::RectF(0, static_cast<float>(local.bottom), width, height), dim_brush_.Get());
        render_target_->FillRectangle(D2D1::RectF(0,
                                                  static_cast<float>(local.top),
                                                  static_cast<float>(local.left),
                                                  static_cast<float>(local.bottom)),
                                      dim_brush_.Get());
        render_target_->FillRectangle(D2D1::RectF(static_cast<float>(local.right),
                                                  static_cast<float>(local.top),
                                                  width,
                                                  static_cast<float>(local.bottom)),
                                      dim_brush_.Get());
        render_target_->DrawRectangle(local_rect(selected), blue_brush_.Get(), 2.0F);
    } else {
        render_target_->FillRectangle(
            D2D1::RectF(0, 0, static_cast<float>(monitor_.bounds.width()), static_cast<float>(monitor_.bounds.height())),
            dim_brush_.Get());
    }

    if (session_.selection_complete()) {
        for (const auto& annotation : session_.annotations()) {
            draw_annotation(annotation, false);
        }
        if (const Annotation* preview = session_.preview()) {
            draw_annotation(*preview, true);
        }

        ComPtr<IDWriteTextFormat> format;
        dwrite_factory()->CreateTextFormat(L"Microsoft YaHei",
                                           nullptr,
                                           DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL,
                                           15.0F,
                                           L"zh-CN",
                                           format.GetAddressOf());
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        for (const auto& button : session_.toolbar()) {
            if (!intersect(button.bounds, monitor_.bounds)) {
                continue;
            }
            const D2D1_RECT_F bounds = local_rect(button.bounds);
            render_target_->FillRectangle(bounds,
                                          session_.active_tool() != Tool::none &&
                                                  ((button.id == L"rect" && session_.active_tool() == Tool::rectangle) ||
                                                   (button.id == L"arrow" && session_.active_tool() == Tool::arrow) ||
                                                   (button.id == L"mosaic" && session_.active_tool() == Tool::mosaic) ||
                                                   (button.id == L"text" && session_.active_tool() == Tool::text))
                                              ? blue_brush_.Get()
                                              : toolbar_brush_.Get());
            render_target_->DrawTextW(
                button.label.c_str(), static_cast<UINT32>(button.label.size()), format.Get(), bounds, white_brush_.Get());
        }

        const std::wstring dimensions =
            std::format(L"{} × {}", session_.selection().width(), session_.selection().height());
        RectI text_bounds{session_.selection().left,
                          session_.selection().top - 28,
                          session_.selection().left + 130,
                          session_.selection().top - 4};
        if (text_bounds.top < monitor_.bounds.top) {
            text_bounds.top = session_.selection().top + 4;
            text_bounds.bottom = text_bounds.top + 24;
        }
        if (intersect(text_bounds, monitor_.bounds)) {
            const auto bounds = local_rect(text_bounds);
            render_target_->FillRectangle(bounds, toolbar_brush_.Get());
            render_target_->DrawTextW(dimensions.c_str(),
                                      static_cast<UINT32>(dimensions.size()),
                                      format.Get(),
                                      bounds,
                                      white_brush_.Get());
        }
    }

    const HRESULT result = render_target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
        background_.Reset();
        render_target_.Reset();
    }
    EndPaint(hwnd_, &paint);
}

LRESULT CALLBACK OverlayWindow::window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<OverlayWindow*>(create->lpCreateParams);
        self->hwnd_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return DefWindowProcW(window, message, w_param, l_param);
    }
    if (message == WM_PAINT) {
        self->paint();
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
        POINT point{};
        GetCursorPos(&point);
        self->session_.on_mouse_down(window, point, message == WM_RBUTTONDOWN);
        return 0;
    }
    if (message == WM_MOUSEMOVE) {
        POINT point{};
        GetCursorPos(&point);
        self->session_.on_mouse_move(point);
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        POINT point{};
        GetCursorPos(&point);
        self->session_.on_mouse_up(point);
        return 0;
    }
    if (message == WM_KEYDOWN) {
        self->session_.on_key_down(window, w_param);
        return 0;
    }
    if (message == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

RegionResult run_region_capture(const RegionRequest& request) {
    OverlaySession session(request);
    RegionResult result = session.run();
    release_overlay_factories();
    return result;
}

}  // namespace airshot
