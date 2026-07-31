import argparse
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import queue
import stat
import struct
import sys
import threading
import time


SCHEMA_VERSION = 1
SUPPORTED_PROFILES = {
    "rapidocr-v5-fast",
    "rapidocr-v5-accurate",
    "rapidocr-v4-compat",
}
REQUIRED_MODEL_FILES = ("det.onnx", "rec.onnx", "cls.onnx", "dict.txt")
MAX_IMAGE_BYTES = 128 * 1024 * 1024
MAX_DECODED_PIXELS = 40 * 1024 * 1024
MAX_BLOCKS = 16_384
TILE_OVERLAP = 128
WORKER_SCHEMA_VERSION = 1
WORKER_IDLE_TIMEOUT_SECONDS = 60.0
MAX_WORKER_REQUEST_BYTES = 64 * 1024
MAX_WORKER_RESPONSE_BYTES = 8 * 1024 * 1024
MAX_SAFE_JSON_INTEGER = 9_007_199_254_740_991

PROFILE_SETTINGS = {
    "rapidocr-v5-fast": {
        "max_side_len": 2048,
        "det_limit_side_len": 1536,
        "text_score": 0.50,
        "score_mode": "fast",
    },
    "rapidocr-v5-accurate": {
        "max_side_len": 3072,
        "det_limit_side_len": 2048,
        "text_score": 0.50,
        "score_mode": "slow",
    },
    "rapidocr-v4-compat": {
        "max_side_len": 2048,
        "det_limit_side_len": 1536,
        "text_score": 0.50,
        "score_mode": "fast",
    },
}


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
    settings = PROFILE_SETTINGS[profile]
    return {
        "Global.log_level": "critical",
        "Global.model_root_dir": str(model_dir),
        "Global.text_score": settings["text_score"],
        "Global.max_side_len": settings["max_side_len"],
        "Global.min_side_len": 30,
        "Global.use_cls": True,
        "Det.engine_type": EngineType.ONNXRUNTIME,
        "Det.ocr_version": version,
        "Det.model_path": str(model_dir / "det.onnx"),
        "Det.limit_type": "max",
        "Det.limit_side_len": settings["det_limit_side_len"],
        "Det.thresh": 0.30,
        "Det.box_thresh": 0.50,
        "Det.max_candidates": 2000,
        "Det.unclip_ratio": 1.60,
        "Det.use_dilation": True,
        "Det.score_mode": settings["score_mode"],
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


@contextmanager
def redirect_process_stdout_to_stderr():
    """Keep Python and native-library diagnostics away from protocol stdout."""
    sys.stdout.flush()
    sys.stderr.flush()
    saved_stdout = os.dup(sys.stdout.fileno())
    try:
        os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
        yield
    finally:
        sys.stdout.flush()
        sys.stderr.flush()
        os.dup2(saved_stdout, sys.stdout.fileno())
        os.close(saved_stdout)


def read_png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        header = stream.read(24)
    if (
        len(header) != 24
        or header[:8] != b"\x89PNG\r\n\x1a\n"
        or header[12:16] != b"IHDR"
        or struct.unpack(">I", header[8:12])[0] != 13
    ):
        raise ValueError("OCR input is not a canonical PNG image.")
    width, height = struct.unpack(">II", header[16:24])
    if width <= 0 or height <= 0 or width > 100_000 or height > 100_000:
        raise ValueError("OCR PNG dimensions are outside the supported range.")
    if width * height > MAX_DECODED_PIXELS:
        raise ValueError("OCR PNG decoded pixel count exceeds the safety limit.")
    return width, height


def tile_starts(length: int, tile_size: int, overlap: int) -> list[int]:
    if length <= tile_size:
        return [0]
    step = tile_size - overlap
    starts = list(range(0, length - tile_size + 1, step))
    final_start = length - tile_size
    if starts[-1] != final_start:
        starts.append(final_start)
    return starts


def axis_tiles(length: int, tile_size: int, overlap: int) -> list[tuple[int, int, float, float]]:
    starts = tile_starts(length, tile_size, overlap)
    sizes = [min(tile_size, length - start) for start in starts]
    boundaries = [
        (starts[index] + sizes[index] + starts[index + 1]) * 0.5
        for index in range(len(starts) - 1)
    ]
    return [
        (
            start,
            sizes[index],
            0.0 if index == 0 else boundaries[index - 1],
            float(length) if index + 1 == len(starts) else boundaries[index],
        )
        for index, start in enumerate(starts)
    ]


def make_tiles(
    width: int,
    height: int,
    tile_size: int,
    overlap: int,
) -> list[tuple[int, int, int, int, float, float, float, float]]:
    x_tiles = axis_tiles(width, tile_size, overlap)
    y_tiles = axis_tiles(height, tile_size, overlap)
    tiles = [
        (x, y, tile_width, tile_height, owner_left, owner_top, owner_right, owner_bottom)
        for y, tile_height, owner_top, owner_bottom in y_tiles
        for x, tile_width, owner_left, owner_right in x_tiles
    ]
    if len(tiles) > 512:
        raise ValueError("OCR image requires too many tiles.")
    return tiles


def clean_text(value: object) -> str:
    text = str(value)
    text = "".join(" " if ord(character) < 32 or ord(character) == 127 else character for character in text)
    return " ".join(text.split())


def quad_bounds(block: dict) -> tuple[float, float, float, float]:
    xs = [point[0] for point in block["quad"]]
    ys = [point[1] for point in block["quad"]]
    return min(xs), min(ys), max(xs), max(ys)


def normalized_text(text: str) -> str:
    return "".join(text.split()).casefold()


def duplicate_blocks(first: dict, second: dict) -> bool:
    if normalized_text(first["text"]) != normalized_text(second["text"]):
        return False
    first_left, first_top, first_right, first_bottom = quad_bounds(first)
    second_left, second_top, second_right, second_bottom = quad_bounds(second)
    intersection_width = max(0.0, min(first_right, second_right) - max(first_left, second_left))
    intersection_height = max(0.0, min(first_bottom, second_bottom) - max(first_top, second_top))
    intersection = intersection_width * intersection_height
    first_area = max(0.0, first_right - first_left) * max(0.0, first_bottom - first_top)
    second_area = max(0.0, second_right - second_left) * max(0.0, second_bottom - second_top)
    if min(first_area, second_area) <= 0.0:
        return False
    union = first_area + second_area - intersection
    iou = intersection / union if union > 0.0 else 0.0
    overlap_of_smaller = intersection / min(first_area, second_area)
    return iou >= 0.35 or overlap_of_smaller >= 0.65


def deduplicate_blocks(blocks: list[dict]) -> list[dict]:
    retained: list[dict] = []
    for candidate in blocks:
        duplicate_index = next(
            (index for index, current in enumerate(retained) if duplicate_blocks(current, candidate)),
            None,
        )
        if duplicate_index is None:
            retained.append(candidate)
        elif candidate["score"] > retained[duplicate_index]["score"]:
            retained[duplicate_index] = candidate
    return retained


def block_owned_by_tile(
    block: dict,
    tile: tuple[int, int, int, int, float, float, float, float],
) -> bool:
    _, _, _, _, owner_left, owner_top, owner_right, owner_bottom = tile
    left, top, right, bottom = quad_bounds(block)
    center_x = (left + right) * 0.5
    center_y = (top + bottom) * 0.5
    return owner_left <= center_x < owner_right and owner_top <= center_y < owner_bottom


def extract_result_blocks(
    result: object,
    offset_x: int,
    offset_y: int,
    image_width: int,
    image_height: int,
) -> list[dict]:
    boxes = getattr(result, "boxes", None)
    texts = getattr(result, "txts", None)
    scores = getattr(result, "scores", None)
    if boxes is None and texts is None and scores is None:
        return []
    if boxes is None or texts is None or scores is None:
        raise RuntimeError("RapidOCR returned a partial result.")
    if len(boxes) != len(texts) or len(boxes) != len(scores):
        raise RuntimeError("RapidOCR returned mismatched result arrays.")

    blocks: list[dict] = []
    for box, raw_text, raw_score in zip(boxes, texts, scores):
        text = clean_text(raw_text)
        score = float(raw_score)
        if not text or not math.isfinite(score) or score < 0.0 or score > 1.0:
            continue
        points = box.tolist() if hasattr(box, "tolist") else list(box)
        if len(points) != 4:
            raise RuntimeError("RapidOCR returned a non-quadrilateral box.")
        quad: list[list[float]] = []
        for point in points:
            if len(point) != 2:
                raise RuntimeError("RapidOCR returned an invalid point.")
            x = float(point[0]) + offset_x
            y = float(point[1]) + offset_y
            if not math.isfinite(x) or not math.isfinite(y):
                raise RuntimeError("RapidOCR returned a non-finite coordinate.")
            quad.append([
                min(max(x, 0.0), float(image_width)),
                min(max(y, 0.0), float(image_height)),
            ])
        block = {"quad": quad, "text": text, "score": score}
        left, top, right, bottom = quad_bounds(block)
        if right - left >= 0.25 and bottom - top >= 0.25:
            blocks.append(block)
    return blocks


def recognize_tiles(engine: object, image: object, tile_size: int) -> tuple[list[dict], int]:
    image_height, image_width = image.shape[:2]
    tiles = make_tiles(image_width, image_height, tile_size, TILE_OVERLAP)
    blocks: list[dict] = []
    for tile in tiles:
        x, y, width, height = tile[:4]
        tile_image = image[y : y + height, x : x + width]
        result = engine(tile_image)
        tile_blocks = extract_result_blocks(result, x, y, image_width, image_height)
        if len(tiles) > 1:
            tile_blocks = [
                block
                for block in tile_blocks
                if block_owned_by_tile(block, tile)
            ]
        blocks.extend(tile_blocks)
        if len(blocks) > MAX_BLOCKS * 2:
            raise RuntimeError("RapidOCR produced too many intermediate blocks.")
    return deduplicate_blocks(blocks), len(tiles)


def elapsed_ms(start: float, end: float) -> float:
    return round(max(0.0, (end - start) * 1000.0), 3)


def make_protocol(
    profile: str,
    source_width: int,
    source_height: int,
    input_width: int,
    input_height: int,
    scale_x: float,
    scale_y: float,
    preprocess_mode: str,
    tile_count: int,
    tile_size: int,
    timings: dict,
    blocks: list[dict],
) -> dict:
    tiled = tile_count > 1
    return {
        "schemaVersion": SCHEMA_VERSION,
        "profile": profile,
        "preprocess": {
            "sourceWidth": source_width,
            "sourceHeight": source_height,
            "inputWidth": input_width,
            "inputHeight": input_height,
            "scaleX": scale_x,
            "scaleY": scale_y,
            "resample": preprocess_mode,
            "tiled": tiled,
            "tileCount": tile_count,
            "tileSize": tile_size if tiled else 0,
            "tileOverlap": TILE_OVERLAP if tiled else 0,
            "coordinateSpace": "input-pixels",
        },
        "timings": timings,
        "blocks": blocks,
    }


def reject_duplicate_json_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Worker request contains duplicate JSON fields.")
        result[key] = value
    return result


def validate_image_metadata(metadata: dict) -> dict:
    expected_keys = {
        "path",
        "sourceWidth",
        "sourceHeight",
        "inputWidth",
        "inputHeight",
        "scaleX",
        "scaleY",
        "resample",
    }
    if type(metadata) is not dict or set(metadata) != expected_keys:
        raise ValueError("Worker image metadata fields are invalid.")
    dimensions = (
        metadata["sourceWidth"],
        metadata["sourceHeight"],
        metadata["inputWidth"],
        metadata["inputHeight"],
    )
    if any(type(value) is not int or value < 1 or value > 100_000 for value in dimensions):
        raise ValueError("Worker image dimensions are invalid.")
    if type(metadata["path"]) is not str or not metadata["path"] or len(metadata["path"]) > 32_767:
        raise ValueError("Worker image path is invalid.")
    scale_x = metadata["scaleX"]
    scale_y = metadata["scaleY"]
    if (
        isinstance(scale_x, bool)
        or not isinstance(scale_x, (int, float))
        or isinstance(scale_y, bool)
        or not isinstance(scale_y, (int, float))
        or not math.isfinite(scale_x)
        or not math.isfinite(scale_y)
        or scale_x < 0.01
        or scale_x > 2.0
        or scale_y < 0.01
        or scale_y > 2.0
        or not math.isclose(
            scale_x,
            metadata["inputWidth"] / metadata["sourceWidth"],
            rel_tol=0.0,
            abs_tol=1e-9,
        )
        or not math.isclose(
            scale_y,
            metadata["inputHeight"] / metadata["sourceHeight"],
            rel_tol=0.0,
            abs_tol=1e-9,
        )
    ):
        raise ValueError("Worker image scale metadata is invalid.")
    if metadata["resample"] not in {
        "none",
        "bilinear-upscale",
        "progressive-bilinear-downscale",
    }:
        raise ValueError("Worker image resample metadata is invalid.")
    return metadata


def validate_worker_request(
    payload: bytes,
    profile: str,
    dependency_root: str,
    dependency_sequence: int,
) -> dict:
    if not payload or len(payload) > MAX_WORKER_REQUEST_BYTES:
        raise ValueError("Worker request exceeds the frame limit.")
    request = json.loads(
        payload.decode("utf-8", errors="strict"),
        object_pairs_hook=reject_duplicate_json_keys,
        parse_constant=lambda value: (_ for _ in ()).throw(
            ValueError(f"Invalid JSON constant: {value}")
        ),
    )
    expected_keys = {
        "schemaVersion",
        "requestId",
        "profile",
        "dependencyRoot",
        "dependencySequence",
        "image",
    }
    if type(request) is not dict or set(request) != expected_keys:
        raise ValueError("Worker request fields are invalid.")
    if request["schemaVersion"] != WORKER_SCHEMA_VERSION:
        raise ValueError("Worker request schema version is unsupported.")
    if (
        type(request["requestId"]) is not int
        or request["requestId"] < 1
        or request["requestId"] > MAX_SAFE_JSON_INTEGER
    ):
        raise ValueError("Worker request id is invalid.")
    if (
        request["profile"] != profile
        or request["dependencyRoot"] != dependency_root
        or request["dependencySequence"] != dependency_sequence
    ):
        raise ValueError("Worker request identity does not match this worker.")
    request["image"] = validate_image_metadata(request["image"])
    return request


def make_worker_response(
    request: dict,
    dependency_root: str,
    dependency_sequence: int,
    result: dict | None = None,
    error: str | None = None,
) -> dict:
    ok = result is not None and error is None
    response = {
        "schemaVersion": WORKER_SCHEMA_VERSION,
        "requestId": request["requestId"],
        "profile": request["profile"],
        "dependencyRoot": dependency_root,
        "dependencySequence": dependency_sequence,
        "ok": ok,
    }
    if ok:
        response["result"] = result
    else:
        response["error"] = clean_text(error or "Warm OCR worker failed.")[:4096]
    return response


def encode_worker_frame(value: dict, maximum_bytes: int = MAX_WORKER_RESPONSE_BYTES) -> bytes:
    payload = json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
    ).encode("utf-8", errors="strict")
    if not payload or len(payload) > maximum_bytes:
        raise ValueError("Worker frame payload exceeds its limit.")
    return struct.pack("<I", len(payload)) + payload


