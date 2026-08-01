# Third-party notices

Air Screenshot is distributed under `LGPL-3.0-only`. The canonical license
text is included in `LICENSE`. This notice records third-party material that is
adapted by the source tree or shipped in the optional OCR payload; it does not
replace the corresponding upstream license terms.

Release builds also include `AirScreenshot.spdx.json`. The separately signed
OCR payload carries its native runtime and license files alongside the models.

## Material adapted by the main program

### xland/ScreenCapture

- Copyright: Copyright (C) 2022 Liu Xiao Lun
- License: `LGPL-3.0-only`
- Source: <https://github.com/xland/ScreenCapture>
- Revision: `1574683043fa5f64b6cd45d9ec2e0db1bafbc15b`

Air Screenshot adapts portions of the native Windows implementation concerning
window rendering and highlighting, selection-mask behavior, and annotation-tool
design. The Qt branch at revision
`768b6500f259aa229b8a727a19b8b105605aaa83` was used only as a behavioral
reference; no Qt dependency is shipped.

## Optional OCR payload

The following components are downloaded separately from the single-file
`AirScreenshot.exe`. OCR runs in a child process started through the main
executable's internal OCR mode.

| Component | Resolved direct version | License | Source |
| --- | --- | --- | --- |
| paddle-ocr-rs | 0.6.1 | Apache-2.0 | <https://github.com/mg-chao/paddle-ocr-rs> |
| ort Rust bindings | 2.0.0-rc.10 | MIT OR Apache-2.0 | <https://github.com/pykeio/ort> |
| image | 0.25.10 | MIT OR Apache-2.0 | <https://github.com/image-rs/image> |
| serde / serde_json | 1.0.229 / 1.0.151 | MIT OR Apache-2.0 | <https://github.com/serde-rs/serde> |
| windows-sys | 0.60.2 | MIT OR Apache-2.0 | <https://github.com/microsoft/windows-rs> |
| Microsoft ONNX Runtime | 1.22.0 | MIT | <https://github.com/microsoft/onnxruntime> |
| Rust standard library | 1.88.0 | Apache-2.0 OR MIT | <https://github.com/rust-lang/rust> |
| Microsoft Visual C++ Redistributable | Visual Studio 2022 / VC143 | Microsoft redistributable terms | <https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist> |
| RapidAI/RapidOCR model distribution | 3.8.0 | Apache-2.0 | <https://github.com/RapidAI/RapidOCR> |
| PaddleOCR model files | PP-OCRv4 / PP-OCRv5 | Apache-2.0 | <https://github.com/PaddlePaddle/PaddleOCR> |

`rapidocr_runner.exe` is compiled from the repository's Rust source; no Python
interpreter, Python package, or PyInstaller bootloader is shipped. The payload
retains the ONNX Runtime and paddle-ocr-rs license texts plus ONNX Runtime's
third-party notices. It also includes `rust-crates.json` for the exact locked
Windows dependency graph and an aggregated `rust-crates-NOTICES.txt` containing
the license files distributed by those crate archives. Rust's standard-library
copyright and license inventory is included separately as
`rust-standard-library-COPYRIGHT.html`.

## Behavioral references not redistributed

Snipaste (<https://www.snipaste.com/>), PixPin
(<https://pixpin.com/>), Snow Shot
(<https://github.com/mg-chao/snow-shot>), and Flameshot
(<https://github.com/flameshot-org/flameshot>), together with ShareX's official
scrolling-capture documentation
(<https://getsharex.com/docs/scrolling-screenshot.html>), are used only as
behavioral and interaction references for capture, annotation, scrolling
capture, pinning, and OCR workflows. No code, binary, brand asset, or
proprietary artwork from those products is included on that basis.
