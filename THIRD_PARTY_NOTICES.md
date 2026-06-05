# Third-party notices

Air Screenshot is licensed under GNU LGPL-3.0-only.

## xland/ScreenCapture

Copyright (C) 2022 Liu Xiao Lun.

Source: <https://github.com/xland/ScreenCapture>

Air Screenshot selectively adapts ideas and portions of the native Windows implementation from:

- `main` commit `1574683043fa5f64b6cd45d9ec2e0db1bafbc15b`
- Window rendering, window highlighting, selection-mask behavior, and annotation-tool design

The Qt branch at commit `768b6500f259aa229b8a727a19b8b105605aaa83` was used only as a behavioral reference. No Qt code is included.

Modified files in this repository carry their own project copyright and remain available under LGPL-3.0-only.

## flameshot-org/Flameshot

Source: <https://github.com/flameshot-org/flameshot>

Air Screenshot uses Flameshot as a behavior and interaction reference for screenshot and annotation workflows. No Flameshot code is included in this repository by this notice alone; future code-level adaptations should be documented with exact commit and file provenance.

## RapidAI/RapidOCR

Source: <https://github.com/RapidAI/RapidOCR>

The optional local high-accuracy OCR runtime is designed to use RapidOCR-compatible native inference through a sidecar process and an adapter DLL. RapidOCR dependency binaries and models are distributed separately from the main executable and must be covered by their own release manifest and licenses.

## PaddlePaddle/PaddleOCR Models

Source: <https://github.com/PaddlePaddle/PaddleOCR>

The optional local high-accuracy OCR dependency package targets PP-OCRv5 mobile detection, recognition, and text-line orientation models. Model files are downloaded separately and verified by SHA256 before installation.

## Microsoft ONNX Runtime

Source: <https://github.com/microsoft/onnxruntime>

The optional local high-accuracy OCR dependency package uses the CPU ONNX Runtime DLL for local inference. The main Air Screenshot host does not load ONNX Runtime directly; OCR dependencies are loaded only inside `airshot_ocr.exe`.
