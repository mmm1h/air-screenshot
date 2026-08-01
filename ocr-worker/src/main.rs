use air_screenshot_ocr_worker::{
    ImageMetadata, MAX_BLOCKS, MAX_IMAGE_BYTES, MAX_SAFE_JSON_INTEGER, MAX_WORKER_REQUEST_BYTES,
    MAX_WORKER_RESPONSE_BYTES, OcrBlock, OcrProtocol, OcrTimings, PreprocessMetadata, TILE_OVERLAP,
    block_owned_by_tile, clean_text, deduplicate_blocks, encode_worker_frame, make_tiles,
    make_worker_response, parse_worker_request, profile_settings, read_png_dimensions,
    round_milliseconds, validate_image_metadata,
};
use image::RgbImage;
use paddle_ocr_rs::ocr_lite::OcrLite;
use std::collections::HashSet;
use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, SyncSender};
use std::thread;
use std::time::{Duration, Instant};
use windows_sys::Win32::Storage::FileSystem::{
    FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_REPARSE_POINT, GetFileAttributesW,
    INVALID_FILE_ATTRIBUTES,
};

const REQUIRED_MODEL_FILES: [&str; 4] = ["det.onnx", "rec.onnx", "cls.onnx", "dict.txt"];
const WORKER_IDLE_TIMEOUT: Duration = Duration::from_secs(60);

#[derive(Debug)]
struct Arguments {
    server: bool,
    model_dir: String,
    profile: String,
    ort_threads: usize,
    image: Option<String>,
    source_width: Option<u32>,
    source_height: Option<u32>,
    input_width: Option<u32>,
    input_height: Option<u32>,
    scale_x: Option<f64>,
    scale_y: Option<f64>,
    preprocess_mode: Option<String>,
    dependency_root: Option<String>,
    dependency_sequence: Option<u64>,
}

enum ParsedArguments {
    Run(Box<Arguments>),
    Help,
}

struct NativeOcrEngine {
    ocr: OcrLite,
    profile: String,
}

impl NativeOcrEngine {
    fn load(model_dir: &Path, profile: &str, thread_count: usize) -> Result<Self, String> {
        let det = path_as_utf8(&model_dir.join("det.onnx"))?;
        let cls = path_as_utf8(&model_dir.join("cls.onnx"))?;
        let rec = path_as_utf8(&model_dir.join("rec.onnx"))?;
        let mut ocr = OcrLite::new();
        // RapidOCR's published ONNX recognition models carry their character
        // table as model metadata. Using it avoids maintaining a second,
        // potentially mismatched decoder implementation in this worker.
        ocr.init_models(&det, &cls, &rec, thread_count)
            .map_err(|error| format!("Unable to initialize native OCR models: {error:?}"))?;
        Ok(Self {
            ocr,
            profile: profile.to_string(),
        })
    }

    fn recognize_tile(
        &mut self,
        image: &RgbImage,
        offset_x: u32,
        offset_y: u32,
        image_width: u32,
        image_height: u32,
    ) -> Result<Vec<OcrBlock>, String> {
        let settings = profile_settings(&self.profile)
            .ok_or_else(|| "OCR profile is unsupported.".to_string())?;
        let result = self
            .ocr
            .detect_angle_rollback(
                image,
                50,
                settings.detection_limit,
                0.50,
                0.30,
                1.60,
                true,
                false,
                0.80,
            )
            .map_err(|error| format!("Native OCR inference failed: {error:?}"))?;

        let mut blocks = Vec::with_capacity(result.text_blocks.len());
        for text_block in result.text_blocks {
            let text = clean_text(&text_block.text);
            let score = f64::from(text_block.text_score);
            if text.is_empty()
                || !score.is_finite()
                || score < f64::from(settings.text_score)
                || score > 1.0
            {
                continue;
            }
            if text_block.box_points.len() != 4 {
                return Err("Native OCR returned a non-quadrilateral box.".to_string());
            }
            let mut quad = [[0.0_f64; 2]; 4];
            for (index, point) in text_block.box_points.iter().enumerate() {
                quad[index] = [
                    f64::from(point.x.saturating_add(offset_x)).min(f64::from(image_width)),
                    f64::from(point.y.saturating_add(offset_y)).min(f64::from(image_height)),
                ];
            }
            let block = OcrBlock { quad, text, score };
            let (left, top, right, bottom) = air_screenshot_ocr_worker::quad_bounds(&block);
            if right - left >= 0.25 && bottom - top >= 0.25 {
                blocks.push(block);
            }
        }
        Ok(blocks)
    }

