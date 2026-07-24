# -*- mode: python ; coding: utf-8 -*-
import os

from PyInstaller.utils.hooks import collect_data_files


runner_script = os.environ["AIRSHOT_RAPIDOCR_RUNNER_SCRIPT"]
rapidocr_data = collect_data_files(
    "rapidocr",
    includes=["config.yaml", "default_models.yaml"],
)

a = Analysis(
    [runner_script],
    pathex=[],
    binaries=[],
    datas=rapidocr_data,
    hiddenimports=[
        "rapidocr.inference_engine.onnxruntime",
        "onnxruntime.capi._pybind_state",
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        "MNN",
        "openvino",
        "paddle",
        "tensorrt",
        "torch",
        "torchvision",
    ],
    noarchive=False,
    optimize=1,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="rapidocr_runner",
    console=True,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    name="rapidocr_runner",
)
