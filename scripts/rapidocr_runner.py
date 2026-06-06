import argparse
import sys

from rapidocr import EngineType, OCRVersion, RapidOCR


def build_params(profile: str, model_dir: str, thread_count: int) -> dict:
    version = OCRVersion.PPOCRV5 if profile.startswith("rapidocr-v5") else OCRVersion.PPOCRV4
    return {
        "Global.log_level": "critical",
        "Det.engine_type": EngineType.ONNXRUNTIME,
        "Det.ocr_version": version,
        "Det.model_path": f"{model_dir}/det.onnx",
        "Cls.engine_type": EngineType.ONNXRUNTIME,
        "Cls.ocr_version": version,
        "Cls.model_path": f"{model_dir}/cls.onnx",
        "Rec.engine_type": EngineType.ONNXRUNTIME,
        "Rec.ocr_version": version,
        "Rec.model_path": f"{model_dir}/rec.onnx",
        "Rec.rec_keys_path": f"{model_dir}/dict.txt",
        "EngineConfig.onnxruntime.intra_op_num_threads": max(1, thread_count),
        "EngineConfig.onnxruntime.inter_op_num_threads": 1,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--model-dir", required=True)
    parser.add_argument("--ocr-profile", default="rapidocr-v5-fast")
    parser.add_argument("--ort-threads", type=int, default=2)
    args = parser.parse_args()

    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

    try:
        engine = RapidOCR(params=build_params(args.ocr_profile, args.model_dir, args.ort_threads))
        result = engine(args.image)
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