    fn recognize_tiles(&mut self, image: &RgbImage) -> Result<(Vec<OcrBlock>, usize, u32), String> {
        let settings = profile_settings(&self.profile)
            .ok_or_else(|| "OCR profile is unsupported.".to_string())?;
        let tiles = make_tiles(
            image.width(),
            image.height(),
            settings.tile_size,
            TILE_OVERLAP,
        )?;
        let tile_count = tiles.len();
        let mut blocks = Vec::new();
        for tile in &tiles {
            let tile_image =
                image::imageops::crop_imm(image, tile.x, tile.y, tile.width, tile.height)
                    .to_image();
            let mut tile_blocks =
                self.recognize_tile(&tile_image, tile.x, tile.y, image.width(), image.height())?;
            if tile_count > 1 {
                tile_blocks.retain(|block| block_owned_by_tile(block, tile));
            }
            blocks.extend(tile_blocks);
            if blocks.len() > MAX_BLOCKS * 2 {
                return Err("Native OCR produced too many intermediate blocks.".to_string());
            }
        }
        Ok((deduplicate_blocks(blocks), tile_count, settings.tile_size))
    }
}

fn path_as_utf8(path: &Path) -> Result<String, String> {
    path.to_str()
        .map(ToOwned::to_owned)
        .ok_or_else(|| "OCR dependency path is not valid Unicode.".to_string())
}

fn path_attributes(path: &Path) -> Option<u32> {
    let mut wide: Vec<u16> = path.as_os_str().encode_wide().collect();
    wide.push(0);
    // SAFETY: `wide` is a valid, nul-terminated UTF-16 buffer for the duration
    // of this call and GetFileAttributesW does not retain the pointer.
    let attributes = unsafe { GetFileAttributesW(wide.as_ptr()) };
    (attributes != INVALID_FILE_ATTRIBUTES).then_some(attributes)
}

fn is_non_reparse_directory(path: &Path) -> bool {
    path_attributes(path).is_some_and(|attributes| {
        attributes & FILE_ATTRIBUTE_DIRECTORY != 0 && attributes & FILE_ATTRIBUTE_REPARSE_POINT == 0
    })
}

fn is_regular_non_reparse_file(path: &Path) -> bool {
    path_attributes(path).is_some_and(|attributes| {
        attributes & FILE_ATTRIBUTE_DIRECTORY == 0 && attributes & FILE_ATTRIBUTE_REPARSE_POINT == 0
    })
}

fn validate_model_directory(model_dir: &Path) -> Result<(), String> {
    if !is_non_reparse_directory(model_dir) {
        return Err("OCR model directory is missing or is a reparse point.".to_string());
    }
    if REQUIRED_MODEL_FILES
        .iter()
        .any(|name| !is_regular_non_reparse_file(&model_dir.join(name)))
    {
        return Err("OCR model directory is incomplete or contains a reparse point.".to_string());
    }
    Ok(())
}

fn initialize_onnx_runtime() -> Result<(), String> {
    let executable = std::env::current_exe()
        .map_err(|error| format!("Unable to locate the native OCR worker: {error}"))?;
    let runtime_path = executable
        .parent()
        .ok_or_else(|| "Unable to locate the native OCR runtime directory.".to_string())?
        .join("onnxruntime.dll");
    if !is_regular_non_reparse_file(&runtime_path) {
        return Err("Native OCR runtime is missing or is a reparse point.".to_string());
    }
    ort::init_from(path_as_utf8(&runtime_path)?)
        .commit()
        .map_err(|error| format!("Unable to initialize ONNX Runtime: {error}"))?;
    Ok(())
}