def read_exact(stream, size: int) -> bytes | None:
    result = bytearray()
    while len(result) < size:
        chunk = stream.read(size - len(result))
        if not chunk:
            if not result:
                return None
            raise EOFError("Worker input pipe ended inside a frame.")
        result.extend(chunk)
    return bytes(result)


def worker_frame_reader(stream, messages: queue.Queue) -> None:
    try:
        while True:
            header = read_exact(stream, 4)
            if header is None:
                messages.put(None)
                return
            payload_size = struct.unpack("<I", header)[0]
            if payload_size < 1 or payload_size > MAX_WORKER_REQUEST_BYTES:
                raise ValueError("Worker request frame length is invalid.")
            payload = read_exact(stream, payload_size)
            if payload is None:
                raise EOFError("Worker input pipe ended before the frame payload.")
            messages.put(payload)
    except Exception as exc:
        messages.put(exc)


def validate_input_image(image_path: Path) -> None:
    if (
        not is_regular_non_reparse_file(image_path)
        or image_path.suffix.lower() != ".png"
        or image_path.stat().st_size > MAX_IMAGE_BYTES
    ):
        raise ValueError("OCR input must be a regular PNG file no larger than 128 MiB.")


def run_image_request(
    engine: object,
    profile: str,
    metadata: dict,
    cv2: object,
    np: object,
    model_init_ms: float,
) -> dict:
    total_start = time.perf_counter()
    image_path = Path(metadata["path"])
    validate_input_image(image_path)
    png_width, png_height = read_png_dimensions(image_path)
    if png_width != metadata["inputWidth"] or png_height != metadata["inputHeight"]:
        raise ValueError("OCR PNG dimensions do not match preprocessing metadata.")

    decode_start = time.perf_counter()
    encoded = image_path.read_bytes()
    image = cv2.imdecode(np.frombuffer(encoded, dtype=np.uint8), cv2.IMREAD_COLOR)
    decode_end = time.perf_counter()
    if image is None or image.ndim != 3 or image.shape[2] != 3:
        raise ValueError("OCR PNG could not be decoded as a color image.")
    if image.shape[1] != png_width or image.shape[0] != png_height:
        raise ValueError("OCR decoded dimensions changed unexpectedly.")

    inference_start = time.perf_counter()
    tile_size = PROFILE_SETTINGS[profile]["max_side_len"]
    blocks, tile_count = recognize_tiles(engine, image, tile_size)
    inference_end = time.perf_counter()

    merge_start = time.perf_counter()
    blocks = deduplicate_blocks(blocks)
    if len(blocks) > MAX_BLOCKS:
        raise RuntimeError("RapidOCR produced too many text blocks.")
    merge_end = time.perf_counter()
    total_end = time.perf_counter()
    request_total_ms = elapsed_ms(total_start, total_end)
    return make_protocol(
        profile,
        metadata["sourceWidth"],
        metadata["sourceHeight"],
        metadata["inputWidth"],
        metadata["inputHeight"],
        float(metadata["scaleX"]),
        float(metadata["scaleY"]),
        metadata["resample"],
        tile_count,
        tile_size,
        {
            "decodeMs": elapsed_ms(decode_start, decode_end),
            "modelInitMs": round(model_init_ms, 3),
            "inferenceMs": elapsed_ms(inference_start, inference_end),
            "mergeMs": elapsed_ms(merge_start, merge_end),
            "totalMs": round(request_total_ms + model_init_ms, 3),
        },
        blocks,
    )


