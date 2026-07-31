# Third-party notices

Air Screenshot is distributed under `LGPL-3.0-only`. The canonical license
text is included in `LICENSE`. This notice records third-party material that is
adapted by the source tree or shipped in the optional OCR payload; it does not
replace the corresponding upstream license terms.

Release builds also include `AirScreenshot.spdx.json`. That SBOM records the
versions resolved into a particular OCR payload, including transitive Python
packages that are not individually enumerated here.

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
| RapidAI/RapidOCR | 3.8.1 | Apache-2.0 | <https://github.com/RapidAI/RapidOCR> |
| Microsoft ONNX Runtime | 1.20.1 | MIT | <https://github.com/microsoft/onnxruntime> |
| PyInstaller | 6.17.0 | GPL-2.0-or-later with Bootloader Exception | <https://github.com/pyinstaller/pyinstaller> |
| PaddleOCR model files | PP-OCRv4 / PP-OCRv5 | Apache-2.0 | <https://github.com/PaddlePaddle/PaddleOCR> |

PyInstaller's bootloader exception permits distribution of the generated
`rapidocr_runner.exe` under the licenses applicable to the bundled application
and libraries. The payload retains license and package metadata supplied by its
dependencies where available.

## Behavioral references not redistributed

Snipaste (<https://www.snipaste.com/>), PixPin
(<https://pixpin.cn/>), Snow Shot
(<https://github.com/mg-chao/snow-shot>), and Flameshot
(<https://github.com/flameshot-org/flameshot>) are used as behavioral and
interaction references for capture, annotation, scrolling capture, pinning,
and OCR workflows. No code, binary, brand asset, or proprietary artwork from
those products is included on that basis.