fn validate_input_image(path: &Path) -> Result<(u32, u32), String> {
    if !is_regular_non_reparse_file(path)
        || path
            .extension()
            .and_then(OsStr::to_str)
            .is_none_or(|extension| !extension.eq_ignore_ascii_case("png"))
    {
        return Err("OCR input must be a regular PNG file no larger than 128 MiB.".to_string());
    }
    let metadata = fs::metadata(path)
        .map_err(|_| "OCR input must be a regular PNG file no larger than 128 MiB.".to_string())?;
    if metadata.len() > MAX_IMAGE_BYTES {
        return Err("OCR input must be a regular PNG file no larger than 128 MiB.".to_string());
    }
    let mut header = [0_u8; 24];
    File::open(path)
        .and_then(|mut file| file.read_exact(&mut header))
        .map_err(|_| "OCR input is not a canonical PNG image.".to_string())?;
    read_png_dimensions(&header)
}

fn run_image_request(
    engine: &mut NativeOcrEngine,
    metadata: &ImageMetadata,
    model_init_ms: f64,
) -> Result<OcrProtocol, String> {
    let total_start = Instant::now();
    let image_path = PathBuf::from(&metadata.path);
    let (png_width, png_height) = validate_input_image(&image_path)?;
    if png_width != metadata.input_width || png_height != metadata.input_height {
        return Err("OCR PNG dimensions do not match preprocessing metadata.".to_string());
    }

    let decode_start = Instant::now();
    let encoded =
        fs::read(&image_path).map_err(|error| format!("Unable to read OCR PNG: {error}"))?;
    let image = image::load_from_memory_with_format(&encoded, image::ImageFormat::Png)
        .map_err(|error| format!("OCR PNG could not be decoded as a color image: {error}"))?
        .to_rgb8();
    let decode_ms = round_milliseconds(decode_start.elapsed());
    if image.width() != png_width || image.height() != png_height {
        return Err("OCR decoded dimensions changed unexpectedly.".to_string());
    }

    let inference_start = Instant::now();
    let (blocks, tile_count, tile_size) = engine.recognize_tiles(&image)?;
    let inference_ms = round_milliseconds(inference_start.elapsed());

    let merge_start = Instant::now();
    let blocks = deduplicate_blocks(blocks);
    if blocks.len() > MAX_BLOCKS {
        return Err("Native OCR produced too many text blocks.".to_string());
    }
    let merge_ms = round_milliseconds(merge_start.elapsed());
    let request_total_ms = round_milliseconds(total_start.elapsed());
    let tiled = tile_count > 1;
    Ok(OcrProtocol {
        schema_version: 1,
        profile: engine.profile.clone(),
        preprocess: PreprocessMetadata {
            source_width: metadata.source_width,
            source_height: metadata.source_height,
            input_width: metadata.input_width,
            input_height: metadata.input_height,
            scale_x: metadata.scale_x,
            scale_y: metadata.scale_y,
            resample: metadata.resample.clone(),
            tiled,
            tile_count,
            tile_size: if tiled { tile_size } else { 0 },
            tile_overlap: if tiled { TILE_OVERLAP } else { 0 },
            coordinate_space: "input-pixels",
        },
        timings: OcrTimings {
            decode_ms,
            model_init_ms,
            inference_ms,
            merge_ms,
            total_ms: ((request_total_ms + model_init_ms) * 1_000.0).round() / 1_000.0,
        },
        blocks,
    })
}

enum ReaderMessage {
    Frame(Vec<u8>),
    Eof,
    Error(String),
}

fn read_exact_or_eof<R: Read>(reader: &mut R, output: &mut [u8]) -> io::Result<bool> {
    let mut offset = 0;
    while offset < output.len() {
        match reader.read(&mut output[offset..]) {
            Ok(0) if offset == 0 => return Ok(false),
            Ok(0) => {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "Worker input pipe ended inside a frame.",
                ));
            }
            Ok(count) => offset += count,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
    Ok(true)
}

