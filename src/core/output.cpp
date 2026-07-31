#include "airshot/output.h"

#include "airshot/common.h"
#include "airshot/config.h"
#include "output_test_support.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <ole2.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace airshot {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kClipboardBudget = 512ULL * 1024ULL * 1024ULL;
constexpr std::array<DWORD, 7> kClipboardRetryDelays{
    10, 20, 40, 80, 160, 250, 250};
constexpr DWORD kClipboardCommitWaitMilliseconds = 5'000;
constexpr DWORD kClipboardComWaitSliceMilliseconds = 25;
constexpr DWORD kClipboardRollbackRetryMilliseconds = 3'500;
constexpr DWORD kClipboardRollbackRetryDelayMilliseconds = 100;
constexpr std::size_t kClipboardSnapshotFormatLimit = 1'024;

std::atomic_size_t required_clipboard_format_failure_index{};
std::atomic_uint clipboard_flush_failures_remaining{};
std::atomic<DWORD> clipboard_worker_delay_milliseconds{};
std::atomic<DWORD> clipboard_wait_timeout_override_milliseconds{};
std::atomic<DWORD> clipboard_pre_flush_delay_milliseconds{};
std::atomic_bool clipboard_forward_set_pending{};
std::atomic_bool clipboard_snapshot_failure_for_testing{};
std::atomic_uint64_t clipboard_commit_generation_source{1};
std::atomic_uint64_t clipboard_active_commit_generation{};
// Zero means no bypass. A DWORD sequence is stored plus one so the valid
// sequence value zero remains representable.
std::atomic_uint64_t clipboard_snapshot_bypass_marker{};

[[nodiscard]] bool retryable_clipboard_result(HRESULT result) noexcept {
    return result == CLIPBRD_E_CANT_OPEN ||
           result == CLIPBRD_E_CANT_CLOSE;
}

void wait_for_clipboard_retry(DWORD duration) noexcept {
    const ULONGLONG deadline = GetTickCount64() + duration;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            return;
        }
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        const DWORD wait_result = MsgWaitForMultipleObjectsEx(
            0,
            nullptr,
            remaining,
            QS_SENDMESSAGE,
            MWMO_INPUTAVAILABLE);
        if (wait_result == WAIT_TIMEOUT || wait_result == WAIT_FAILED) {
            return;
        }
        if (wait_result != WAIT_OBJECT_0) {
            return;
        }

        // Dispatch only nonqueued synchronous messages. This lets OLE satisfy
        // WM_RENDERFORMAT while another process has the clipboard open without
        // consuming posted input on this STA worker.
        MSG message{};
        (void)PeekMessageW(
            &message,
            nullptr,
            0,
            0,
            PM_NOREMOVE | PM_NOYIELD | PM_QS_SENDMESSAGE);
    }
}

template <typename Operation>
[[nodiscard]] HRESULT retry_clipboard_operation(
    Operation&& operation) noexcept {
    HRESULT result = E_FAIL;
    for (std::size_t attempt = 0;
         attempt <= kClipboardRetryDelays.size();
         ++attempt) {
        result = operation();
        if (SUCCEEDED(result) || !retryable_clipboard_result(result) ||
            attempt == kClipboardRetryDelays.size()) {
            return result;
        }
        wait_for_clipboard_retry(kClipboardRetryDelays[attempt]);
    }
    return result;
}

template <typename Operation>
[[nodiscard]] HRESULT retry_clipboard_flush_operation(
    Operation&& operation) noexcept {
    HRESULT result = E_FAIL;
    for (std::size_t attempt = 0;
         attempt <= kClipboardRetryDelays.size();
         ++attempt) {
        result = operation();
        if (SUCCEEDED(result) ||
            (result != E_FAIL && !retryable_clipboard_result(result)) ||
            attempt == kClipboardRetryDelays.size()) {
            return result;
        }
        wait_for_clipboard_retry(kClipboardRetryDelays[attempt]);
    }
    return result;
}

void clear_error(std::wstring* error) {
    if (error) {
        error->clear();
    }
}

void set_error(std::wstring* error, std::wstring message) {
    if (error) {
        *error = std::move(message);
    }
}

[[nodiscard]] bool checked_add(std::size_t first, std::size_t second, std::size_t& result) noexcept {
    if (second > std::numeric_limits<std::size_t>::max() - first) {
        return false;
    }
    result = first + second;
    return true;
}

[[nodiscard]] bool checked_multiply(std::size_t first, std::size_t second, std::size_t& result) noexcept {
    if (first != 0 && second > std::numeric_limits<std::size_t>::max() / first) {
        return false;
    }
    result = first * second;
    return true;
}

class GlobalMemory {
public:
    GlobalMemory() = default;
    explicit GlobalMemory(HGLOBAL value) noexcept : value_(value) {}
    ~GlobalMemory() {
        if (value_) {
            GlobalFree(value_);
        }
    }
    GlobalMemory(const GlobalMemory&) = delete;
    GlobalMemory& operator=(const GlobalMemory&) = delete;
    GlobalMemory(GlobalMemory&& other) noexcept : value_(other.release()) {}
    GlobalMemory& operator=(GlobalMemory&& other) noexcept {
        if (this != &other) {
            if (value_) {
                GlobalFree(value_);
            }
            value_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] HGLOBAL get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
    [[nodiscard]] HGLOBAL release() noexcept {
        const HGLOBAL value = value_;
        value_ = nullptr;
        return value;
    }

private:
    HGLOBAL value_{};
};

class GlobalLockView {
public:
    explicit GlobalLockView(HGLOBAL memory) noexcept : memory_(memory), value_(GlobalLock(memory)) {}
    ~GlobalLockView() {
        if (value_) {
            GlobalUnlock(memory_);
        }
    }
    GlobalLockView(const GlobalLockView&) = delete;
    GlobalLockView& operator=(const GlobalLockView&) = delete;

    [[nodiscard]] void* get() const noexcept { return value_; }

private:
    HGLOBAL memory_{};
    void* value_{};
};

class OwnedBitmap {
public:
    OwnedBitmap() = default;
    explicit OwnedBitmap(HBITMAP value) noexcept : value_(value) {}
    ~OwnedBitmap() {
        if (value_) {
            DeleteObject(value_);
        }
    }
    OwnedBitmap(const OwnedBitmap&) = delete;
    OwnedBitmap& operator=(const OwnedBitmap&) = delete;
    OwnedBitmap(OwnedBitmap&& other) noexcept : value_(other.release()) {}
    OwnedBitmap& operator=(OwnedBitmap&& other) noexcept {
        if (this != &other) {
            if (value_) {
                DeleteObject(value_);
            }
            value_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] HBITMAP get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
    [[nodiscard]] HBITMAP release() noexcept {
        const HBITMAP value = value_;
        value_ = nullptr;
        return value;
    }

private:
    HBITMAP value_{};
};

[[nodiscard]] GlobalMemory duplicate_global_memory(
    HGLOBAL source) noexcept {
    if (!source) {
        return {};
    }
    const SIZE_T bytes = GlobalSize(source);
    if (bytes == 0) {
        return {};
    }

    GlobalMemory duplicate(GlobalAlloc(GMEM_MOVEABLE, bytes));
    if (!duplicate) {
        return {};
    }
    const GlobalLockView source_view(source);
    const GlobalLockView duplicate_view(duplicate.get());
    if (!source_view.get() || !duplicate_view.get()) {
        return {};
    }
    std::memcpy(duplicate_view.get(), source_view.get(), bytes);
    return duplicate;
}

struct ClipboardFormatData {
    ClipboardFormatData(UINT format, GlobalMemory&& value) noexcept
        : descriptor{
              static_cast<CLIPFORMAT>(format),
              nullptr,
              DVASPECT_CONTENT,
              -1,
              TYMED_HGLOBAL},
          global(std::move(value)) {}

    ClipboardFormatData(UINT format, OwnedBitmap&& value) noexcept
        : descriptor{
              static_cast<CLIPFORMAT>(format),
              nullptr,
              DVASPECT_CONTENT,
              -1,
              TYMED_GDI},
          bitmap(std::move(value)) {}

    ClipboardFormatData(const ClipboardFormatData&) = delete;
    ClipboardFormatData& operator=(const ClipboardFormatData&) = delete;
    ClipboardFormatData(ClipboardFormatData&&) noexcept = default;
    ClipboardFormatData& operator=(ClipboardFormatData&&) noexcept = default;

