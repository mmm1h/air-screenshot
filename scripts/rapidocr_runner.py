import argparse
import os
from pathlib import Path
import stat
import sys

SUPPORTED_PROFILES = {
    "rapidocr-v5-fast",
    "rapidocr-v5-accurate",
    "rapidocr-v4-compat",
}
REQUIRED_MODEL_FILES = ("det.onnx", "rec.onnx", "cls.onnx", "dict.txt")
MAX_IMAGE_BYTES = 128 * 1024 * 1024


def is_regular_non_reparse_file(path: Path) -> bool:
    try:
        metadata = path.lstat()
        return (
            stat.S_ISREG(metadata.st_mode)
            and not metadata.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT
        )
    except (AttributeError, OSError):
        return False


def is_non_reparse_directory(path: Path) -> bool:
    try:
        metadata = path.lstat()
        return (
            stat.S_ISDIR(metadata.st_mode)
            and not metadata.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT
        )
    except (AttributeError, OSError):
        return False


def build_params(profile: str, model_dir: Path, thread_count: int) -> dict:
    from rapidocr import EngineType, OCRVersion

    version = OCRVersion.PPOCRV5 if profile.startswith("rapidocr-v5") else OCRVersion.PPOCRV4
    return {
        "Global.log_level": "critical",
        "Det.engine_type": EngineType.ONNXRUNTIME,
        "Det.ocr_version": version,
        "Det.model_path": str(model_dir / "det.onnx"),
        "Cls.engine_type": EngineType.ONNXRUNTIME,
        "Cls.ocr_version": version,
        "Cls.model_path": str(model_dir / "cls.onnx"),
        "Rec.engine_type": EngineType.ONNXRUNTIME,
        "Rec.ocr_version": version,
        "Rec.model_path": str(model_dir / "rec.onnx"),
        "Rec.rec_keys_path": str(model_dir / "dict.txt"),
        "EngineConfig.onnxruntime.intra_op_num_threads": thread_count,
        "EngineConfig.onnxruntime.inter_op_num_threads": 1,
    }


def main() -> int:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--image", required=True)
    parser.add_argument("--model-dir", required=True)
    parser.add_argument(
        "--ocr-profile",
        choices=sorted(SUPPORTED_PROFILES),
        default="rapidocr-v5-fast",
    )
    parser.add_argument("--ort-threads", type=int, default=2)
    args = parser.parse_args()

    sys.stdout.reconfigure(encoding="utf-8", errors="strict", newline="\n")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace", newline="\n")

    image_path = Path(args.image)
    model_dir = Path(args.model_dir)
    thread_count = min(max(args.ort_threads, 1), 16)
    if (
        not is_regular_non_reparse_file(image_path)
        or image_path.suffix.lower() != ".png"
        or image_path.stat().st_size > MAX_IMAGE_BYTES
    ):
        print("OCR input must be a regular PNG file no larger than 128 MiB.", file=sys.stderr)
        return 2
    if not is_non_reparse_directory(model_dir):
        print("OCR model directory is missing or is a reparse point.", file=sys.stderr)
        return 2
    if any(not is_regular_non_reparse_file(model_dir / name) for name in REQUIRED_MODEL_FILES):
        print("OCR model directory is incomplete or contains a reparse point.", file=sys.stderr)
        return 2

    os.environ["OMP_NUM_THREADS"] = str(thread_count)
    os.environ["OMP_WAIT_POLICY"] = "PASSIVE"
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["NO_PROXY"] = "*"

    try:
        from rapidocr import RapidOCR

        engine = RapidOCR(params=build_params(args.ocr_profile, model_dir, thread_count))
        result = engine(str(image_path))
        text = "\n".join(result.txts or [])
        if not text.strip():
            print("RapidOCR did not recognize any text.", file=sys.stderr)
            return 2
        print(text, end="")
        return 0
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