fn worker_frame_reader_from<R: Read>(mut input: R, sender: SyncSender<ReaderMessage>) {
    loop {
        let mut header = [0_u8; 4];
        match read_exact_or_eof(&mut input, &mut header) {
            Ok(false) => {
                let _ = sender.send(ReaderMessage::Eof);
                return;
            }
            Ok(true) => {}
            Err(error) => {
                let _ = sender.send(ReaderMessage::Error(error.to_string()));
                return;
            }
        }
        let payload_size = u32::from_le_bytes(header) as usize;
        if payload_size == 0 || payload_size > MAX_WORKER_REQUEST_BYTES {
            let _ = sender.send(ReaderMessage::Error(
                "Worker request frame length is invalid.".to_string(),
            ));
            return;
        }
        let mut payload = vec![0_u8; payload_size];
        match read_exact_or_eof(&mut input, &mut payload) {
            Ok(true) => {
                if sender.send(ReaderMessage::Frame(payload)).is_err() {
                    return;
                }
            }
            Ok(false) => {
                let _ = sender.send(ReaderMessage::Error(
                    "Worker input pipe ended before the frame payload.".to_string(),
                ));
                return;
            }
            Err(error) => {
                let _ = sender.send(ReaderMessage::Error(error.to_string()));
                return;
            }
        }
    }
}

fn worker_frame_reader(sender: SyncSender<ReaderMessage>) {
    let stdin = io::stdin();
    worker_frame_reader_from(stdin.lock(), sender);
}

fn receive_frame_with_timeout(
    receiver: &Receiver<ReaderMessage>,
    timeout: Duration,
) -> Result<Option<Vec<u8>>, String> {
    match receiver.recv_timeout(timeout) {
        Ok(ReaderMessage::Frame(payload)) => Ok(Some(payload)),
        Ok(ReaderMessage::Eof) | Err(RecvTimeoutError::Timeout) => Ok(None),
        Ok(ReaderMessage::Error(error)) => Err(error),
        Err(RecvTimeoutError::Disconnected) => Ok(None),
    }
}

fn receive_frame(receiver: &Receiver<ReaderMessage>) -> Result<Option<Vec<u8>>, String> {
    receive_frame_with_timeout(receiver, WORKER_IDLE_TIMEOUT)
}

fn run_worker_server(
    engine: &mut NativeOcrEngine,
    profile: &str,
    dependency_root: &str,
    dependency_sequence: u64,
    model_init_ms: f64,
) -> Result<(), String> {
    let (sender, receiver) = mpsc::sync_channel(2);
    thread::Builder::new()
        .name("airshot-ocr-frame-reader".to_string())
        .spawn(move || worker_frame_reader(sender))
        .map_err(|error| format!("Unable to start OCR frame reader: {error}"))?;
    let stdout = io::stdout();
    let mut output = stdout.lock();
    let mut first_request = true;
    while let Some(payload) = receive_frame(&receiver)? {
        let request =
            parse_worker_request(&payload, profile, dependency_root, dependency_sequence)?;
        let result = run_image_request(
            engine,
            &request.image,
            if first_request { model_init_ms } else { 0.0 },
        );
        first_request = false;
        let response = make_worker_response(&request, dependency_root, dependency_sequence, result);
        let frame = encode_worker_frame(&response, MAX_WORKER_RESPONSE_BYTES)?;
        output
            .write_all(&frame)
            .and_then(|_| output.flush())
            .map_err(|error| format!("Unable to write OCR worker response: {error}"))?;
    }
    Ok(())
}

fn parse_value<T: std::str::FromStr>(name: &str, value: &str) -> Result<T, String> {
    value
        .parse::<T>()
        .map_err(|_| format!("Invalid value for {name}."))
}

