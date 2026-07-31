import importlib.util
import io
from pathlib import Path
import struct
import sys
import time
import types
import unittest
from unittest import mock


RUNNER_PATH = Path(__file__).resolve().parents[1] / "scripts" / "rapidocr_runner.py"
SPEC = importlib.util.spec_from_file_location("airshot_rapidocr_runner", RUNNER_PATH)
runner = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(runner)


def block(left, top, right, bottom, text, score):
    return {
        "quad": [[left, top], [right, top], [right, bottom], [left, bottom]],
        "text": text,
        "score": score,
    }


class RunnerGeometryTests(unittest.TestCase):
    def test_long_image_tiles_cover_without_downscaling(self):
        tiles = runner.make_tiles(1440, 10_000, 3072, 128)
        self.assertEqual(len(tiles), 4)
        self.assertEqual(tiles[0][:4], (0, 0, 1440, 3072))
        self.assertEqual(tiles[-1][:4], (0, 6928, 1440, 3072))
        for first, second in zip(tiles, tiles[1:]):
            self.assertLessEqual(second[1], first[1] + first[3])
            self.assertGreaterEqual(first[1] + first[3] - second[1], 128)
            self.assertEqual(first[7], second[5])

    def test_tile_ownership_assigns_overlap_center_once(self):
        first, second = runner.make_tiles(1000, 1872, 1000, 128)
        candidate = block(100, 920, 500, 960, "overlap", 0.9)
        first_owns = runner.block_owned_by_tile(candidate, first)
        second_owns = runner.block_owned_by_tile(candidate, second)
        self.assertNotEqual(first_owns, second_owns)

    def test_quad_text_dedup_keeps_higher_confidence(self):
        low = block(10, 10, 110, 40, "Air OCR", 0.6)
        high = block(12, 11, 112, 41, "Air OCR", 0.9)
        unrelated = block(10, 50, 110, 80, "next", 0.8)
        result = runner.deduplicate_blocks([low, high, unrelated])
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0]["score"], 0.9)
        self.assertEqual(result[1]["text"], "next")

    def test_profile_parameters_are_explicit_and_bounded(self):
        for profile, settings in runner.PROFILE_SETTINGS.items():
            self.assertIn(profile, runner.SUPPORTED_PROFILES)
            self.assertEqual(settings["text_score"], 0.50)
            self.assertGreaterEqual(settings["det_limit_side_len"], 1536)
            self.assertLessEqual(settings["max_side_len"], 3072)
        self.assertGreater(
            runner.PROFILE_SETTINGS["rapidocr-v5-accurate"]["max_side_len"],
            runner.PROFILE_SETTINGS["rapidocr-v5-fast"]["max_side_len"],
        )

    def test_rapidocr_params_use_only_omegaconf_safe_primitive_paths(self):
        fake_rapidocr = types.SimpleNamespace(
            EngineType=types.SimpleNamespace(ONNXRUNTIME="onnxruntime"),
            OCRVersion=types.SimpleNamespace(PPOCRV4="v4", PPOCRV5="v5"),
        )
        model_dir = Path("C:/airshot/models/rapidocr-v5-fast")
        with mock.patch.dict(sys.modules, {"rapidocr": fake_rapidocr}):
            params = runner.build_params("rapidocr-v5-fast", model_dir, 2)
        self.assertEqual(params["Global.model_root_dir"], str(model_dir))
        self.assertEqual(params["Det.model_path"], str(model_dir / "det.onnx"))
        self.assertFalse(any(isinstance(value, Path) for value in params.values()))

    def test_protocol_schema_is_exact_and_json_safe(self):
        timings = {
            "decodeMs": 1.0,
            "modelInitMs": 2.0,
            "inferenceMs": 3.0,
            "mergeMs": 0.1,
            "totalMs": 7.0,
        }
        protocol = runner.make_protocol(
            "rapidocr-v5-fast",
            100,
            50,
            200,
            100,
            2.0,
            2.0,
            "bilinear-upscale",
            2,
            2048,
            timings,
            [block(0, 0, 20, 10, "中文", 0.9)],
        )
        self.assertEqual(
            set(protocol),
            {"schemaVersion", "profile", "preprocess", "timings", "blocks"},
        )
        self.assertEqual(
            set(protocol["preprocess"]),
            {
                "sourceWidth",
                "sourceHeight",
                "inputWidth",
                "inputHeight",
                "scaleX",
                "scaleY",
                "resample",
                "tiled",
                "tileCount",
                "tileSize",
                "tileOverlap",
                "coordinateSpace",
            },
        )
        encoded = runner.json.dumps(protocol, ensure_ascii=False, allow_nan=False)
        self.assertIn("中文", encoded)