def run_worker_server(
    profile: str,
    dependency_root: str,
    dependency_sequence: int,
    engine: object,
    cv2: object,
    np: object,
    model_init_ms: float,
    input_stream=None,
    output_stream=None,
    idle_timeout_seconds: float = WORKER_IDLE_TIMEOUT_SECONDS,
) -> int:
    if input_stream is None:
        input_stream = sys.stdin.buffer
    if output_stream is None:
        output_stream = sys.stdout.buffer
    messages: queue.Queue = queue.Queue(maxsize=2)
    reader = threading.Thread(
        target=worker_frame_reader,
        args=(input_stream, messages),
        daemon=True,
        name="airshot-ocr-frame-reader",
    )
    reader.start()
    first_request = True
    while True:
        try:
            message = messages.get(timeout=idle_timeout_seconds)
        except queue.Empty:
            return 0
        if message is None:
            return 0
        if isinstance(message, Exception):
            print(str(message), file=sys.stderr)
            return 2
        try:
            request = validate_worker_request(
                message,
                profile,
                dependency_root,
                dependency_sequence,
            )
        except Exception as exc:
            print(str(exc), file=sys.stderr)
            return 2

        try:
            with redirect_process_stdout_to_stderr():
                result = run_image_request(
                    engine,
                    profile,
                    request["image"],
                    cv2,
                    np,
                    model_init_ms if first_request else 0.0,
                )
            response = make_worker_response(
                request,
                dependency_root,
                dependency_sequence,
                result=result,
            )
        except Exception as exc:
            response = make_worker_response(
                request,
                dependency_root,
                dependency_sequence,
                error=str(exc),
            )
        first_request = False
        frame = encode_worker_frame(response)
        output_stream.write(frame)
        output_stream.flush()