fn parse_arguments() -> Result<ParsedArguments, String> {
    let mut iterator = std::env::args().skip(1);
    let mut seen = HashSet::new();
    let mut server = false;
    let mut model_dir = None;
    let mut profile = None;
    let mut ort_threads = None;
    let mut image = None;
    let mut source_width = None;
    let mut source_height = None;
    let mut input_width = None;
    let mut input_height = None;
    let mut scale_x = None;
    let mut scale_y = None;
    let mut preprocess_mode = None;
    let mut dependency_root = None;
    let mut dependency_sequence = None;

    while let Some(argument) = iterator.next() {
        if argument == "--help" || argument == "-h" {
            return Ok(ParsedArguments::Help);
        }
        if !argument.starts_with("--") {
            return Err(format!("Unexpected OCR worker argument: {argument}"));
        }
        if !seen.insert(argument.clone()) {
            return Err(format!("Duplicate OCR worker argument: {argument}"));
        }
        if argument == "--server" {
            server = true;
            continue;
        }
        let value = iterator
            .next()
            .ok_or_else(|| format!("Missing value for {argument}."))?;
        match argument.as_str() {
            "--model-dir" => model_dir = Some(value),
            "--ocr-profile" => profile = Some(value),
            "--ort-threads" => ort_threads = Some(parse_value(&argument, &value)?),
            "--image" => image = Some(value),
            "--source-width" => source_width = Some(parse_value(&argument, &value)?),
            "--source-height" => source_height = Some(parse_value(&argument, &value)?),
            "--input-width" => input_width = Some(parse_value(&argument, &value)?),
            "--input-height" => input_height = Some(parse_value(&argument, &value)?),
            "--scale-x" => scale_x = Some(parse_value(&argument, &value)?),
            "--scale-y" => scale_y = Some(parse_value(&argument, &value)?),
            "--preprocess-mode" => preprocess_mode = Some(value),
            "--dependency-root" => dependency_root = Some(value),
            "--dependency-sequence" => dependency_sequence = Some(parse_value(&argument, &value)?),
            _ => return Err(format!("Unknown OCR worker argument: {argument}")),
        }
    }

    let profile = profile.unwrap_or_else(|| "rapidocr-v5-fast".to_string());
    if profile_settings(&profile).is_none() {
        return Err("OCR profile is unsupported.".to_string());
    }
    let ort_threads = ort_threads.ok_or_else(|| "--ort-threads is required.".to_string())?;
    if !(1..=4).contains(&ort_threads) {
        return Err("OCR thread count must be in 1..4.".to_string());
    }
    Ok(ParsedArguments::Run(Box::new(Arguments {
        server,
        model_dir: model_dir.ok_or_else(|| "--model-dir is required.".to_string())?,
        profile,
        ort_threads,
        image,
        source_width,
        source_height,
        input_width,
        input_height,
        scale_x,
        scale_y,
        preprocess_mode,
        dependency_root,
        dependency_sequence,
    })))
}

fn one_shot_metadata(arguments: &Arguments) -> Result<ImageMetadata, String> {
    let metadata = ImageMetadata {
        path: arguments
            .image
            .clone()
            .ok_or_else(|| "--image is required outside server mode.".to_string())?,
        source_width: arguments
            .source_width
            .ok_or_else(|| "--source-width is required outside server mode.".to_string())?,
        source_height: arguments
            .source_height
            .ok_or_else(|| "--source-height is required outside server mode.".to_string())?,
        input_width: arguments
            .input_width
            .ok_or_else(|| "--input-width is required outside server mode.".to_string())?,
        input_height: arguments
            .input_height
            .ok_or_else(|| "--input-height is required outside server mode.".to_string())?,
        scale_x: arguments
            .scale_x
            .ok_or_else(|| "--scale-x is required outside server mode.".to_string())?,
        scale_y: arguments
            .scale_y
            .ok_or_else(|| "--scale-y is required outside server mode.".to_string())?,
        resample: arguments
            .preprocess_mode
            .clone()
            .ok_or_else(|| "--preprocess-mode is required outside server mode.".to_string())?,
    };
    validate_image_metadata(&metadata)?;
    Ok(metadata)
}

fn print_help() {
    println!(
        "Air Screenshot native OCR worker\n\n\
         One shot: rapidocr_runner.exe --image <png> --model-dir <dir> \\\n         --ocr-profile <profile> --ort-threads <1..4> --source-width <n> \\\n         --source-height <n> --input-width <n> --input-height <n> \\\n         --scale-x <n> --scale-y <n> --preprocess-mode <mode>\n\
         Server: rapidocr_runner.exe --server --model-dir <dir> \\\n         --ocr-profile <profile> --ort-threads <1..4> \\\n         --dependency-root <dir> --dependency-sequence <n>"
    );
}

fn configure_runtime(thread_count: usize) {
    // SAFETY: this runs before the frame-reader thread is created and before
    // ONNX Runtime is initialized, so no concurrent environment access exists.
    unsafe {
        std::env::set_var("OMP_NUM_THREADS", thread_count.to_string());
        std::env::set_var("OMP_WAIT_POLICY", "PASSIVE");
        std::env::set_var("HF_HUB_OFFLINE", "1");
        std::env::set_var("NO_PROXY", "*");
    }
}

