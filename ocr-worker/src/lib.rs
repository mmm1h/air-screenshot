use serde::Serialize;
use serde::de::{self, Deserialize, Deserializer, MapAccess, Visitor};
use std::fmt;
use std::time::Duration;

pub const OCR_SCHEMA_VERSION: u32 = 1;
pub const WORKER_SCHEMA_VERSION: u32 = 1;
pub const MAX_IMAGE_BYTES: u64 = 128 * 1024 * 1024;
pub const MAX_DECODED_PIXELS: u64 = 40 * 1024 * 1024;
pub const MAX_BLOCKS: usize = 16_384;
pub const TILE_OVERLAP: u32 = 128;
pub const MAX_TILES: usize = 512;
pub const MAX_WORKER_REQUEST_BYTES: usize = 64 * 1024;
pub const MAX_WORKER_RESPONSE_BYTES: usize = 8 * 1024 * 1024;
pub const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;

pub const SUPPORTED_PROFILES: [&str; 3] = [
    "rapidocr-v5-fast",
    "rapidocr-v5-accurate",
    "rapidocr-v4-compat",
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SessionThreadPlan {
    pub intra_threads: usize,
    pub inter_threads: usize,
}

pub fn session_thread_plan(requested_threads: usize) -> SessionThreadPlan {
    SessionThreadPlan {
        intra_threads: requested_threads.clamp(1, 4),
        // Detection, classification and recognition sessions are invoked
        // serially, so a second inter-op pool only oversubscribes the CPU.
        inter_threads: 1,
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ProfileSettings {
    pub tile_size: u32,
    pub detection_limit: u32,
    pub padding: u32,
    pub box_score_threshold: f32,
    pub box_threshold: f32,
    pub unclip_ratio: f32,
    pub text_score: f32,
    pub initial_angle_detection: bool,
    pub angle_rollback_threshold: f32,
    pub retry_low_confidence: bool,
    pub retry_angle_detection: bool,
    pub retry_score_threshold: f64,
    pub retry_minimum_score_gain: f64,
    pub retry_contrast: f32,
}

pub fn profile_settings(profile: &str) -> Option<ProfileSettings> {
    match profile {
        "rapidocr-v5-fast" => Some(ProfileSettings {
            // Keep the tile at or below the detector limit. Otherwise
            // paddle-ocr-rs silently downsizes an image that the host has
            // already enlarged for small screen text.
            tile_size: 1536,
            detection_limit: 1536,
            padding: 50,
            box_score_threshold: 0.50,
            box_threshold: 0.30,
            unclip_ratio: 1.60,
            text_score: 0.50,
            initial_angle_detection: false,
            angle_rollback_threshold: 0.90,
            retry_low_confidence: false,
            retry_angle_detection: false,
            retry_score_threshold: 0.0,
            retry_minimum_score_gain: 0.0,
            retry_contrast: 0.0,
        }),
        "rapidocr-v5-accurate" => Some(ProfileSettings {
            tile_size: 2048,
            detection_limit: 2048,
            padding: 64,
            // The accurate profile deliberately favors recall. These values
            // are isolated here so a checked-in OCR corpus can calibrate them
            // without changing the inference pipeline.
            box_score_threshold: 0.45,
            box_threshold: 0.25,
            unclip_ratio: 1.70,
            text_score: 0.45,
            initial_angle_detection: false,
            angle_rollback_threshold: 0.90,
            retry_low_confidence: true,
            retry_angle_detection: true,
            retry_score_threshold: 0.80,
            retry_minimum_score_gain: 0.03,
            retry_contrast: 18.0,
        }),
        "rapidocr-v4-compat" => Some(ProfileSettings {
            tile_size: 1536,
            detection_limit: 1536,
            padding: 50,
            box_score_threshold: 0.50,
            box_threshold: 0.30,
            unclip_ratio: 1.60,
            text_score: 0.50,
            // Preserve the previous always-on classifier behavior only in the
            // compatibility profile. Fast and accurate keep the common
            // horizontal-screen-text path free of classifier inference.
            initial_angle_detection: true,
            angle_rollback_threshold: 0.80,
            retry_low_confidence: false,
            retry_angle_detection: false,
            retry_score_threshold: 0.0,
            retry_minimum_score_gain: 0.0,
            retry_contrast: 0.0,
        }),
        _ => None,
    }
}

pub fn mean_block_score(blocks: &[OcrBlock]) -> Option<f64> {
    if blocks.is_empty() {
        return None;
    }
    Some(blocks.iter().map(|block| block.score).sum::<f64>() / blocks.len() as f64)
}

pub fn should_retry_recognition(blocks: &[OcrBlock], settings: ProfileSettings) -> bool {
    settings.retry_low_confidence
        && mean_block_score(blocks).is_none_or(|score| score < settings.retry_score_threshold)
}

pub fn retry_result_is_better(
    primary: &[OcrBlock],
    retry: &[OcrBlock],
    settings: ProfileSettings,
) -> bool {
    let Some(retry_score) = mean_block_score(retry) else {
        return false;
    };
    let Some(primary_score) = mean_block_score(primary) else {
        return retry_score >= f64::from(settings.text_score);
    };

    // Do not trade most of the recognized content for a marginal confidence
    // increase. Character coverage and the minimum gain are intentionally
    // simple, deterministic knobs for later corpus calibration.
    let primary_characters = primary
        .iter()
        .map(|block| block.text.chars().count())
        .sum::<usize>();
    let retry_characters = retry
        .iter()
        .map(|block| block.text.chars().count())
        .sum::<usize>();
    retry_characters.saturating_mul(4) >= primary_characters.saturating_mul(3)
        && retry_score >= primary_score + settings.retry_minimum_score_gain
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct OcrBlock {
    pub quad: [[f64; 2]; 4],
    pub text: String,
    pub score: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Tile {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
    pub owner_left: f64,
    pub owner_top: f64,
    pub owner_right: f64,
    pub owner_bottom: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ImageMetadata {
    pub path: String,
    pub source_width: u32,
    pub source_height: u32,
    pub input_width: u32,
    pub input_height: u32,
    pub scale_x: f64,
    pub scale_y: f64,
    pub resample: String,
}

#[derive(Clone, Debug, PartialEq)]
pub struct WorkerRequest {
    pub schema_version: u32,
    pub request_id: u64,
    pub profile: String,
    pub dependency_root: String,
    pub dependency_sequence: u64,
    pub image: ImageMetadata,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PreprocessMetadata {
    pub source_width: u32,
    pub source_height: u32,
    pub input_width: u32,
    pub input_height: u32,
    pub scale_x: f64,
    pub scale_y: f64,
    pub resample: String,
    pub tiled: bool,
    pub tile_count: usize,
    pub tile_size: u32,
    pub tile_overlap: u32,
    pub coordinate_space: &'static str,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OcrTimings {
    pub decode_ms: f64,
    pub model_init_ms: f64,
    pub inference_ms: f64,
    pub merge_ms: f64,
    pub total_ms: f64,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OcrProtocol {
    pub schema_version: u32,
    pub profile: String,
    pub preprocess: PreprocessMetadata,
    pub timings: OcrTimings,
    pub blocks: Vec<OcrBlock>,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct WorkerResponse {
    pub schema_version: u32,
    pub request_id: u64,
    pub profile: String,
    pub dependency_root: String,
    pub dependency_sequence: u64,
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub result: Option<OcrProtocol>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

pub fn clean_text(value: &str) -> String {
    let sanitized: String = value
        .chars()
        .map(|character| {
            if (character as u32) < 32 || character == '\u{7f}' {
                ' '
            } else {
                character
            }
        })
        .collect();
    sanitized.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn normalized_text(text: &str) -> String {
    text.chars()
        .filter(|character| !character.is_whitespace())
        .flat_map(char::to_lowercase)
        .collect()
}

pub fn quad_bounds(block: &OcrBlock) -> (f64, f64, f64, f64) {
    let mut left = f64::INFINITY;
    let mut top = f64::INFINITY;
    let mut right = f64::NEG_INFINITY;
    let mut bottom = f64::NEG_INFINITY;
    for [x, y] in block.quad {
        left = left.min(x);
        top = top.min(y);
        right = right.max(x);
        bottom = bottom.max(y);
    }
    (left, top, right, bottom)
}

pub fn duplicate_blocks(first: &OcrBlock, second: &OcrBlock) -> bool {
    if normalized_text(&first.text) != normalized_text(&second.text) {
        return false;
    }
    let (first_left, first_top, first_right, first_bottom) = quad_bounds(first);
    let (second_left, second_top, second_right, second_bottom) = quad_bounds(second);
    let intersection_width = (first_right.min(second_right) - first_left.max(second_left)).max(0.0);
    let intersection_height =
        (first_bottom.min(second_bottom) - first_top.max(second_top)).max(0.0);
    let intersection = intersection_width * intersection_height;
    let first_area = (first_right - first_left).max(0.0) * (first_bottom - first_top).max(0.0);
    let second_area = (second_right - second_left).max(0.0) * (second_bottom - second_top).max(0.0);
    if first_area.min(second_area) <= 0.0 {
        return false;
    }
    let union = first_area + second_area - intersection;
    let iou = if union > 0.0 {
        intersection / union
    } else {
        0.0
    };
    let overlap_of_smaller = intersection / first_area.min(second_area);
    iou >= 0.35 || overlap_of_smaller >= 0.65
}

pub fn deduplicate_blocks(blocks: Vec<OcrBlock>) -> Vec<OcrBlock> {
    let mut retained: Vec<OcrBlock> = Vec::new();
    for candidate in blocks {
        if let Some(index) = retained
            .iter()
            .position(|current| duplicate_blocks(current, &candidate))
        {
            if candidate.score > retained[index].score {
                retained[index] = candidate;
            }
        } else {
            retained.push(candidate);
        }
    }
    retained
}

pub fn tile_starts(length: u32, tile_size: u32, overlap: u32) -> Result<Vec<u32>, String> {
    if tile_size == 0 || tile_size <= overlap {
        return Err("OCR tile geometry is invalid.".to_string());
    }
    if length <= tile_size {
        return Ok(vec![0]);
    }
    let step = tile_size - overlap;
    let mut starts = Vec::new();
    let final_start = length - tile_size;
    let mut start = 0_u32;
    loop {
        starts.push(start);
        if final_start - start < step {
            break;
        }
        start += step;
    }
    if starts.last().copied() != Some(final_start) {
        starts.push(final_start);
    }
    Ok(starts)
}

fn axis_tiles(
    length: u32,
    tile_size: u32,
    overlap: u32,
) -> Result<Vec<(u32, u32, f64, f64)>, String> {
    let starts = tile_starts(length, tile_size, overlap)?;
    let sizes: Vec<u32> = starts
        .iter()
        .map(|start| tile_size.min(length - *start))
        .collect();
    let boundaries: Vec<f64> = (0..starts.len().saturating_sub(1))
        .map(|index| (f64::from(starts[index] + sizes[index]) + f64::from(starts[index + 1])) * 0.5)
        .collect();
    Ok(starts
        .iter()
        .enumerate()
        .map(|(index, start)| {
            (
                *start,
                sizes[index],
                if index == 0 {
                    0.0
                } else {
                    boundaries[index - 1]
                },
                if index + 1 == starts.len() {
                    f64::from(length)
                } else {
                    boundaries[index]
                },
            )
        })
        .collect())
}

pub fn make_tiles(
    width: u32,
    height: u32,
    tile_size: u32,
    overlap: u32,
) -> Result<Vec<Tile>, String> {
    let x_tiles = axis_tiles(width, tile_size, overlap)?;
    let y_tiles = axis_tiles(height, tile_size, overlap)?;
    let tile_count = x_tiles
        .len()
        .checked_mul(y_tiles.len())
        .ok_or_else(|| "OCR image requires too many tiles.".to_string())?;
    if tile_count > MAX_TILES {
        return Err("OCR image requires too many tiles.".to_string());
    }
    let mut result = Vec::with_capacity(tile_count);
    for (y, height, owner_top, owner_bottom) in y_tiles {
        for &(x, width, owner_left, owner_right) in &x_tiles {
            result.push(Tile {
                x,
                y,
                width,
                height,
                owner_left,
                owner_top,
                owner_right,
                owner_bottom,
            });
        }
    }
    Ok(result)
}

pub fn block_owned_by_tile(block: &OcrBlock, tile: &Tile) -> bool {
    let (left, top, right, bottom) = quad_bounds(block);
    let center_x = (left + right) * 0.5;
    let center_y = (top + bottom) * 0.5;
    tile.owner_left <= center_x
        && center_x < tile.owner_right
        && tile.owner_top <= center_y
        && center_y < tile.owner_bottom
}

pub fn read_png_dimensions(header: &[u8]) -> Result<(u32, u32), String> {
    const SIGNATURE: &[u8; 8] = b"\x89PNG\r\n\x1a\n";
    if header.len() < 24
        || &header[..8] != SIGNATURE
        || &header[12..16] != b"IHDR"
        || u32::from_be_bytes(header[8..12].try_into().expect("four-byte PNG length")) != 13
    {
        return Err("OCR input is not a canonical PNG image.".to_string());
    }
    let width = u32::from_be_bytes(header[16..20].try_into().expect("four-byte PNG width"));
    let height = u32::from_be_bytes(header[20..24].try_into().expect("four-byte PNG height"));
    if width == 0 || height == 0 || width > 100_000 || height > 100_000 {
        return Err("OCR PNG dimensions are outside the supported range.".to_string());
    }
    if u64::from(width) * u64::from(height) > MAX_DECODED_PIXELS {
        return Err("OCR PNG decoded pixel count exceeds the safety limit.".to_string());
    }
    Ok((width, height))
}

pub fn validate_image_metadata(metadata: &ImageMetadata) -> Result<(), String> {
    let dimensions = [
        metadata.source_width,
        metadata.source_height,
        metadata.input_width,
        metadata.input_height,
    ];
    if dimensions
        .iter()
        .any(|dimension| *dimension < 1 || *dimension > 100_000)
    {
        return Err("Worker image dimensions are invalid.".to_string());
    }
    if metadata.path.is_empty() || metadata.path.chars().count() > 32_767 {
        return Err("Worker image path is invalid.".to_string());
    }
    let expected_scale_x = f64::from(metadata.input_width) / f64::from(metadata.source_width);
    let expected_scale_y = f64::from(metadata.input_height) / f64::from(metadata.source_height);
    if !metadata.scale_x.is_finite()
        || !metadata.scale_y.is_finite()
        || !(0.01..=2.0).contains(&metadata.scale_x)
        || !(0.01..=2.0).contains(&metadata.scale_y)
        || (metadata.scale_x - expected_scale_x).abs() > 1e-9
        || (metadata.scale_y - expected_scale_y).abs() > 1e-9
    {
        return Err("Worker image scale metadata is invalid.".to_string());
    }
    if !matches!(
        metadata.resample.as_str(),
        "none" | "bilinear-upscale" | "progressive-bilinear-downscale"
    ) {
        return Err("Worker image resample metadata is invalid.".to_string());
    }
    Ok(())
}

pub fn parse_worker_request(
    payload: &[u8],
    profile: &str,
    dependency_root: &str,
    dependency_sequence: u64,
) -> Result<WorkerRequest, String> {
    if payload.is_empty() || payload.len() > MAX_WORKER_REQUEST_BYTES {
        return Err("Worker request exceeds the frame limit.".to_string());
    }
    let mut deserializer = serde_json::Deserializer::from_slice(payload);
    let request = WorkerRequest::deserialize(&mut deserializer)
        .map_err(|error| format!("Worker request JSON is invalid: {error}"))?;
    deserializer
        .end()
        .map_err(|error| format!("Worker request JSON is invalid: {error}"))?;
    if request.schema_version != WORKER_SCHEMA_VERSION {
        return Err("Worker request schema version is unsupported.".to_string());
    }
    if request.request_id < 1 || request.request_id > MAX_SAFE_JSON_INTEGER {
        return Err("Worker request id is invalid.".to_string());
    }
    if request.profile != profile
        || request.dependency_root != dependency_root
        || request.dependency_sequence != dependency_sequence
    {
        return Err("Worker request identity does not match this worker.".to_string());
    }
    validate_image_metadata(&request.image)?;
    Ok(request)
}

pub fn make_worker_response(
    request: &WorkerRequest,
    dependency_root: &str,
    dependency_sequence: u64,
    result: Result<OcrProtocol, String>,
) -> WorkerResponse {
    match result {
        Ok(protocol) => WorkerResponse {
            schema_version: WORKER_SCHEMA_VERSION,
            request_id: request.request_id,
            profile: request.profile.clone(),
            dependency_root: dependency_root.to_string(),
            dependency_sequence,
            ok: true,
            result: Some(protocol),
            error: None,
        },
        Err(error) => WorkerResponse {
            schema_version: WORKER_SCHEMA_VERSION,
            request_id: request.request_id,
            profile: request.profile.clone(),
            dependency_root: dependency_root.to_string(),
            dependency_sequence,
            ok: false,
            result: None,
            error: Some(clean_text(&error).chars().take(4096).collect()),
        },
    }
}

pub fn encode_worker_frame<T: Serialize>(
    value: &T,
    maximum_bytes: usize,
) -> Result<Vec<u8>, String> {
    let payload = serde_json::to_vec(value)
        .map_err(|error| format!("Unable to encode worker response: {error}"))?;
    if payload.is_empty() || payload.len() > maximum_bytes {
        return Err("Worker frame payload exceeds its limit.".to_string());
    }
    let payload_length = u32::try_from(payload.len())
        .map_err(|_| "Worker frame payload exceeds its limit.".to_string())?;
    let mut frame = Vec::with_capacity(4 + payload.len());
    frame.extend_from_slice(&payload_length.to_le_bytes());
    frame.extend_from_slice(&payload);
    Ok(frame)
}

pub fn round_milliseconds(duration: Duration) -> f64 {
    (duration.as_secs_f64() * 1_000_000.0).round() / 1_000.0
}

struct ImageMetadataVisitor;

impl<'de> Visitor<'de> for ImageMetadataVisitor {
    type Value = ImageMetadata;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("an exact worker image metadata object")
    }

    fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
    where
        M: MapAccess<'de>,
    {
        let mut path = None;
        let mut source_width = None;
        let mut source_height = None;
        let mut input_width = None;
        let mut input_height = None;
        let mut scale_x = None;
        let mut scale_y = None;
        let mut resample = None;
        while let Some(key) = map.next_key::<String>()? {
            match key.as_str() {
                "path" => set_once(&mut path, map.next_value()?, "path")?,
                "sourceWidth" => set_once(&mut source_width, map.next_value()?, "sourceWidth")?,
                "sourceHeight" => set_once(&mut source_height, map.next_value()?, "sourceHeight")?,
                "inputWidth" => set_once(&mut input_width, map.next_value()?, "inputWidth")?,
                "inputHeight" => set_once(&mut input_height, map.next_value()?, "inputHeight")?,
                "scaleX" => set_once(&mut scale_x, map.next_value()?, "scaleX")?,
                "scaleY" => set_once(&mut scale_y, map.next_value()?, "scaleY")?,
                "resample" => set_once(&mut resample, map.next_value()?, "resample")?,
                _ => return Err(de::Error::unknown_field(&key, IMAGE_FIELDS)),
            }
        }
        Ok(ImageMetadata {
            path: path.ok_or_else(|| de::Error::missing_field("path"))?,
            source_width: source_width.ok_or_else(|| de::Error::missing_field("sourceWidth"))?,
            source_height: source_height.ok_or_else(|| de::Error::missing_field("sourceHeight"))?,
            input_width: input_width.ok_or_else(|| de::Error::missing_field("inputWidth"))?,
            input_height: input_height.ok_or_else(|| de::Error::missing_field("inputHeight"))?,
            scale_x: scale_x.ok_or_else(|| de::Error::missing_field("scaleX"))?,
            scale_y: scale_y.ok_or_else(|| de::Error::missing_field("scaleY"))?,
            resample: resample.ok_or_else(|| de::Error::missing_field("resample"))?,
        })
    }
}

impl<'de> Deserialize<'de> for ImageMetadata {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        deserializer.deserialize_map(ImageMetadataVisitor)
    }
}

const IMAGE_FIELDS: &[&str] = &[
    "path",
    "sourceWidth",
    "sourceHeight",
    "inputWidth",
    "inputHeight",
    "scaleX",
    "scaleY",
    "resample",
];

struct WorkerRequestVisitor;

impl<'de> Visitor<'de> for WorkerRequestVisitor {
    type Value = WorkerRequest;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("an exact warm OCR worker request object")
    }

    fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
    where
        M: MapAccess<'de>,
    {
        let mut schema_version = None;
        let mut request_id = None;
        let mut profile = None;
        let mut dependency_root = None;
        let mut dependency_sequence = None;
        let mut image = None;
        while let Some(key) = map.next_key::<String>()? {
            match key.as_str() {
                "schemaVersion" => {
                    set_once(&mut schema_version, map.next_value()?, "schemaVersion")?
                }
                "requestId" => set_once(&mut request_id, map.next_value()?, "requestId")?,
                "profile" => set_once(&mut profile, map.next_value()?, "profile")?,
                "dependencyRoot" => {
                    set_once(&mut dependency_root, map.next_value()?, "dependencyRoot")?
                }
                "dependencySequence" => set_once(
                    &mut dependency_sequence,
                    map.next_value()?,
                    "dependencySequence",
                )?,
                "image" => set_once(&mut image, map.next_value()?, "image")?,
                _ => return Err(de::Error::unknown_field(&key, WORKER_FIELDS)),
            }
        }
        Ok(WorkerRequest {
            schema_version: schema_version
                .ok_or_else(|| de::Error::missing_field("schemaVersion"))?,
            request_id: request_id.ok_or_else(|| de::Error::missing_field("requestId"))?,
            profile: profile.ok_or_else(|| de::Error::missing_field("profile"))?,
            dependency_root: dependency_root
                .ok_or_else(|| de::Error::missing_field("dependencyRoot"))?,
            dependency_sequence: dependency_sequence
                .ok_or_else(|| de::Error::missing_field("dependencySequence"))?,
            image: image.ok_or_else(|| de::Error::missing_field("image"))?,
        })
    }
}

impl<'de> Deserialize<'de> for WorkerRequest {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        deserializer.deserialize_map(WorkerRequestVisitor)
    }
}

const WORKER_FIELDS: &[&str] = &[
    "schemaVersion",
    "requestId",
    "profile",
    "dependencyRoot",
    "dependencySequence",
    "image",
];

fn set_once<T, E>(slot: &mut Option<T>, value: T, field: &'static str) -> Result<(), E>
where
    E: de::Error,
{
    if slot.is_some() {
        return Err(E::duplicate_field(field));
    }
    *slot = Some(value);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn block(left: f64, top: f64, right: f64, bottom: f64, text: &str, score: f64) -> OcrBlock {
        OcrBlock {
            quad: [[left, top], [right, top], [right, bottom], [left, bottom]],
            text: text.to_string(),
            score,
        }
    }

    fn request_json(extra: &str) -> Vec<u8> {
        format!(
            "{{\"schemaVersion\":1,\"requestId\":7,\"profile\":\"rapidocr-v5-fast\",\"dependencyRoot\":\"C:\\\\ocr\",\"dependencySequence\":3,\"image\":{{\"path\":\"C:\\\\image.png\",\"sourceWidth\":100,\"sourceHeight\":50,\"inputWidth\":100,\"inputHeight\":50,\"scaleX\":1,\"scaleY\":1,\"resample\":\"none\"}}{extra}}}"
        )
        .into_bytes()
    }

    #[test]
    fn tiles_cover_the_complete_axis_and_use_single_owners() {
        let tiles = make_tiles(4097, 2300, 2048, 128).expect("tiles");
        assert_eq!(tiles.len(), 6);
        assert_eq!(tiles.first().expect("first").x, 0);
        assert_eq!(
            tiles.last().expect("last").x + tiles.last().expect("last").width,
            4097
        );

        for y in [0.0, 1150.0, 2299.0] {
            for x in [0.0, 1919.0, 1920.0, 3000.0, 4096.0] {
                let candidate = block(x, y, x + 0.1, y + 0.1, "x", 1.0);
                assert_eq!(
                    tiles
                        .iter()
                        .filter(|tile| block_owned_by_tile(&candidate, tile))
                        .count(),
                    1
                );
            }
        }
    }

    #[test]
    fn dedupe_keeps_the_best_scoring_overlapping_text() {
        let result = deduplicate_blocks(vec![
            block(0.0, 0.0, 100.0, 30.0, "Air OCR", 0.7),
            block(2.0, 1.0, 102.0, 31.0, "airocr", 0.9),
            block(200.0, 0.0, 260.0, 30.0, "Air OCR", 0.8),
        ]);
        assert_eq!(result.len(), 2);
        assert_eq!(result[0].score, 0.9);
    }

    #[test]
    fn profile_limits_match_the_host_contract() {
        assert_eq!(
            profile_settings("rapidocr-v5-fast")
                .expect("fast")
                .tile_size,
            1536
        );
        for profile in SUPPORTED_PROFILES {
            let settings = profile_settings(profile).expect("supported profile");
            assert_eq!(settings.tile_size, settings.detection_limit);
            assert!(settings.tile_size > TILE_OVERLAP);
            assert!(settings.detection_limit <= 2048);
        }
        assert!(profile_settings("unknown").is_none());
    }

    #[test]
    fn session_thread_plan_avoids_inter_op_oversubscription() {
        assert_eq!(
            session_thread_plan(0),
            SessionThreadPlan {
                intra_threads: 1,
                inter_threads: 1,
            }
        );
        assert_eq!(session_thread_plan(3).intra_threads, 3);
        assert_eq!(session_thread_plan(32).intra_threads, 4);
        assert_eq!(session_thread_plan(32).inter_threads, 1);
    }

    #[test]
    fn profiles_keep_angle_and_retry_work_off_the_fast_path() {
        let fast = profile_settings("rapidocr-v5-fast").expect("fast");
        let accurate = profile_settings("rapidocr-v5-accurate").expect("accurate");
        let compat = profile_settings("rapidocr-v4-compat").expect("compat");

        assert!(!fast.initial_angle_detection);
        assert!(!fast.retry_low_confidence);
        assert!(accurate.retry_low_confidence);
        assert!(accurate.retry_angle_detection);
        assert!(accurate.box_score_threshold < fast.box_score_threshold);
        assert!(accurate.box_threshold < fast.box_threshold);
        assert!(compat.initial_angle_detection);
        assert!(!compat.retry_low_confidence);
    }

    #[test]
    fn accurate_retry_is_bounded_by_confidence_and_character_coverage() {
        let accurate = profile_settings("rapidocr-v5-accurate").expect("accurate");
        let fast = profile_settings("rapidocr-v5-fast").expect("fast");
        let weak = vec![block(0.0, 0.0, 100.0, 20.0, "abcdefgh", 0.60)];
        let strong = vec![block(0.0, 0.0, 100.0, 20.0, "abcdefgh", 0.90)];
        let truncated = vec![block(0.0, 0.0, 40.0, 20.0, "abc", 0.95)];

        assert!(should_retry_recognition(&[], accurate));
        assert!(should_retry_recognition(&weak, accurate));
        assert!(!should_retry_recognition(&strong, accurate));
        assert!(!should_retry_recognition(&weak, fast));
        assert!(retry_result_is_better(&[], &strong, accurate));
        assert!(retry_result_is_better(&weak, &strong, accurate));
        assert!(!retry_result_is_better(&weak, &truncated, accurate));
        assert!(!retry_result_is_better(&strong, &weak, accurate));
    }

    #[test]
    fn request_parser_is_strict_and_preserves_identity() {
        let request = parse_worker_request(&request_json(""), "rapidocr-v5-fast", "C:\\ocr", 3)
            .expect("valid request");
        assert_eq!(request.request_id, 7);
        assert_eq!(request.image.input_width, 100);

        let duplicate = request_json(",\"requestId\":8");
        assert!(parse_worker_request(&duplicate, "rapidocr-v5-fast", "C:\\ocr", 3).is_err());
        assert!(
            parse_worker_request(&request_json(""), "rapidocr-v4-compat", "C:\\ocr", 3).is_err()
        );
    }

    #[test]
    fn request_parser_rejects_nested_duplicates_unknown_fields_and_bad_scales() {
        let duplicate_image = String::from_utf8(request_json("")).expect("UTF-8").replace(
            "\"resample\":\"none\"",
            "\"resample\":\"none\",\"scaleX\":1",
        );
        assert!(
            parse_worker_request(duplicate_image.as_bytes(), "rapidocr-v5-fast", "C:\\ocr", 3,)
                .is_err()
        );

        let unknown = String::from_utf8(request_json(""))
            .expect("UTF-8")
            .replace("\"requestId\":7", "\"requestId\":7,\"unexpected\":true");
        assert!(
            parse_worker_request(unknown.as_bytes(), "rapidocr-v5-fast", "C:\\ocr", 3).is_err()
        );

        let bad_scale = String::from_utf8(request_json(""))
            .expect("UTF-8")
            .replace("\"scaleX\":1", "\"scaleX\":0.5");
        assert!(
            parse_worker_request(bad_scale.as_bytes(), "rapidocr-v5-fast", "C:\\ocr", 3,).is_err()
        );
    }

    #[test]
    fn tile_limits_fail_closed() {
        assert_eq!(tile_starts(10, 10, 2).expect("one tile"), vec![0]);
        assert!(tile_starts(100, 128, 128).is_err());
        assert!(make_tiles(100_000, 100_000, 256, 128).is_err());
    }

    #[test]
    fn worker_error_is_sanitized_and_bounded() {
        let request = parse_worker_request(&request_json(""), "rapidocr-v5-fast", "C:\\ocr", 3)
            .expect("request");
        let raw_error = format!("line one\n{}", "x".repeat(5_000));
        let response = make_worker_response(&request, "C:\\ocr", 3, Err(raw_error));
        let error = response.error.expect("error response");
        assert!(!error.contains('\n'));
        assert_eq!(error.chars().count(), 4_096);
        assert!(!response.ok);
        assert!(response.result.is_none());
    }

    #[test]
    fn frame_is_little_endian_and_bounded() {
        let value = serde_json::json!({"ok": true});
        let frame = encode_worker_frame(&value, 1024).expect("frame");
        assert_eq!(
            u32::from_le_bytes(frame[..4].try_into().expect("header")) as usize,
            frame.len() - 4
        );
        assert!(encode_worker_frame(&value, 1).is_err());
    }

    #[test]
    fn png_header_is_canonical_and_pixel_limited() {
        let mut header = Vec::from(*b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR");
        header.extend_from_slice(&100_u32.to_be_bytes());
        header.extend_from_slice(&50_u32.to_be_bytes());
        assert_eq!(read_png_dimensions(&header).expect("dimensions"), (100, 50));
        header[12] = b'X';
        assert!(read_png_dimensions(&header).is_err());
    }
}