def parse_arguments():
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--server", action="store_true")
    parser.add_argument("--image")
    parser.add_argument("--model-dir", required=True)
    parser.add_argument(
        "--ocr-profile",
        choices=sorted(SUPPORTED_PROFILES),
        default="rapidocr-v5-fast",
    )
    parser.add_argument("--ort-threads", type=int, required=True)
    parser.add_argument("--source-width", type=int)
    parser.add_argument("--source-height", type=int)
    parser.add_argument("--input-width", type=int)
    parser.add_argument("--input-height", type=int)
    parser.add_argument("--scale-x", type=float)
    parser.add_argument("--scale-y", type=float)
    parser.add_argument(
        "--preprocess-mode",
        choices=("none", "bilinear-upscale", "progressive-bilinear-downscale"),
    )
    parser.add_argument("--dependency-root")
    parser.add_argument("--dependency-sequence", type=int)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    sys.stdout.reconfigure(encoding="utf-8", errors="strict", newline="\n")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace", newline="\n")

    model_dir = Path(args.model_dir)
    if args.ort_threads < 1 or args.ort_threads > 4:
        print("OCR thread count must be in 1..4.", file=sys.stderr)
        return 2
    if not is_non_reparse_directory(model_dir):
        print("OCR model directory is missing or is a reparse point.", file=sys.stderr)
        return 2
    if any(not is_regular_non_reparse_file(model_dir / name) for name in REQUIRED_MODEL_FILES):
        print("OCR model directory is incomplete or contains a reparse point.", file=sys.stderr)
        return 2

    os.environ["OMP_NUM_THREADS"] = str(args.ort_threads)
    os.environ["OMP_WAIT_POLICY"] = "PASSIVE"
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["NO_PROXY"] = "*"
    os.environ["OC_DISABLE_DOT_ACCESS_WARNING"] = "1"

    try:
        with redirect_process_stdout_to_stderr():
            import cv2
            import numpy as np
            from rapidocr import RapidOCR

            model_start = time.perf_counter()
            engine = RapidOCR(
                params=build_params(args.ocr_profile, model_dir, args.ort_threads)
            )
            model_end = time.perf_counter()
        model_init_ms = elapsed_ms(model_start, model_end)

        if args.server:
            if (
                type(args.dependency_root) is not str
                or not args.dependency_root
                or type(args.dependency_sequence) is not int
                or args.dependency_sequence < 1
                or args.dependency_sequence > MAX_SAFE_JSON_INTEGER
                or not is_non_reparse_directory(Path(args.dependency_root))
            ):
                raise ValueError("Warm worker dependency identity is invalid.")
            return run_worker_server(
                args.ocr_profile,
                args.dependency_root,
                args.dependency_sequence,
                engine,
                cv2,
                np,
                model_init_ms,
            )

        metadata = validate_image_metadata(
            {
                "path": args.image,
                "sourceWidth": args.source_width,
                "sourceHeight": args.source_height,
                "inputWidth": args.input_width,
                "inputHeight": args.input_height,
                "scaleX": args.scale_x,
                "scaleY": args.scale_y,
                "resample": args.preprocess_mode,
            }
        )
        with redirect_process_stdout_to_stderr():
            protocol = run_image_request(
                engine,
                args.ocr_profile,
                metadata,
                cv2,
                np,
                model_init_ms,
            )
        json.dump(protocol, sys.stdout, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
        return 0
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