fn real_main() -> Result<(), String> {
    let arguments = match parse_arguments()? {
        ParsedArguments::Help => {
            print_help();
            return Ok(());
        }
        ParsedArguments::Run(arguments) => arguments,
    };
    let model_dir = PathBuf::from(&arguments.model_dir);
    validate_model_directory(&model_dir)?;
    configure_runtime(arguments.ort_threads);
    initialize_onnx_runtime()?;

    let model_start = Instant::now();
    let mut engine = NativeOcrEngine::load(&model_dir, &arguments.profile, arguments.ort_threads)?;
    let model_init_ms = round_milliseconds(model_start.elapsed());

    if arguments.server {
        let dependency_root = arguments
            .dependency_root
            .as_deref()
            .ok_or_else(|| "Warm worker dependency identity is invalid.".to_string())?;
        let dependency_sequence = arguments
            .dependency_sequence
            .ok_or_else(|| "Warm worker dependency identity is invalid.".to_string())?;
        if dependency_root.is_empty()
            || !(1..=MAX_SAFE_JSON_INTEGER).contains(&dependency_sequence)
            || !is_non_reparse_directory(Path::new(dependency_root))
        {
            return Err("Warm worker dependency identity is invalid.".to_string());
        }
        return run_worker_server(
            &mut engine,
            &arguments.profile,
            dependency_root,
            dependency_sequence,
            model_init_ms,
        );
    }

    let metadata = one_shot_metadata(&arguments)?;
    let protocol = run_image_request(&mut engine, &metadata, model_init_ms)?;
    let stdout = io::stdout();
    let mut output = stdout.lock();
    serde_json::to_writer(&mut output, &protocol)
        .map_err(|error| format!("Unable to encode OCR result: {error}"))?;
    output
        .flush()
        .map_err(|error| format!("Unable to write OCR result: {error}"))?;
    Ok(())
}

fn main() {
    if let Err(error) = real_main() {
        eprintln!("{}", clean_text(&error));
        std::process::exit(2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn read_message(input: Vec<u8>) -> ReaderMessage {
        let (sender, receiver) = mpsc::sync_channel(2);
        worker_frame_reader_from(Cursor::new(input), sender);
        receiver.recv().expect("reader message")
    }

    #[test]
    fn exact_reader_distinguishes_clean_eof_from_truncation() {
        let mut empty = Cursor::new(Vec::<u8>::new());
        let mut output = [0_u8; 4];
        assert!(!read_exact_or_eof(&mut empty, &mut output).expect("clean EOF"));

        let mut truncated = Cursor::new(vec![1_u8, 2]);
        assert_eq!(
            read_exact_or_eof(&mut truncated, &mut output)
                .expect_err("truncated input")
                .kind(),
            io::ErrorKind::UnexpectedEof
        );
    }

    #[test]
    fn frame_reader_accepts_one_frame_then_reports_eof() {
        let payload = br#"{"schemaVersion":1}"#.to_vec();
        let mut framed = (payload.len() as u32).to_le_bytes().to_vec();
        framed.extend_from_slice(&payload);
        let (sender, receiver) = mpsc::sync_channel(2);
        worker_frame_reader_from(Cursor::new(framed), sender);
        match receiver.recv().expect("frame") {
            ReaderMessage::Frame(actual) => assert_eq!(actual, payload),
            _ => panic!("expected a frame"),
        }
        assert!(matches!(receiver.recv().expect("EOF"), ReaderMessage::Eof));
    }

    #[test]
    fn frame_reader_rejects_invalid_lengths_and_truncated_payloads() {
        assert!(matches!(
            read_message(0_u32.to_le_bytes().to_vec()),
            ReaderMessage::Error(_)
        ));
        assert!(matches!(
            read_message(
                ((MAX_WORKER_REQUEST_BYTES + 1) as u32)
                    .to_le_bytes()
                    .to_vec()
            ),
            ReaderMessage::Error(_)
        ));
        let mut truncated = 4_u32.to_le_bytes().to_vec();
        truncated.extend_from_slice(&[1, 2]);
        assert!(matches!(read_message(truncated), ReaderMessage::Error(_)));
    }

    #[test]
    fn idle_receive_returns_without_an_error() {
        let (_sender, receiver) = mpsc::sync_channel(1);
        assert!(
            receive_frame_with_timeout(&receiver, Duration::from_millis(1))
                .expect("idle timeout")
                .is_none()
        );
    }
}