class WarmWorkerProtocolTests(unittest.TestCase):
    def request(self, request_id=1, profile="rapidocr-v5-fast", root="C:\\ocr", sequence=9):
        return {
            "schemaVersion": runner.WORKER_SCHEMA_VERSION,
            "requestId": request_id,
            "profile": profile,
            "dependencyRoot": root,
            "dependencySequence": sequence,
            "image": {
                "path": f"C:\\temp\\{request_id}.png",
                "sourceWidth": 100,
                "sourceHeight": 50,
                "inputWidth": 200,
                "inputHeight": 100,
                "scaleX": 2.0,
                "scaleY": 2.0,
                "resample": "bilinear-upscale",
            },
        }

    def payload(self, request):
        return runner.json.dumps(request, separators=(",", ":")).encode("utf-8")

    def decode_frames(self, encoded):
        frames = []
        cursor = 0
        while cursor < len(encoded):
            size = struct.unpack("<I", encoded[cursor : cursor + 4])[0]
            cursor += 4
            frames.append(runner.json.loads(encoded[cursor : cursor + size].decode("utf-8")))
            cursor += size
        return frames

    def test_request_identity_and_duplicate_fields_are_strict(self):
        valid = self.payload(self.request())
        parsed = runner.validate_worker_request(valid, "rapidocr-v5-fast", "C:\\ocr", 9)
        self.assertEqual(parsed["requestId"], 1)
        for profile, root, sequence in [
            ("rapidocr-v5-accurate", "C:\\ocr", 9),
            ("rapidocr-v5-fast", "C:\\other", 9),
            ("rapidocr-v5-fast", "C:\\ocr", 10),
        ]:
            with self.assertRaises(ValueError):
                runner.validate_worker_request(valid, profile, root, sequence)
        duplicate = valid[:-1] + b',"requestId":1}'
        with self.assertRaises(ValueError):
            runner.validate_worker_request(duplicate, "rapidocr-v5-fast", "C:\\ocr", 9)

    def test_frame_reader_rejects_truncated_and_oversized_frames(self):
        messages = runner.queue.Queue()
        runner.worker_frame_reader(io.BytesIO(struct.pack("<I", 10) + b"{}"), messages)
        self.assertIsInstance(messages.get_nowait(), EOFError)
        messages = runner.queue.Queue()
        runner.worker_frame_reader(
            io.BytesIO(struct.pack("<I", runner.MAX_WORKER_REQUEST_BYTES + 1)),
            messages,
        )
        self.assertIsInstance(messages.get_nowait(), ValueError)

    def test_server_reuses_one_engine_and_reports_init_once(self):
        requests = [self.request(1), self.request(2)]
        input_bytes = b"".join(
            runner.encode_worker_frame(request, runner.MAX_WORKER_REQUEST_BYTES)
            for request in requests
        )
        output = io.BytesIO()
        engine = object()
        calls = []

        def fake_run_image_request(current_engine, profile, metadata, cv2, np, model_init_ms):
            calls.append((current_engine, profile, metadata["path"], model_init_ms))
            return runner.make_protocol(
                profile,
                100,
                50,
                200,
                100,
                2.0,
                2.0,
                "bilinear-upscale",
                1,
                2048,
                {
                    "decodeMs": 0.0,
                    "modelInitMs": model_init_ms,
                    "inferenceMs": 0.0,
                    "mergeMs": 0.0,
                    "totalMs": model_init_ms,
                },
                [],
            )

        with mock.patch.object(runner, "run_image_request", side_effect=fake_run_image_request):
            result = runner.run_worker_server(
                "rapidocr-v5-fast",
                "C:\\ocr",
                9,
                engine,
                object(),
                object(),
                12.5,
                input_stream=io.BytesIO(input_bytes),
                output_stream=output,
                idle_timeout_seconds=1.0,
            )
        self.assertEqual(result, 0)
        self.assertEqual(len(calls), 2)
        self.assertIs(calls[0][0], engine)
        self.assertIs(calls[1][0], engine)
        self.assertEqual(calls[0][3], 12.5)
        self.assertEqual(calls[1][3], 0.0)
        responses = self.decode_frames(output.getvalue())
        self.assertEqual([response["requestId"] for response in responses], [1, 2])
        self.assertTrue(all(response["ok"] for response in responses))

    def test_server_exits_on_idle_timeout_and_pipe_break(self):
        class BlockingStream:
            def read(self, size):
                time.sleep(0.1)
                return b""

        started = time.perf_counter()
        result = runner.run_worker_server(
            "rapidocr-v5-fast",
            "C:\\ocr",
            9,
            object(),
            object(),
            object(),
            0.0,
            input_stream=BlockingStream(),
            output_stream=io.BytesIO(),
            idle_timeout_seconds=0.01,
        )
        self.assertEqual(result, 0)
        self.assertLess(time.perf_counter() - started, 0.08)
        self.assertEqual(
            runner.run_worker_server(
                "rapidocr-v5-fast",
                "C:\\ocr",
                9,
                object(),
                object(),
                object(),
                0.0,
                input_stream=io.BytesIO(b""),
                output_stream=io.BytesIO(),
                idle_timeout_seconds=1.0,
            ),
            0,
        )


if __name__ == "__main__":
    unittest.main()