    FORMATETC descriptor{};
    GlobalMemory global;
    OwnedBitmap bitmap;
};

class ClipboardDataObject final : public IDataObject {
public:
    explicit ClipboardDataObject(
        std::vector<ClipboardFormatData>&& formats) noexcept
        : formats_(std::move(formats)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interface_id,
        void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (InlineIsEqualGUID(interface_id, IID_IUnknown) ||
            InlineIsEqualGUID(interface_id, IID_IDataObject)) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining =
            references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetData(
        FORMATETC* requested,
        STGMEDIUM* medium) noexcept override {
        if (!requested || !medium) {
            return E_INVALIDARG;
        }
        *medium = {};

        const ClipboardFormatData* format = nullptr;
        const HRESULT query_result = query_format(*requested, format);
        if (FAILED(query_result)) {
            return query_result;
        }

        // IDataObject::GetData transfers an independently releasable medium.
        // In particular, OleFlushClipboard can transfer the returned handle to
        // the system clipboard, so it must never alias our source payload.
        medium->tymed = format->descriptor.tymed;
        if (format->descriptor.tymed == TYMED_HGLOBAL) {
            GlobalMemory duplicate =
                duplicate_global_memory(format->global.get());
            if (!duplicate) {
                *medium = {};
                return STG_E_MEDIUMFULL;
            }
            medium->hGlobal = duplicate.release();
        } else {
            OwnedBitmap duplicate(static_cast<HBITMAP>(CopyImage(
                format->bitmap.get(),
                IMAGE_BITMAP,
                0,
                0,
                LR_CREATEDIBSECTION)));
            if (!duplicate) {
                *medium = {};
                return STG_E_MEDIUMFULL;
            }
            medium->hBitmap = duplicate.release();
        }
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(
        FORMATETC*,
        STGMEDIUM*) noexcept override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(
        FORMATETC* requested) noexcept override {
        if (!requested) {
            return E_INVALIDARG;
        }
        const ClipboardFormatData* format = nullptr;
        return query_format(*requested, format);
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
        FORMATETC*,
        FORMATETC* canonical) noexcept override {
        if (!canonical) {
            return E_INVALIDARG;
        }
        canonical->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(
        FORMATETC*,
        STGMEDIUM*,
        BOOL) noexcept override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(
        DWORD direction,
        IEnumFORMATETC** enumerator) noexcept override {
        if (!enumerator) {
            return E_POINTER;
        }
        *enumerator = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }

        try {
            std::vector<FORMATETC> descriptors;
            descriptors.reserve(formats_.size());
            for (const auto& format : formats_) {
                descriptors.push_back(format.descriptor);
            }
            return SHCreateStdEnumFmtEtc(
                static_cast<UINT>(descriptors.size()),
                descriptors.data(),
                enumerator);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (const std::length_error&) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT STDMETHODCALLTYPE DAdvise(
        FORMATETC*,
        DWORD,
        IAdviseSink*,
        DWORD* connection) noexcept override {
        if (connection) {
            *connection = 0;
        }
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) noexcept override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(
        IEnumSTATDATA** enumerator) noexcept override {
        if (enumerator) {
            *enumerator = nullptr;
        }
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    [[nodiscard]] HRESULT query_format(
        const FORMATETC& requested,
        const ClipboardFormatData*& result) const noexcept {
        result = nullptr;
        if (requested.dwAspect != DVASPECT_CONTENT) {
            return DV_E_DVASPECT;
        }
        if (requested.lindex != -1) {
            return DV_E_LINDEX;
        }
        if (requested.ptd) {
            return DV_E_DVTARGETDEVICE;
        }
        for (const auto& format : formats_) {
            if (format.descriptor.cfFormat != requested.cfFormat) {
                continue;
            }
            if ((format.descriptor.tymed & requested.tymed) == 0) {
                return DV_E_TYMED;
            }
            result = &format;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    std::atomic<ULONG> references_{1};
    std::vector<ClipboardFormatData> formats_;
};

class OwnedStgMedium {
public:
    OwnedStgMedium() = default;
    explicit OwnedStgMedium(STGMEDIUM& value) noexcept : value_(value) {
        value = {};
    }
    ~OwnedStgMedium() { reset(); }
    OwnedStgMedium(const OwnedStgMedium&) = delete;
    OwnedStgMedium& operator=(const OwnedStgMedium&) = delete;
    OwnedStgMedium(OwnedStgMedium&& other) noexcept : value_(other.release()) {}
    OwnedStgMedium& operator=(OwnedStgMedium&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] const STGMEDIUM& get() const noexcept { return value_; }

private:
    void reset() noexcept {
        if (value_.tymed != TYMED_NULL || value_.pUnkForRelease) {
            ReleaseStgMedium(&value_);
        }
        value_ = {};
    }

    [[nodiscard]] STGMEDIUM release() noexcept {
        const STGMEDIUM value = value_;
        value_ = {};
        return value;
    }

    STGMEDIUM value_{};
};

struct ClipboardSnapshotFormat {
    ClipboardSnapshotFormat(
        const FORMATETC& source_descriptor,
        STGMEDIUM& source_medium) noexcept
        : descriptor(source_descriptor),
          medium(source_medium) {
        descriptor.ptd = nullptr;
        descriptor.tymed = medium.get().tymed;
    }

    ClipboardSnapshotFormat(const ClipboardSnapshotFormat&) = delete;
    ClipboardSnapshotFormat& operator=(const ClipboardSnapshotFormat&) = delete;
    ClipboardSnapshotFormat(ClipboardSnapshotFormat&&) noexcept = default;
    ClipboardSnapshotFormat& operator=(ClipboardSnapshotFormat&&) noexcept =
        default;

    FORMATETC descriptor{};
    OwnedStgMedium medium;
};

[[nodiscard]] HRESULT duplicate_snapshot_medium(
    const ClipboardSnapshotFormat& format,
    STGMEDIUM* destination) noexcept {
    *destination = {};
    const STGMEDIUM& source = format.medium.get();
    destination->tymed = source.tymed;
    switch (source.tymed) {
        case TYMED_HGLOBAL:
            destination->hGlobal = static_cast<HGLOBAL>(OleDuplicateData(
                source.hGlobal,
                format.descriptor.cfFormat,
                0));
            break;
        case TYMED_GDI:
            destination->hBitmap = static_cast<HBITMAP>(OleDuplicateData(
                source.hBitmap,
                format.descriptor.cfFormat,
                0));
            break;
        case TYMED_MFPICT:
            destination->hMetaFilePict = static_cast<HMETAFILEPICT>(
                OleDuplicateData(
                    source.hMetaFilePict,
                    format.descriptor.cfFormat,
                    0));
            break;
        case TYMED_ENHMF:
            destination->hEnhMetaFile = CopyEnhMetaFileW(
                source.hEnhMetaFile,
                nullptr);
            break;
        case TYMED_ISTREAM:
            if (!source.pstm) {
                *destination = {};
                return STG_E_MEDIUMFULL;
            }
            {
                const HRESULT clone_result =
                    source.pstm->Clone(&destination->pstm);
                if (FAILED(clone_result) || !destination->pstm) {
                    *destination = {};
                    return FAILED(clone_result)
                               ? clone_result
                               : STG_E_MEDIUMFULL;
                }
            }
            break;
        case TYMED_ISTORAGE:
            destination->pstg = source.pstg;
            if (destination->pstg) {
                destination->pstg->AddRef();
            }
            break;
        default:
            *destination = {};
            return DV_E_TYMED;
    }
    if (!destination->hGlobal) {
        *destination = {};
        return STG_E_MEDIUMFULL;
    }
    destination->pUnkForRelease = nullptr;
    return S_OK;
}

class ClipboardSnapshotDataObject final : public IDataObject {
public:
    explicit ClipboardSnapshotDataObject(
        std::vector<ClipboardSnapshotFormat>&& formats) noexcept
        : formats_(std::move(formats)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interface_id,
        void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (InlineIsEqualGUID(interface_id, IID_IUnknown) ||
            InlineIsEqualGUID(interface_id, IID_IDataObject)) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining =
            references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetData(
        FORMATETC* requested,
        STGMEDIUM* medium) noexcept override {
        if (!requested || !medium) {
            return E_INVALIDARG;
        }
        *medium = {};
        const ClipboardSnapshotFormat* format = nullptr;
        const HRESULT query_result = query_format(*requested, format);
        if (FAILED(query_result)) {
            return query_result;
        }
        return duplicate_snapshot_medium(*format, medium);
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(
        FORMATETC*,
        STGMEDIUM*) noexcept override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(
        FORMATETC* requested) noexcept override {
        if (!requested) {
            return E_INVALIDARG;
        }
        const ClipboardSnapshotFormat* format = nullptr;
        return query_format(*requested, format);
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
        FORMATETC*,
        FORMATETC* canonical) noexcept override {
        if (!canonical) {
            return E_INVALIDARG;
        }
        canonical->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(
        FORMATETC*,
        STGMEDIUM*,
        BOOL) noexcept override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(
        DWORD direction,
        IEnumFORMATETC** enumerator) noexcept override {
        if (!enumerator) {
            return E_POINTER;
        }
        *enumerator = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        try {
            std::vector<FORMATETC> descriptors;
            descriptors.reserve(formats_.size());
            for (const auto& format : formats_) {
                descriptors.push_back(format.descriptor);
            }
            return SHCreateStdEnumFmtEtc(
                static_cast<UINT>(descriptors.size()),
                descriptors.data(),
                enumerator);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (const std::length_error&) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT STDMETHODCALLTYPE DAdvise(
        FORMATETC*,
        DWORD,
        IAdviseSink*,
        DWORD* connection) noexcept override {
        if (connection) {
            *connection = 0;
        }
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) noexcept override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(
        IEnumSTATDATA** enumerator) noexcept override {
        if (enumerator) {
            *enumerator = nullptr;
        }
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    [[nodiscard]] HRESULT query_format(
        const FORMATETC& requested,
        const ClipboardSnapshotFormat*& result) const noexcept {
        result = nullptr;
        if (requested.ptd) {
            return DV_E_DVTARGETDEVICE;
        }
        for (const auto& format : formats_) {
            if (format.descriptor.cfFormat != requested.cfFormat) {
                continue;
            }
            if (format.descriptor.dwAspect != requested.dwAspect) {
                continue;
            }
            if (format.descriptor.lindex != requested.lindex) {
                continue;
            }
            if ((format.descriptor.tymed & requested.tymed) == 0) {
                continue;
            }
            result = &format;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    std::atomic<ULONG> references_{1};
    std::vector<ClipboardSnapshotFormat> formats_;
};

[[nodiscard]] HRESULT finalize_clipboard_snapshot(
    std::vector<ClipboardSnapshotFormat>&& formats,
    ComPtr<IDataObject>& snapshot) noexcept {
    auto* raw_snapshot = new (std::nothrow)
        ClipboardSnapshotDataObject(std::move(formats));
    if (!raw_snapshot) {
        return E_OUTOFMEMORY;
    }
    snapshot.Attach(raw_snapshot);
    return S_OK;
}

[[nodiscard]] DWORD select_snapshot_tymed(DWORD offered) noexcept {
    constexpr std::array<DWORD, 6> preference{
        TYMED_HGLOBAL,
        TYMED_GDI,
        TYMED_ENHMF,
        TYMED_MFPICT,
        TYMED_ISTREAM,
        TYMED_ISTORAGE,
    };
    for (const DWORD tymed : preference) {
        if ((offered & tymed) != 0) {
            return tymed;
        }
    }
    return TYMED_NULL;
}

[[nodiscard]] HRESULT capture_clipboard_snapshot(
    IDataObject* source,
    ComPtr<IDataObject>& snapshot) noexcept {
    snapshot.Reset();
    if (!source) {
        return E_POINTER;
    }

    ComPtr<IEnumFORMATETC> enumerator;
    HRESULT result = retry_clipboard_operation(
        [&source, &enumerator] {
            return source->EnumFormatEtc(
                DATADIR_GET,
                enumerator.ReleaseAndGetAddressOf());
        });
    if (FAILED(result)) {
        return result;
    }
    if (!enumerator) {
        return E_UNEXPECTED;
    }

    try {
        std::vector<ClipboardSnapshotFormat> formats;
        std::size_t global_bytes = 0;
        for (;;) {
            FORMATETC descriptor{};
            ULONG fetched = 0;
            result = retry_clipboard_operation(
                [&enumerator, &descriptor, &fetched] {
                    if (descriptor.ptd) {
                        CoTaskMemFree(descriptor.ptd);
                    }
                    descriptor = {};
                    fetched = 0;
                    return enumerator->Next(1, &descriptor, &fetched);
                });
            if (result == S_FALSE) {
                if (descriptor.ptd) {
                    CoTaskMemFree(descriptor.ptd);
                }
                break;
            }
            if (FAILED(result) || fetched != 1) {
                if (descriptor.ptd) {
                    CoTaskMemFree(descriptor.ptd);
                }
                return FAILED(result) ? result : E_FAIL;
            }
            if (descriptor.ptd) {
                CoTaskMemFree(descriptor.ptd);
                return DV_E_DVTARGETDEVICE;
            }
            if (formats.size() >= kClipboardSnapshotFormatLimit) {
                return DV_E_FORMATETC;
            }

            descriptor.tymed = select_snapshot_tymed(descriptor.tymed);
            if (descriptor.tymed == TYMED_NULL) {
                return DV_E_TYMED;
            }
            STGMEDIUM medium{};
            result = retry_clipboard_operation(
                [&source, &descriptor, &medium] {
                    if (medium.tymed != TYMED_NULL ||
                        medium.pUnkForRelease) {
                        ReleaseStgMedium(&medium);
                    }
                    medium = {};
                    return source->GetData(&descriptor, &medium);
                });
            if (FAILED(result)) {
                if (medium.tymed != TYMED_NULL || medium.pUnkForRelease) {
                    ReleaseStgMedium(&medium);
                }
                return result;
            }
            if (medium.tymed != descriptor.tymed) {
                ReleaseStgMedium(&medium);
                return DV_E_TYMED;
            }
            if (!medium.hGlobal) {
                ReleaseStgMedium(&medium);
                return STG_E_MEDIUMFULL;
            }
            if (medium.tymed == TYMED_ISTREAM) {
                IStream* cloned_stream = nullptr;
                const HRESULT clone_result = medium.pstm
                                                 ? medium.pstm->Clone(
                                                       &cloned_stream)
                                                 : STG_E_MEDIUMFULL;
                if (FAILED(clone_result) || !cloned_stream) {
                    ReleaseStgMedium(&medium);
                    return FAILED(clone_result)
                               ? clone_result
                               : STG_E_MEDIUMFULL;
                }
                ReleaseStgMedium(&medium);
                medium = {};
                medium.tymed = TYMED_ISTREAM;
                medium.pstm = cloned_stream;
                medium.pUnkForRelease = nullptr;
            }
            if (medium.tymed == TYMED_HGLOBAL) {
                const SIZE_T bytes = GlobalSize(medium.hGlobal);
                std::size_t next_bytes = 0;
                if (bytes == 0 ||
                    !checked_add(global_bytes, bytes, next_bytes) ||
                    next_bytes > kClipboardBudget) {
                    ReleaseStgMedium(&medium);
                    return STG_E_MEDIUMFULL;
                }
                global_bytes = next_bytes;
            }
            ClipboardSnapshotFormat captured(descriptor, medium);
            formats.push_back(std::move(captured));
        }

        return finalize_clipboard_snapshot(
            std::move(formats),
            snapshot);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (const std::length_error&) {
        return E_OUTOFMEMORY;
    }
}

[[nodiscard]] DWORD clipboard_format_tymed(UINT format) noexcept {
    switch (format) {
        case CF_BITMAP:
        case CF_PALETTE:
        case CF_DSPBITMAP:
            return TYMED_GDI;
        case CF_METAFILEPICT:
        case CF_DSPMETAFILEPICT:
            return TYMED_MFPICT;
        case CF_ENHMETAFILE:
        case CF_DSPENHMETAFILE:
            return TYMED_ENHMF;
        default:
            return TYMED_HGLOBAL;
    }
}

[[nodiscard]] HRESULT capture_win32_clipboard_snapshot(
    ComPtr<IDataObject>& snapshot) noexcept {
    snapshot.Reset();
    const HRESULT open_result = retry_clipboard_operation([] {
        return OpenClipboard(nullptr) ? S_OK : CLIPBRD_E_CANT_OPEN;
    });
    if (FAILED(open_result)) {
        return open_result;
    }

    const HRESULT capture_result = [&snapshot]() noexcept -> HRESULT {
        try {
            std::vector<ClipboardSnapshotFormat> formats;
            std::size_t global_bytes = 0;
            UINT format = 0;
            for (;;) {
                SetLastError(ERROR_SUCCESS);
                format = EnumClipboardFormats(format);
                if (format == 0) {
                    if (GetLastError() != ERROR_SUCCESS) {
                        return CLIPBRD_E_CANT_OPEN;
                    }
                    return finalize_clipboard_snapshot(
                        std::move(formats),
                        snapshot);
                }
                if (formats.size() >= kClipboardSnapshotFormatLimit) {
                    return DV_E_FORMATETC;
                }

                const HANDLE source = GetClipboardData(format);
                if (!source) {
                    return DV_E_FORMATETC;
                }
                const DWORD tymed = clipboard_format_tymed(format);
                const HANDLE duplicate = tymed == TYMED_ENHMF
                                             ? CopyEnhMetaFileW(
                                                   static_cast<HENHMETAFILE>(
                                                       source),
                                                   nullptr)
                                             : OleDuplicateData(
                                                   source,
                                                   static_cast<CLIPFORMAT>(
                                                       format),
                                                   0);
                if (!duplicate) {
                    return STG_E_MEDIUMFULL;
                }

                STGMEDIUM medium{};
                medium.tymed = tymed;
                switch (medium.tymed) {
                    case TYMED_GDI:
                        medium.hBitmap = static_cast<HBITMAP>(duplicate);
                        break;
                    case TYMED_MFPICT:
                        medium.hMetaFilePict =
                            static_cast<HMETAFILEPICT>(duplicate);
                        break;
                    case TYMED_ENHMF:
                        medium.hEnhMetaFile =
                            static_cast<HENHMETAFILE>(duplicate);
                        break;
                    default:
                        medium.tymed = TYMED_HGLOBAL;
                        medium.hGlobal = static_cast<HGLOBAL>(duplicate);
                        break;
                }

                if (medium.tymed == TYMED_HGLOBAL) {
                    const SIZE_T bytes = GlobalSize(medium.hGlobal);
                    std::size_t next_bytes = 0;
                    if (bytes == 0 ||
                        !checked_add(global_bytes, bytes, next_bytes) ||
                        next_bytes > kClipboardBudget) {
                        ReleaseStgMedium(&medium);
                        return STG_E_MEDIUMFULL;
                    }
                    global_bytes = next_bytes;
                }

                const FORMATETC descriptor{
                    static_cast<CLIPFORMAT>(format),
                    nullptr,
                    DVASPECT_CONTENT,
                    -1,
                    medium.tymed,
                };
                ClipboardSnapshotFormat captured(descriptor, medium);
                formats.push_back(std::move(captured));
            }
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (const std::length_error&) {
            return E_OUTOFMEMORY;
        }
    }();

    const BOOL close_succeeded = CloseClipboard();
    if (!close_succeeded && SUCCEEDED(capture_result)) {
        snapshot.Reset();
        return CLIPBRD_E_CANT_CLOSE;
    }
    return capture_result;
}

class ScopedOleApartment {
public:
    ScopedOleApartment() noexcept : result_(OleInitialize(nullptr)) {}
    ~ScopedOleApartment() {
        if (SUCCEEDED(result_)) {
            OleUninitialize();
        }
    }
    ScopedOleApartment(const ScopedOleApartment&) = delete;
    ScopedOleApartment& operator=(const ScopedOleApartment&) = delete;

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{};
};

[[nodiscard]] bool reserve_clipboard_formats(
    std::vector<ClipboardFormatData>& formats,
    std::size_t count) noexcept {
    try {
        formats.reserve(count);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

[[nodiscard]] bool stage_required_clipboard_format(
    std::vector<ClipboardFormatData>& formats,
    std::size_t required_ordinal,
    UINT format,
    GlobalMemory&& value) noexcept {
    if (required_clipboard_format_failure_index.load(
            std::memory_order_relaxed) == required_ordinal) {
        return false;
    }
    try {
        formats.emplace_back(format, std::move(value));
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

void stage_optional_clipboard_format(
    std::vector<ClipboardFormatData>& formats,
    UINT format,
    GlobalMemory&& value) noexcept {
    if (!value) {
        return;
    }
    try {
        formats.emplace_back(format, std::move(value));
    } catch (const std::bad_alloc&) {
    } catch (const std::length_error&) {
    }
}

void stage_optional_clipboard_format(
    std::vector<ClipboardFormatData>& formats,
    UINT format,
    OwnedBitmap&& value) noexcept {
    if (!value) {
        return;
    }
    try {
        formats.emplace_back(format, std::move(value));
    } catch (const std::bad_alloc&) {
    } catch (const std::length_error&) {
    }
}

template <typename Operation>
[[nodiscard]] HRESULT retry_clipboard_rollback_operation(
    Operation&& operation,
    bool retry_unspecified_failure = false) noexcept {
    const ULONGLONG deadline =
        GetTickCount64() + kClipboardRollbackRetryMilliseconds;
    HRESULT result = E_FAIL;
    for (;;) {
        result = operation();
        if (SUCCEEDED(result) ||
            (!retryable_clipboard_result(result) &&
             !(retry_unspecified_failure && result == E_FAIL))) {
            return result;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            return result;
        }
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        wait_for_clipboard_retry(
            remaining < kClipboardRollbackRetryDelayMilliseconds
                ? remaining
                : kClipboardRollbackRetryDelayMilliseconds);
    }
}

enum class ClipboardCommitDisposition {
    success,
    preserved,
    uncertain,
    superseded,
    preparation_timed_out,
    timed_out,
    busy,
};

struct ClipboardCommitOutcome {
    ClipboardCommitDisposition disposition{ClipboardCommitDisposition::uncertain};
    HRESULT result{E_FAIL};
    HRESULT snapshot_result{S_OK};
    HRESULT rollback_result{S_OK};
    HRESULT ownership_result{S_OK};
    bool rollback_available{};
    bool rollback_attempted{};

    [[nodiscard]] bool succeeded() const noexcept {
        return disposition == ClipboardCommitDisposition::success;
    }
};

enum class ClipboardWorkerPhase : unsigned char {
    preparing,
    mutating,
    canceled,
};

struct ClipboardWorkerState {
    explicit ClipboardWorkerState(
        std::vector<ClipboardFormatData>&& staged_formats,
        std::uint64_t generation) noexcept
        : formats(std::move(staged_formats)),
          commit_generation(generation) {}

    std::vector<ClipboardFormatData> formats;
    ClipboardCommitOutcome outcome;
    const std::uint64_t commit_generation{};
    std::atomic<ClipboardWorkerPhase> phase{ClipboardWorkerPhase::preparing};
};

void release_clipboard_commit_generation(
    std::uint64_t generation) noexcept {
    std::uint64_t expected = generation;
    (void)clipboard_active_commit_generation.compare_exchange_strong(
        expected,
        0,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

class ClipboardInFlightRelease {
public:
    explicit ClipboardInFlightRelease(std::uint64_t generation) noexcept
        : generation_(generation) {}
    ~ClipboardInFlightRelease() {
        // A canceled preparation can release its token while its detached
        // worker is still blocked in a foreign IDataObject. Compare against
        // this worker's generation so it can never clear a newer commit.
        release_clipboard_commit_generation(generation_);
    }
    ClipboardInFlightRelease(const ClipboardInFlightRelease&) = delete;
    ClipboardInFlightRelease& operator=(const ClipboardInFlightRelease&) = delete;

private:
    const std::uint64_t generation_{};
};

[[nodiscard]] std::uint64_t next_clipboard_commit_generation() noexcept {
    for (;;) {
        const std::uint64_t generation =
            clipboard_commit_generation_source.fetch_add(
                1,
                std::memory_order_relaxed);
        if (generation != 0) {
            return generation;
        }
    }
}

void remember_clipboard_snapshot_bypass() noexcept {
    const std::uint64_t marker =
        static_cast<std::uint64_t>(GetClipboardSequenceNumber()) + 1;
    clipboard_snapshot_bypass_marker.store(marker, std::memory_order_release);
}

[[nodiscard]] bool should_bypass_clipboard_snapshot() noexcept {
    std::uint64_t marker = clipboard_snapshot_bypass_marker.load(
        std::memory_order_acquire);
    if (marker == 0) {
        return false;
    }
    const std::uint64_t current_marker =
        static_cast<std::uint64_t>(GetClipboardSequenceNumber()) + 1;
    if (marker == current_marker) {
        return true;
    }
    (void)clipboard_snapshot_bypass_marker.compare_exchange_strong(
        marker,
        0,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
    return false;
}

[[nodiscard]] bool cancel_preparing_clipboard_worker(
    const std::shared_ptr<ClipboardWorkerState>& state) noexcept {
    ClipboardWorkerPhase expected = ClipboardWorkerPhase::preparing;
    if (!state->phase.compare_exchange_strong(
            expected,
            ClipboardWorkerPhase::canceled,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }

    // Record the still-current owner before releasing this generation. A
    // retry on the same clipboard sequence skips the owner snapshot, avoiding
    // an unbounded series of detached workers against the same hung source.
    remember_clipboard_snapshot_bypass();
    release_clipboard_commit_generation(state->commit_generation);
    return true;
}

class ClipboardForwardSetTestPhase {
public:
    ClipboardForwardSetTestPhase() noexcept {
        clipboard_forward_set_pending.store(true, std::memory_order_release);
    }
    ~ClipboardForwardSetTestPhase() {
        clipboard_forward_set_pending.store(false, std::memory_order_release);
    }
    ClipboardForwardSetTestPhase(const ClipboardForwardSetTestPhase&) = delete;
    ClipboardForwardSetTestPhase& operator=(
        const ClipboardForwardSetTestPhase&) = delete;
};

[[nodiscard]] bool consume_clipboard_flush_failure() noexcept {
    unsigned int remaining = clipboard_flush_failures_remaining.load(
        std::memory_order_relaxed);
    while (remaining != 0) {
        if (clipboard_flush_failures_remaining.compare_exchange_weak(
                remaining,
                remaining - 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] HRESULT flush_new_clipboard() noexcept {
    if (consume_clipboard_flush_failure()) {
        return STG_E_WRITEFAULT;
    }
    return OleFlushClipboard();
}

void recover_failed_clipboard_commit(
    ClipboardCommitOutcome& outcome,
    IDataObject* failed_data_object,
    IDataObject* previous_data_object) noexcept {
    outcome.ownership_result = OleIsCurrentClipboard(failed_data_object);
    if (outcome.ownership_result != S_OK) {
        // A concurrent publisher now owns the clipboard (or ownership cannot
        // be proven). Never overwrite that newer state with our old snapshot.
        outcome.disposition = ClipboardCommitDisposition::superseded;
        return;
    }
    if (!outcome.rollback_available || !previous_data_object) {
        outcome.disposition = ClipboardCommitDisposition::uncertain;
        return;
    }

    outcome.rollback_attempted = true;
    outcome.rollback_result = retry_clipboard_rollback_operation(
        [previous_data_object] {
            return OleSetClipboard(previous_data_object);
        });
    if (SUCCEEDED(outcome.rollback_result)) {
        outcome.ownership_result =
            OleIsCurrentClipboard(previous_data_object);
        if (outcome.ownership_result != S_OK) {
            // Another publisher won after rollback set. Do not flush (or claim
            // preservation), because OleFlushClipboard acts on the current
            // clipboard object rather than a supplied object.
            outcome.disposition = ClipboardCommitDisposition::superseded;
            return;
        }
        outcome.rollback_result = retry_clipboard_rollback_operation(
            [] { return OleFlushClipboard(); },
            true);
    }
    outcome.disposition = SUCCEEDED(outcome.rollback_result)
                              ? ClipboardCommitDisposition::preserved
                              : ClipboardCommitDisposition::uncertain;
}

void run_clipboard_worker(
    std::shared_ptr<ClipboardWorkerState> state) noexcept {
    const ClipboardInFlightRelease release_in_flight(
        state->commit_generation);
    const DWORD delay = clipboard_worker_delay_milliseconds.load(
        std::memory_order_relaxed);
    if (delay != 0) {
        Sleep(delay);
    }
    if (state->phase.load(std::memory_order_acquire) ==
        ClipboardWorkerPhase::canceled) {
        state->outcome.result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        state->outcome.disposition = ClipboardCommitDisposition::preserved;
        return;
    }

    const ScopedOleApartment apartment;
    state->outcome.result = apartment.result();
    if (FAILED(state->outcome.result)) {
        state->outcome.disposition = ClipboardCommitDisposition::preserved;
        return;
    }

    // Retain the previous IDataObject in this apartment before any mutation.
    // OLE can wrap Win32 clipboard owners as IDataObject, so this also covers
    // content published through SetClipboardData rather than OleSetClipboard.
    ComPtr<IDataObject> previous_clipboard_view;
    const bool bypass_snapshot = should_bypass_clipboard_snapshot();
    const bool force_snapshot_failure =
        clipboard_snapshot_failure_for_testing.load(
            std::memory_order_relaxed);
    HRESULT snapshot_result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (!bypass_snapshot && !force_snapshot_failure) {
        snapshot_result = retry_clipboard_operation(
            [&previous_clipboard_view] {
                return OleGetClipboard(
                    previous_clipboard_view.ReleaseAndGetAddressOf());
            });
    } else if (force_snapshot_failure) {
        snapshot_result = STG_E_MEDIUMFULL;
    }

    if (state->phase.load(std::memory_order_acquire) ==
        ClipboardWorkerPhase::canceled) {
        state->outcome.result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        state->outcome.disposition = ClipboardCommitDisposition::preserved;
        return;
    }

    // OleGetClipboard may return a forwarding view into a live remote source.
    // Materialize every advertised medium while it still represents the old
    // clipboard; re-publishing that forwarding view directly is not a stable
    // rollback and can recurse into a source that is no longer current.
    ComPtr<IDataObject> previous_data_object;
    if (!bypass_snapshot &&
        !force_snapshot_failure &&
        SUCCEEDED(snapshot_result)) {
        snapshot_result = capture_clipboard_snapshot(
            previous_clipboard_view.Get(),
            previous_data_object);
    }
    previous_clipboard_view.Reset();
    if (state->phase.load(std::memory_order_acquire) ==
        ClipboardWorkerPhase::canceled) {
        state->outcome.result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        state->outcome.disposition = ClipboardCommitDisposition::preserved;
        return;
    }
    if (!bypass_snapshot &&
        !force_snapshot_failure &&
        FAILED(snapshot_result)) {
        // Non-OLE publishers (notably CF_HDROP producers) can expose a default
        // OLE view whose format enumerator is incomplete or transiently fails.
        // Snapshot the concrete system handles as a pre-mutation fallback.
        snapshot_result = capture_win32_clipboard_snapshot(
            previous_data_object);
    }
    state->outcome.snapshot_result = snapshot_result;
    state->outcome.rollback_available = SUCCEEDED(snapshot_result);

    auto* raw_object = new (std::nothrow)
        ClipboardDataObject(std::move(state->formats));
    if (!raw_object) {
        state->outcome.result = E_OUTOFMEMORY;
        state->outcome.disposition = ClipboardCommitDisposition::preserved;
        return;
    }
    ComPtr<IDataObject> data_object;
    data_object.Attach(raw_object);

    ClipboardWorkerPhase expected_phase = ClipboardWorkerPhase::preparing;
    if (!state->phase.compare_exchange_strong(
            expected_phase,
            ClipboardWorkerPhase::mutating,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        state->outcome.result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        state->outcome.disposition = ClipboardCommitDisposition::preserved;
        return;
    }

    state->outcome.result = retry_clipboard_operation(
        [&data_object] { return OleSetClipboard(data_object.Get()); });
    if (FAILED(state->outcome.result)) {
        // OleSetClipboard can fail after partially changing clipboard state,
        // so do not claim preservation unless the retained object is restored
        // and rendered successfully.
        recover_failed_clipboard_commit(
            state->outcome,
            data_object.Get(),
            previous_data_object.Get());
        return;
    }
    clipboard_snapshot_bypass_marker.store(0, std::memory_order_release);

    {
        const ClipboardForwardSetTestPhase forward_set_phase;
        const DWORD pre_flush_delay =
            clipboard_pre_flush_delay_milliseconds.load(
                std::memory_order_relaxed);
        if (pre_flush_delay != 0) {
            wait_for_clipboard_retry(pre_flush_delay);
        }
    }

    state->outcome.ownership_result =
        OleIsCurrentClipboard(data_object.Get());
    if (state->outcome.ownership_result != S_OK) {
        state->outcome.result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        state->outcome.disposition =
            ClipboardCommitDisposition::superseded;
        return;
    }

    // Flush while the source apartment is alive so callers never depend on
    // this transient IDataObject. If final rendering fails, restore and flush
    // the retained previous object before leaving the apartment.
    state->outcome.result = retry_clipboard_flush_operation(
        [] { return flush_new_clipboard(); });
    if (SUCCEEDED(state->outcome.result)) {
        state->outcome.disposition = ClipboardCommitDisposition::success;
        return;
    }

    recover_failed_clipboard_commit(
        state->outcome,
        data_object.Get(),
        previous_data_object.Get());
}

enum class ClipboardWorkerWaitResult {
    completed,
    timed_out,
    failed,
};

[[nodiscard]] ClipboardWorkerWaitResult wait_for_clipboard_worker(
    std::thread& worker,
    DWORD timeout,
    DWORD& wait_error) noexcept {
    wait_error = ERROR_SUCCESS;
    HANDLE worker_handle = worker.native_handle();
    if (!worker_handle) {
        wait_error = ERROR_INVALID_HANDLE;
        return ClipboardWorkerWaitResult::failed;
    }

    const ULONGLONG deadline = GetTickCount64() + timeout;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            const DWORD deadline_wait =
                WaitForSingleObject(worker_handle, 0);
            if (deadline_wait == WAIT_OBJECT_0) {
                try {
                    worker.join();
                    return ClipboardWorkerWaitResult::completed;
                } catch (const std::system_error&) {
                    wait_error = ERROR_INVALID_HANDLE;
                    return ClipboardWorkerWaitResult::failed;
                }
            }
            if (deadline_wait == WAIT_FAILED) {
                wait_error = GetLastError();
                if (wait_error == ERROR_SUCCESS) {
                    wait_error = ERROR_INVALID_HANDLE;
                }
                return ClipboardWorkerWaitResult::failed;
            }
            return ClipboardWorkerWaitResult::timed_out;
        }
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        const DWORD com_wait_slice =
            remaining < kClipboardComWaitSliceMilliseconds
                ? remaining
                : kClipboardComWaitSliceMilliseconds;
        DWORD com_wait_index = 0;
        SetLastError(ERROR_SUCCESS);
        const HRESULT com_wait_result = CoWaitForMultipleHandles(
            COWAIT_DISPATCH_CALLS,
            com_wait_slice,
            1,
            &worker_handle,
            &com_wait_index);
        if (com_wait_result == S_OK && com_wait_index == 0) {
            try {
                worker.join();
                return ClipboardWorkerWaitResult::completed;
            } catch (const std::system_error&) {
                wait_error = ERROR_INVALID_HANDLE;
                return ClipboardWorkerWaitResult::failed;
            }
        }

        // CoWait requires a COM-initialized caller. If this public API is used
        // on an uninitialized thread, retain the same bounded sent-message
        // wait instead of spinning or failing the clipboard operation.
        DWORD sent_message_wait = 0;
        if (FAILED(com_wait_result) &&
            com_wait_result != RPC_S_CALLPENDING) {
            const ULONGLONG after_com_wait = GetTickCount64();
            if (after_com_wait < deadline) {
                const DWORD after_remaining =
                    static_cast<DWORD>(deadline - after_com_wait);
                sent_message_wait =
                    after_remaining < kClipboardComWaitSliceMilliseconds
                        ? after_remaining
                        : kClipboardComWaitSliceMilliseconds;
            }
        }
        const DWORD wait_result = MsgWaitForMultipleObjectsEx(
            1,
            &worker_handle,
            sent_message_wait,
            QS_SENDMESSAGE,
            MWMO_INPUTAVAILABLE);
        if (wait_result == WAIT_OBJECT_0) {
            try {
                worker.join();
                return ClipboardWorkerWaitResult::completed;
            } catch (const std::system_error&) {
                wait_error = ERROR_INVALID_HANDLE;
                return ClipboardWorkerWaitResult::failed;
            }
        }
        if (wait_result == WAIT_TIMEOUT) {
            // Resolve a completion racing the deadline before classifying the
            // worker as detached background work.
            continue;
        }
        if (wait_result != WAIT_OBJECT_0 + 1) {
            wait_error = GetLastError();
            if (wait_error == ERROR_SUCCESS) {
                wait_error = ERROR_INVALID_FUNCTION;
            }
            return ClipboardWorkerWaitResult::failed;
        }

        // PeekMessage dispatches nonqueued sent messages before it examines
        // the posted-message queue. PM_NOREMOVE means normal posted input is
        // never consumed or dispatched while the synchronous caller waits.
        MSG message{};
        (void)PeekMessageW(
            &message,
            nullptr,
            0,
            0,
            PM_NOREMOVE | PM_NOYIELD | PM_QS_SENDMESSAGE);
    }
}

void detach_clipboard_worker(std::thread& worker) noexcept {
    if (!worker.joinable()) {
        return;
    }
    try {
        worker.detach();
    } catch (const std::system_error&) {
        // A successfully started, still-joinable std::thread has a valid
        // native handle. This is defensive only; detach cannot normally fail.
        std::terminate();
    }
}

[[nodiscard]] ClipboardCommitOutcome commit_clipboard_formats(
    std::vector<ClipboardFormatData>&& formats) noexcept {
    const std::uint64_t generation = next_clipboard_commit_generation();
    std::uint64_t expected_generation = 0;
    if (!clipboard_active_commit_generation.compare_exchange_strong(
            expected_generation,
            generation,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return {
            ClipboardCommitDisposition::busy,
            HRESULT_FROM_WIN32(ERROR_BUSY),
        };
    }

    try {
        auto state = std::make_shared<ClipboardWorkerState>(
            std::move(formats),
            generation);
        std::thread worker([state]() noexcept {
            run_clipboard_worker(state);
        });

        const DWORD override_timeout =
            clipboard_wait_timeout_override_milliseconds.load(
                std::memory_order_relaxed);
        const DWORD timeout = override_timeout != 0
                                  ? override_timeout
                                  : kClipboardCommitWaitMilliseconds;
        DWORD wait_error = ERROR_SUCCESS;
        const ClipboardWorkerWaitResult wait_result =
            wait_for_clipboard_worker(worker, timeout, wait_error);
        if (wait_result == ClipboardWorkerWaitResult::completed) {
            return state->outcome;
        }

        const bool preparation_canceled =
            cancel_preparing_clipboard_worker(state);
        detach_clipboard_worker(worker);
        if (wait_result == ClipboardWorkerWaitResult::timed_out) {
            return {
                preparation_canceled
                    ? ClipboardCommitDisposition::preparation_timed_out
                    : ClipboardCommitDisposition::timed_out,
                HRESULT_FROM_WIN32(ERROR_TIMEOUT),
            };
        }
        if (preparation_canceled) {
            return {
                ClipboardCommitDisposition::preserved,
                HRESULT_FROM_WIN32(wait_error),
            };
        }
        return {
            ClipboardCommitDisposition::uncertain,
            HRESULT_FROM_WIN32(wait_error),
        };
    } catch (const std::bad_alloc&) {
        release_clipboard_commit_generation(generation);
        return {
            ClipboardCommitDisposition::preserved,
            E_OUTOFMEMORY,
        };
    } catch (const std::system_error&) {
        release_clipboard_commit_generation(generation);
        return {
            ClipboardCommitDisposition::preserved,
            HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY),
        };
    } catch (...) {
        release_clipboard_commit_generation(generation);
        return {
            ClipboardCommitDisposition::preserved,
            E_FAIL,
        };
    }
}

[[nodiscard]] std::wstring clipboard_commit_error_message(
    const ClipboardCommitOutcome& outcome) {
    const auto hresult_detail = [&outcome] {
        return std::format(
            L"{} (HRESULT 0x{:08X})",
            windows_error_message(static_cast<DWORD>(outcome.result)),
            static_cast<unsigned long>(outcome.result));
    };
    switch (outcome.disposition) {
        case ClipboardCommitDisposition::success:
            return {};
        case ClipboardCommitDisposition::busy:
            return L"已有剪贴板提交仍在后台完成，请稍后重试；本次未更改剪贴板。";
        case ClipboardCommitDisposition::preparation_timed_out:
            return std::format(
                L"读取原剪贴板时超过有界等待，本次提交已取消且不会稍后写入；可立即重试。{}",
                hresult_detail());
        case ClipboardCommitDisposition::timed_out:
            return std::format(
                L"剪贴板已开始提交但在有界等待内未完成，现仍在后台安全收尾；完成前不会接受新的提交。{}",
                hresult_detail());
        case ClipboardCommitDisposition::superseded:
            return std::format(
                L"提交期间剪贴板已被其他应用更新或当前所有权无法确认；为避免覆盖较新内容，未执行回滚。提交错误：{}；所有权 HRESULT 0x{:08X}",
                hresult_detail(),
                static_cast<unsigned long>(outcome.ownership_result));
        case ClipboardCommitDisposition::preserved:
            if (outcome.rollback_attempted) {
                return std::format(
                    L"剪贴板刷新失败，已恢复并固化原剪贴板内容；新内容未提交。{}",
                    hresult_detail());
            }
            return std::format(
                L"无法以事务方式提交剪贴板数据；原剪贴板未更改。{}",
                hresult_detail());
        case ClipboardCommitDisposition::uncertain:
            if (outcome.rollback_attempted) {
                return std::format(
                    L"剪贴板刷新失败，且回滚未能确认完成；当前剪贴板状态不确定。提交错误：{}；回滚 HRESULT 0x{:08X}",
                    hresult_detail(),
                    static_cast<unsigned long>(outcome.rollback_result));
            }
            if (!outcome.rollback_available &&
                FAILED(outcome.snapshot_result)) {
                return std::format(
                    L"新剪贴板提交失败，且原剪贴板无法完整快照，因而无法安全回滚；当前剪贴板状态不确定。提交错误：{}；快照 HRESULT 0x{:08X}",
                    hresult_detail(),
                    static_cast<unsigned long>(outcome.snapshot_result));
            }
            return std::format(
                L"等待剪贴板后台提交时发生错误，当前状态不确定。{}",
                hresult_detail());
    }
    return hresult_detail();
}

class ScreenDc {
public:
    ScreenDc() noexcept : value_(GetDC(nullptr)) {}
    ~ScreenDc() {
        if (value_) {
            ReleaseDC(nullptr, value_);
        }
    }
    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HDC value_{};
};

class WinHandle {
public:
    WinHandle() = default;
    explicit WinHandle(HANDLE value) noexcept : value_(value) {}
    ~WinHandle() {
        if (valid()) {
            CloseHandle(value_);
        }
    }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_{};
};

class TemporaryFile {
public:
    TemporaryFile() = default;
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile() {
        if (!path_.empty()) {
            DeleteFileW(path_.c_str());
        }
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    void release() noexcept { path_.clear(); }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path pictures_directory() {
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &value)) && value) {
        std::filesystem::path result(value);
        CoTaskMemFree(value);
        return result;
    }
    if (value) {
        CoTaskMemFree(value);
    }
    return config_directory();
}

[[nodiscard]] HRESULT create_wic_factory(ComPtr<IWICImagingFactory>& factory) {
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        result =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    }
    return result;
}

[[nodiscard]] HRESULT encode_png(IWICImagingFactory* factory, IStream* stream, const Bitmap& bitmap) {
    if (!factory || !stream || !bitmap.valid()) {
        return E_INVALIDARG;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    HRESULT result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (SUCCEEDED(result)) {
        result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(result)) {
        result = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    }
    if (SUCCEEDED(result)) {
        result = frame->Initialize(properties.Get());
    }
    if (SUCCEEDED(result)) {
        result = frame->SetSize(static_cast<UINT>(bitmap.width), static_cast<UINT>(bitmap.height));
    }
    GUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) {
        result = frame->SetPixelFormat(&format);
    }
    if (SUCCEEDED(result) && !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    if (SUCCEEDED(result)) {
        result = frame->WritePixels(static_cast<UINT>(bitmap.height),
                                    static_cast<UINT>(bitmap.stride_bytes()),
                                    static_cast<UINT>(bitmap.pixels.size()),
                                    const_cast<BYTE*>(bitmap.pixels.data()));
    }
    if (SUCCEEDED(result)) {
        result = frame->Commit();
    }
    if (SUCCEEDED(result)) {
        result = encoder->Commit();
    }
    return result;
}

[[nodiscard]] bool make_opaque_copy(const Bitmap& source, Bitmap& result) noexcept {
    if (!source.valid()) {
        return false;
    }
    try {
        result = source;
    } catch (const std::bad_alloc&) {
        result = {};
        return false;
    } catch (const std::length_error&) {
        result = {};
        return false;
    }
    result.make_opaque();
    return result.valid();
}

[[nodiscard]] bool encode_png_bytes(IWICImagingFactory* factory,
                                    const Bitmap& bitmap,
                                    std::vector<std::uint8_t>& bytes,
                                    HRESULT& error) {
    ComPtr<IStream> stream;
    IStream* raw_stream = nullptr;
    error = CreateStreamOnHGlobal(nullptr, TRUE, &raw_stream);
    if (FAILED(error)) {
        return false;
    }
    stream.Attach(raw_stream);

    error = encode_png(factory, stream.Get(), bitmap);
    if (FAILED(error)) {
        return false;
    }

    HGLOBAL stream_memory = nullptr;
    error = GetHGlobalFromStream(stream.Get(), &stream_memory);
    if (FAILED(error) || !stream_memory) {
        return false;
    }

    STATSTG stream_statistics{};
    error = stream->Stat(&stream_statistics, STATFLAG_NONAME);
    if (FAILED(error)) {
        return false;
    }
    const ULONGLONG logical_size = stream_statistics.cbSize.QuadPart;
    if (logical_size == 0 ||
        logical_size > static_cast<ULONGLONG>(std::numeric_limits<std::size_t>::max())) {
        error = E_FAIL;
        return false;
    }
    const auto byte_count = static_cast<std::size_t>(logical_size);
    if (byte_count > GlobalSize(stream_memory)) {
        error = E_FAIL;
        return false;
    }
    GlobalLockView locked(stream_memory);
    if (!locked.get()) {
        error = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    try {
        const auto* begin = static_cast<const std::uint8_t*>(locked.get());
        bytes.assign(begin, begin + byte_count);
    } catch (const std::bad_alloc&) {
        error = E_OUTOFMEMORY;
        return false;
    } catch (const std::length_error&) {
        error = E_OUTOFMEMORY;
        return false;
    }
    return true;
}

[[nodiscard]] GlobalMemory allocate_global(std::size_t size) {
    if (size == 0) {
        return {};
    }
    return GlobalMemory(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, static_cast<SIZE_T>(size)));
}

[[nodiscard]] GlobalMemory global_from_bytes(std::span<const std::uint8_t> bytes) {
    GlobalMemory memory = allocate_global(bytes.size());
    if (!memory) {
        return {};
    }
    GlobalLockView locked(memory.get());
    if (!locked.get()) {
        return {};
    }
    std::memcpy(locked.get(), bytes.data(), bytes.size());
    return memory;
}

[[nodiscard]] GlobalMemory make_dibv5(const Bitmap& bitmap) {
    std::size_t total_size = 0;
    if (!checked_add(sizeof(BITMAPV5HEADER), bitmap.pixels.size(), total_size)) {
        return {};
    }
    GlobalMemory memory = allocate_global(total_size);
    if (!memory) {
        return {};
    }
    GlobalLockView locked(memory.get());
    if (!locked.get()) {
        return {};
    }

    auto* data = static_cast<std::uint8_t*>(locked.get());
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = bitmap.width;
    header.bV5Height = bitmap.height;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(bitmap.pixels.size());
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = LCS_sRGB;
    std::memcpy(data, &header, sizeof(header));

    auto* destination = data + sizeof(header);
    for (int row = bitmap.height - 1; row >= 0; --row) {
        const auto source_row = bitmap.row(row);
        auto* destination_row =
            destination + static_cast<std::size_t>(bitmap.height - 1 - row) * bitmap.stride_bytes();
        std::memcpy(destination_row, source_row.data(), bitmap.stride_bytes());
    }
    return memory;
}

[[nodiscard]] std::optional<std::size_t> dib24_size(const Bitmap& bitmap,
                                                    std::size_t& stride,
                                                    std::size_t& image_size) noexcept {
    std::size_t raw_stride = 0;
    if (!checked_multiply(static_cast<std::size_t>(bitmap.width), 3, raw_stride) ||
        raw_stride > std::numeric_limits<std::size_t>::max() - 3) {
        return std::nullopt;
    }
    stride = (raw_stride + 3) & ~std::size_t{3};
    if (!checked_multiply(stride, static_cast<std::size_t>(bitmap.height), image_size) ||
        image_size > std::numeric_limits<DWORD>::max()) {
        return std::nullopt;
    }
    std::size_t total_size = 0;
    if (!checked_add(sizeof(BITMAPINFOHEADER), image_size, total_size)) {
        return std::nullopt;
    }
    return total_size;
}

[[nodiscard]] GlobalMemory make_dib24(const Bitmap& bitmap) {
    std::size_t stride = 0;
    std::size_t image_size = 0;
    const auto total_size = dib24_size(bitmap, stride, image_size);
    if (!total_size) {
        return {};
    }
    GlobalMemory memory = allocate_global(*total_size);
    if (!memory) {
        return {};
    }
    GlobalLockView locked(memory.get());
    if (!locked.get()) {
        return {};
    }

    auto* data = static_cast<std::uint8_t*>(locked.get());
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = bitmap.width;
    header.biHeight = bitmap.height;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(image_size);
    std::memcpy(data, &header, sizeof(header));

    auto* destination = data + sizeof(header);
    for (int row = bitmap.height - 1; row >= 0; --row) {
        const auto source_row = bitmap.row(row);
        auto* destination_row = destination + static_cast<std::size_t>(bitmap.height - 1 - row) * stride;
        for (int column = 0; column < bitmap.width; ++column) {
            const std::size_t source_index = static_cast<std::size_t>(column) * Bitmap::bytes_per_pixel;
            const std::size_t destination_index = static_cast<std::size_t>(column) * 3;
            std::memcpy(destination_row + destination_index, source_row.data() + source_index, 3);
        }
    }
    return memory;
}

[[nodiscard]] OwnedBitmap make_device_bitmap(const Bitmap& bitmap) {
    ScreenDc screen;
    if (!screen.get()) {
        return {};
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap.width;
    info.bmiHeader.biHeight = bitmap.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(bitmap.pixels.size());
    void* bits = nullptr;
    OwnedBitmap result(CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!result || !bits) {
        return {};
    }
    auto* destination = static_cast<std::uint8_t*>(bits);
    for (int row = bitmap.height - 1; row >= 0; --row) {
        const auto source_row = bitmap.row(row);
        auto* destination_row =
            destination + static_cast<std::size_t>(bitmap.height - 1 - row) * bitmap.stride_bytes();
        std::memcpy(destination_row, source_row.data(), bitmap.stride_bytes());
    }
    return result;
}

[[nodiscard]] std::optional<std::filesystem::path> reserve_temporary_file(
    const std::filesystem::path& directory,
    DWORD& error) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) {
            error = ERROR_GEN_FAILURE;
            return std::nullopt;
        }
        wchar_t guid_text[40]{};
        if (StringFromGUID2(guid, guid_text, static_cast<int>(std::size(guid_text))) <= 0) {
            error = ERROR_GEN_FAILURE;
            return std::nullopt;
        }
        const auto candidate = directory / std::format(L".airshot-{}.tmp", guid_text);
        WinHandle handle(CreateFileW(candidate.c_str(),
                                     GENERIC_READ | GENERIC_WRITE,
                                     0,
                                     nullptr,
                                     CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL,
                                     nullptr));
        if (handle.valid()) {
            error = ERROR_SUCCESS;
            return candidate;
        }
        error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return std::nullopt;
        }
    }
    error = ERROR_FILE_EXISTS;
    return std::nullopt;
}

[[nodiscard]] HRESULT encode_png_file(IWICImagingFactory* factory,
                                      const Bitmap& bitmap,
                                      const std::filesystem::path& path) {
    ComPtr<IWICStream> stream;
    HRESULT result = factory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    }
    if (SUCCEEDED(result)) {
        result = encode_png(factory, stream.Get(), bitmap);
    }
    return result;
}

[[nodiscard]] bool flush_file_for_commit(const std::filesystem::path& path, DWORD& error) {
    WinHandle handle(CreateFileW(path.c_str(),
                                 GENERIC_WRITE,
                                 0,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                 nullptr));
    if (!handle.valid()) {
        error = GetLastError();
        return false;
    }
    if (!FlushFileBuffers(handle.get())) {
        error = GetLastError();
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] bool replace_file_atomically(const std::filesystem::path& temporary,
                                           const std::filesystem::path& destination,
                                           DWORD& error) {
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            error = ERROR_ACCESS_DENIED;
            return false;
        }
        if (!ReplaceFileW(destination.c_str(),
                          temporary.c_str(),
                          nullptr,
                          0,
                          nullptr,
                          nullptr)) {
            error = GetLastError();
            return false;
        }
    } else {
        const DWORD attributes_error = GetLastError();
        if (attributes_error != ERROR_FILE_NOT_FOUND && attributes_error != ERROR_PATH_NOT_FOUND) {
            error = attributes_error;
            return false;
        }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = GetLastError();
            return false;
        }
    }
    error = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] bool has_png_extension(const std::filesystem::path& path) {
    const std::wstring extension = path.extension().wstring();
    return _wcsicmp(extension.c_str(), L".png") == 0;
}

[[nodiscard]] bool is_reserved_device_filename(
    const std::filesystem::path& path) {
    std::wstring name = path.filename().wstring();
    while (!name.empty() &&
           (name.back() == L' ' || name.back() == L'.')) {
        name.pop_back();
    }
    const std::size_t extension = name.find(L'.');
    if (extension != std::wstring::npos) {
        name.resize(extension);
    }
    while (!name.empty() && name.back() == L' ') {
        name.pop_back();
    }
    std::ranges::transform(
        name,
        name.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(
                std::towupper(character));
        });
    if (name == L"CON" || name == L"PRN" ||
        name == L"AUX" || name == L"NUL" ||
        name == L"CLOCK$" || name == L"CONIN$" ||
        name == L"CONOUT$") {
        return true;
    }
    return name.size() == 4U &&
           (name.starts_with(L"COM") ||
            name.starts_with(L"LPT")) &&
           name.back() >= L'1' && name.back() <= L'9';
}

[[nodiscard]] std::filesystem::path unique_generated_path(const std::filesystem::path& directory) {
    const std::wstring stem = std::format(L"AirShot-{}", timestamp_for_file());
    GUID guid{};
    wchar_t guid_text[40]{};
    if (SUCCEEDED(CoCreateGuid(&guid)) &&
        StringFromGUID2(guid, guid_text, static_cast<int>(std::size(guid_text))) > 2) {
        const std::wstring_view value(guid_text);
        return directory /
               std::format(L"{}-{}.png", stem, value.substr(1, value.size() - 2));
    }

    static std::atomic_uint64_t fallback_sequence{};
    return directory /
           std::format(L"{}-{}-{}-{}.png",
                       stem,
                       GetCurrentProcessId(),
                       GetTickCount64(),
                       fallback_sequence.fetch_add(1, std::memory_order_relaxed));
}

}  // namespace

namespace output_test {

void set_required_clipboard_format_failure_for_testing(
    std::size_t one_based_index) noexcept {
    required_clipboard_format_failure_index.store(
        one_based_index,
        std::memory_order_relaxed);
}

void set_clipboard_flush_failure_for_testing(bool enabled) noexcept {
    clipboard_flush_failures_remaining.store(
        enabled ? 1U : 0U,
        std::memory_order_relaxed);
}

void set_clipboard_worker_delay_for_testing(
    std::uint32_t milliseconds) noexcept {
    clipboard_worker_delay_milliseconds.store(
        static_cast<DWORD>(milliseconds),
        std::memory_order_relaxed);
}

void set_clipboard_wait_timeout_for_testing(
    std::uint32_t milliseconds) noexcept {
    clipboard_wait_timeout_override_milliseconds.store(
        static_cast<DWORD>(milliseconds),
        std::memory_order_relaxed);
}

void set_clipboard_pre_flush_delay_for_testing(
    std::uint32_t milliseconds) noexcept {
    clipboard_pre_flush_delay_milliseconds.store(
        static_cast<DWORD>(milliseconds),
        std::memory_order_relaxed);
}

void set_clipboard_snapshot_failure_for_testing(bool enabled) noexcept {
    clipboard_snapshot_failure_for_testing.store(
        enabled,
        std::memory_order_relaxed);
}

bool clipboard_commit_in_flight_for_testing() noexcept {
    return clipboard_active_commit_generation.load(
               std::memory_order_acquire) != 0;
}

bool clipboard_forward_set_pending_for_testing() noexcept {
    return clipboard_forward_set_pending.load(std::memory_order_acquire);
}

}  // namespace output_test

bool copy_bitmap_to_clipboard(HWND owner, const Bitmap& bitmap, std::wstring* error) {
    clear_error(error);
    (void)owner;
    if (!bitmap.valid()) {
        set_error(error, L"图像为空或像素缓冲区无效。");
        return false;
    }

    std::size_t dibv5_bytes = 0;
    if (!checked_add(sizeof(BITMAPV5HEADER), bitmap.pixels.size(), dibv5_bytes) ||
        dibv5_bytes > kClipboardBudget) {
        set_error(error, L"图像过大，超过 512 MiB 剪贴板预算。");
        return false;
    }

    const ScopedWinrtApartment apartment(true);
    if (!apartment.available()) {
        set_error(error, L"无法初始化图像编码组件。");
        return false;
    }

    Bitmap opaque_bitmap;
    if (!make_opaque_copy(bitmap, opaque_bitmap)) {
        set_error(error, L"无法为剪贴板分配图像内存。");
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT encoding_error = create_wic_factory(factory);
    if (FAILED(encoding_error)) {
        set_error(error, windows_error_message(static_cast<DWORD>(encoding_error)));
        return false;
    }
    std::vector<std::uint8_t> png_bytes;
    if (!encode_png_bytes(factory.Get(), opaque_bitmap, png_bytes, encoding_error)) {
        set_error(error, windows_error_message(static_cast<DWORD>(encoding_error)));
        return false;
    }

    std::size_t png_pair_bytes = 0;
    std::size_t mandatory_bytes = 0;
    if (!checked_multiply(png_bytes.size(), 2, png_pair_bytes) ||
        !checked_add(dibv5_bytes, png_pair_bytes, mandatory_bytes) ||
        mandatory_bytes > kClipboardBudget) {
        set_error(error, L"图像过大，超过 512 MiB 剪贴板预算。");
        return false;
    }

    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    const UINT png_format_alt = RegisterClipboardFormatW(L"image/png");
    if (png_format == 0 || png_format_alt == 0) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }

    GlobalMemory png_memory = global_from_bytes(png_bytes);
    GlobalMemory png_memory_alt = global_from_bytes(png_bytes);
    GlobalMemory dibv5_memory = make_dibv5(opaque_bitmap);
    if (!png_memory || !png_memory_alt || !dibv5_memory) {
        set_error(error, L"无法为剪贴板分配图像内存。");
        return false;
    }

    GlobalMemory dib_memory;
    OwnedBitmap device_bitmap;
    std::size_t dib_stride = 0;
    std::size_t dib_image_bytes = 0;
    const auto optional_dib_bytes = dib24_size(opaque_bitmap, dib_stride, dib_image_bytes);
    std::size_t optional_total = 0;
    std::size_t with_dib = 0;
    if (optional_dib_bytes &&
        checked_add(mandatory_bytes, *optional_dib_bytes, with_dib) &&
        checked_add(with_dib, opaque_bitmap.pixels.size(), optional_total) &&
        optional_total <= kClipboardBudget) {
        dib_memory = make_dib24(opaque_bitmap);
        device_bitmap = make_device_bitmap(opaque_bitmap);
        if (!dib_memory || !device_bitmap) {
            dib_memory = {};
            device_bitmap = {};
        }
    }

    std::vector<ClipboardFormatData> formats;
    if (!reserve_clipboard_formats(formats, 5) ||
        !stage_required_clipboard_format(
            formats, 1, png_format, std::move(png_memory)) ||
        !stage_required_clipboard_format(
            formats, 2, png_format_alt, std::move(png_memory_alt)) ||
        !stage_required_clipboard_format(
            formats, 3, CF_DIBV5, std::move(dibv5_memory))) {
        set_error(
            error,
            L"无法完整准备 PNG、image/png 和 CF_DIBV5 剪贴板格式；原剪贴板未更改。");
        return false;
    }

    if (dib_memory && device_bitmap) {
        stage_optional_clipboard_format(
            formats, CF_DIB, std::move(dib_memory));
        stage_optional_clipboard_format(
            formats, CF_BITMAP, std::move(device_bitmap));
    }

    const ClipboardCommitOutcome commit_result =
        commit_clipboard_formats(std::move(formats));
    if (!commit_result.succeeded()) {
        set_error(error, clipboard_commit_error_message(commit_result));
        return false;
    }
    return true;
}

bool copy_bitmap_to_clipboard(const Bitmap& bitmap, std::wstring* error) {
    return copy_bitmap_to_clipboard(nullptr, bitmap, error);
}

bool copy_text_to_clipboard(HWND owner, std::wstring_view text, std::wstring* error) {
    clear_error(error);
    (void)owner;
    if (text.size() > std::numeric_limits<std::size_t>::max() / sizeof(wchar_t) - 1) {
        set_error(error, L"文本过大，无法复制到剪贴板。");
        return false;
    }

    const std::size_t bytes = (text.size() + 1U) * sizeof(wchar_t);
    GlobalMemory memory = allocate_global(bytes);
    if (!memory) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    {
        GlobalLockView locked(memory.get());
        if (!locked.get()) {
            set_error(error, windows_error_message(GetLastError()));
            return false;
        }
        if (!text.empty()) {
            std::memcpy(locked.get(), text.data(), text.size() * sizeof(wchar_t));
        }
    }

    std::vector<ClipboardFormatData> formats;
    if (!reserve_clipboard_formats(formats, 1) ||
        !stage_required_clipboard_format(
            formats, 1, CF_UNICODETEXT, std::move(memory))) {
        set_error(
            error,
            L"无法完整准备 Unicode 文本剪贴板格式；原剪贴板未更改。");
        return false;
    }

    const ClipboardCommitOutcome commit_result =
        commit_clipboard_formats(std::move(formats));
    if (!commit_result.succeeded()) {
        set_error(error, clipboard_commit_error_message(commit_result));
        return false;
    }
    return true;
}

bool copy_text_to_clipboard(std::wstring_view text, std::wstring* error) {
    return copy_text_to_clipboard(nullptr, text, error);
}

bool save_png(const Bitmap& bitmap, const std::filesystem::path& path, std::wstring* error) {
    clear_error(error);
    if (!bitmap.valid()) {
        set_error(error, L"图像为空或像素缓冲区无效。");
        return false;
    }
    if (path.empty() || path.filename().empty()) {
        set_error(error, L"输出路径无效。");
        return false;
    }
    if (!has_png_extension(path)) {
        set_error(error, L"输出文件必须使用 .png 扩展名。");
        return false;
    }
    if (is_reserved_device_filename(path)) {
        set_error(error, L"输出文件名不能使用 Windows 保留设备名。");
        return false;
    }

    const std::filesystem::path directory = path.parent_path();
    if (!directory.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(directory, directory_error);
        if (directory_error) {
            set_error(error, windows_error_message(static_cast<DWORD>(directory_error.value())));
            return false;
        }
    }

    DWORD temporary_error = ERROR_SUCCESS;
    const auto temporary_path = reserve_temporary_file(directory, temporary_error);
    if (!temporary_path) {
        set_error(error, windows_error_message(temporary_error));
        return false;
    }
    TemporaryFile temporary(*temporary_path);

    const ScopedWinrtApartment apartment(true);
    if (!apartment.available()) {
        set_error(error, L"无法初始化图像编码组件。");
        return false;
    }
    Bitmap opaque_bitmap;
    if (!make_opaque_copy(bitmap, opaque_bitmap)) {
        set_error(error, L"无法为 PNG 编码分配图像内存。");
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = create_wic_factory(factory);
    if (SUCCEEDED(result)) {
        result = encode_png_file(factory.Get(), opaque_bitmap, temporary.path());
    }
    factory.Reset();
    if (FAILED(result)) {
        set_error(error, windows_error_message(static_cast<DWORD>(result)));
        return false;
    }

    DWORD file_error = ERROR_SUCCESS;
    if (!flush_file_for_commit(temporary.path(), file_error)) {
        set_error(error, windows_error_message(file_error));
        return false;
    }
    if (!replace_file_atomically(temporary.path(), path, file_error)) {
        set_error(error, windows_error_message(file_error));
        return false;
    }
    temporary.release();
    return true;
}

std::filesystem::path resolve_output_path(std::wstring_view requested) {
    if (requested.empty()) {
        return unique_generated_path(pictures_directory() / L"Air Screenshot");
    }

    const bool trailing_separator = requested.back() == L'\\' || requested.back() == L'/';
    std::filesystem::path path(requested);
    std::error_code code;
    if (trailing_separator || std::filesystem::is_directory(path, code)) {
        return unique_generated_path(path);
    }
    if (!path.has_extension()) {
        path += L".png";
    }
    return path;
}

std::optional<std::filesystem::path> prompt_png_path(HWND owner) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) {
        return std::nullopt;
    }
    const COMDLG_FILTERSPEC filter[] = {{L"PNG 图像", L"*.png"}};
    dialog->SetFileTypes(1, filter);
    dialog->SetDefaultExtension(L"png");
    const std::wstring name = std::format(L"AirShot-{}.png", timestamp_for_file());
    dialog->SetFileName(name.c_str());
    if (FAILED(dialog->Show(owner))) {
        return std::nullopt;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) {
        return std::nullopt;
    }
    PWSTR value = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) || !value) {
        return std::nullopt;
    }
    std::filesystem::path result(value);
    CoTaskMemFree(value);
    return result;
}

}  // namespace airshot
