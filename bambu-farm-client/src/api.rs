use std::collections::HashMap;
use std::collections::hash_map::DefaultHasher;
use std::env::current_dir;
use std::env::var_os;
use std::fs;
use std::hash::{Hash, Hasher};
use std::io::{Cursor, Read};
use std::path::PathBuf;
use std::process::{self, Command};
use std::sync::Mutex;
use std::time::Duration;

use cxx::let_cxx_string;

use config::{Config, ConfigError, Value as ConfigValue};
use futures::StreamExt;
use futures::stream;
use lazy_static::lazy_static;
use log::debug;
use log::warn;
use serde_json::{json, Value as JsonValue};
use suppaftp::native_tls::TlsConnector;
use suppaftp::types::FileType;
use suppaftp::{NativeTlsConnector, NativeTlsFtpStream};
use tokio::spawn;
use tokio::fs::File;
use tokio::io::AsyncReadExt;
use tokio::sync::mpsc;
use tokio::sync::mpsc::Sender;
use tokio::sync::watch;
use tokio::time::sleep;
use tonic::Request;
use url::Url;
use zip::ZipArchive;

use crate::api::bambu_farm::{
    ConnectRequest, ControlMessageRequest, SendMessageRequest, UploadFileChunk,
};
use crate::api::ffi::bambu_network_cb_connected;
use crate::api::ffi::bambu_network_cb_disconnected;
use crate::api::ffi::bambu_network_cb_message_recv;
use crate::errors::{BAMBU_NETWORK_ERR_SEND_MSG_FAILED, BAMBU_NETWORK_SUCCESS};

use self::bambu_farm::bambu_farm_client::BambuFarmClient;
use self::bambu_farm::PrinterOptionRequest;
use self::ffi::bambu_network_cb_printer_available;

const GRPC_MAX_MESSAGE_SIZE: usize = 32 * 1024 * 1024;
const UPLOAD_CHUNK_SIZE: usize = 256 * 1024;

pub mod bambu_farm {
    tonic::include_proto!("_");
}

fn get_config() -> Option<Config> {
    let config_file_path = config_file_candidates()
        .into_iter()
        .find(|path| path.exists())
        .unwrap_or_else(|| PathBuf::from("bambufarm.toml"));

    Config::builder()
        .add_source(config::File::with_name(&config_file_path.to_string_lossy()).required(false))
        .add_source(config::Environment::with_prefix("BAMBU_FARM"))
        .build()
        .ok()
}

fn value_string(table: &config::Map<String, ConfigValue>, key: &str) -> Option<String> {
    table.get(key).map(|value| value.to_string())
}

fn printer_host_for_device(device_id: &str) -> Option<String> {
    let config = get_config()?;
    let printers = config.get_array("printers").ok()?;
    for printer in printers {
        let printer = printer.into_table().ok()?;
        if value_string(&printer, "dev_id").as_deref() == Some(device_id) {
            return value_string(&printer, "host");
        }
    }
    None
}

fn config_file_candidates() -> Vec<PathBuf> {
    let mut paths = Vec::new();

    if let Ok(dir) = current_dir() {
        paths.push(dir.join("bambufarm.toml"));
    }

    if let Some(xdg_config_home) = var_os("XDG_CONFIG_HOME") {
        paths.push(PathBuf::from(xdg_config_home).join("BambuStudio/bambufarm.toml"));
    }

    if let Some(home) = var_os("HOME") {
        paths.push(PathBuf::from(home).join(".config/BambuStudio/bambufarm.toml"));
    }

    paths
}

lazy_static! {
    static ref RUNTIME: tokio::runtime::Runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(6)
        .enable_all()
        .build()
        .unwrap();
}

lazy_static! {
    static ref MSG_TX: Mutex<HashMap<String, Sender<SendMessageRequest>>> =
        Mutex::new(HashMap::new());
}

lazy_static! {
    static ref CURRENT_CONNECTED_PRINTER: Mutex<Option<String>> = Mutex::new(None);
}

lazy_static! {
    static ref ACTIVE_CONNECTION_STATE: Mutex<Option<(String, u64, watch::Sender<bool>)>> =
        Mutex::new(None);
}

#[cxx::bridge]
mod ffi {

    extern "Rust" {
        pub fn bambu_network_rs_init();
        pub fn bambu_network_rs_log_debug(message: String);
        pub fn bambu_network_rs_connect(device_id: String) -> i32;
        pub fn bambu_network_rs_disconnect(device_id: String) -> i32;
        pub fn bambu_network_rs_send(device_id: String, data: String) -> i32;
        pub fn bambu_network_rs_upload_file(
            device_id: String,
            local_filename: String,
            remote_filename: String,
        ) -> i32;
        pub fn bambu_network_rs_tunnel_request(
            tunnel_url: String,
            device_id: String,
            data: String,
        ) -> Vec<u8>;
        pub fn bambu_network_rs_extract_3mf_thumbnail(
            local_filename: String,
            plate_index: i32,
            output_dir: String,
        ) -> String;
    }

    unsafe extern "C++" {
        include!("api.hpp");

        pub fn bambu_network_cb_printer_available(json: &CxxString);
        pub fn bambu_network_cb_message_recv(device_id: &CxxString, json: &CxxString);
        pub fn bambu_network_cb_connected(device_id: &CxxString);
        pub fn bambu_network_cb_disconnected(
            device_id: &CxxString,
            status: i32,
            message: &CxxString,
        );
    }
}

pub fn get_endpoint() -> String {
    let config_file_path = config_file_candidates()
        .into_iter()
        .find(|path| path.exists())
        .unwrap_or_else(|| PathBuf::from("bambufarm.toml"));

    let config = match Config::builder()
        .add_source(config::File::with_name(&config_file_path.to_string_lossy()).required(false))
        .add_source(config::Environment::with_prefix("BAMBU_FARM"))
        .build()
    {
        Ok(config) => config,
        Err(err) => {
            match err {
                ConfigError::NotFound(_) => {
                    warn!(
                        "No config file found. Try adding one at `{}`",
                        config_file_path.to_string_lossy()
                    );
                }
                ConfigError::FileParse { uri, cause } => {
                    if let Some(uri) = uri {
                        warn!("Error parsing config file at {}\nCause:{}", uri, cause);
                    } else {
                        warn!("Error parsing config file.")
                    }
                }
                _ => {
                    warn!("Unknown error parsing config file {:?}", err)
                }
            }
            return "http://127.0.0.1:47403".into();
        }
    };
    config
        .get_string("endpoint")
        .map(|socket_addr| {
            if config.get_bool("use_https").unwrap_or(false) {
                format!("https://{}", socket_addr)
            } else {
                format!("http://{}", socket_addr)
            }
        })
        .unwrap_or("http://127.0.0.1:47403".into())
}

async fn connect_client() -> Result<BambuFarmClient<tonic::transport::Channel>, tonic::transport::Error> {
    BambuFarmClient::connect(get_endpoint()).await.map(|client| {
        client
            .max_decoding_message_size(GRPC_MAX_MESSAGE_SIZE)
            .max_encoding_message_size(GRPC_MAX_MESSAGE_SIZE)
    })
}

fn disconnect_active_connection() {
    let active = ACTIVE_CONNECTION_STATE.lock().unwrap().take();
    if let Some((device_id, _, cancel_tx)) = active {
        let _ = cancel_tx.send(true);
        MSG_TX.lock().unwrap().remove(&device_id);
        let mut current = CURRENT_CONNECTED_PRINTER.lock().unwrap();
        if current.as_ref() == Some(&device_id) {
            current.take();
        }
    }
}

fn clear_active_connection(device_id: &str, generation: u64) {
    let mut active = ACTIVE_CONNECTION_STATE.lock().unwrap();
    if matches!(active.as_ref(), Some((active_device_id, active_generation, _)) if active_device_id == device_id && *active_generation == generation) {
        active.take();
        MSG_TX.lock().unwrap().remove(device_id);
        let mut current = CURRENT_CONNECTED_PRINTER.lock().unwrap();
        if current.as_deref() == Some(device_id) {
            current.take();
        }
    }
}

fn tunnel_url_host(url: &str) -> Option<String> {
    let prefix = "bambu:///local/";
    let suffix = url.strip_prefix(prefix)?;
    let host_part = suffix.split('?').next().unwrap_or_default();
    let trimmed = host_part.trim_matches('/');
    let trimmed = trimmed.trim_end_matches('.');
    if trimmed.is_empty() {
        None
    } else {
        Some(trimmed.to_string())
    }
}

fn tunnel_query_value(url: &str, key: &str) -> Option<String> {
    let query = url.split_once('?')?.1;
    let parsed = Url::parse(&format!("http://dummy/?{}", query)).ok()?;
    parsed
        .query_pairs()
        .find(|(candidate, _)| candidate == key)
        .map(|(_, value)| value.into_owned())
}

fn ftps_connector() -> Result<NativeTlsConnector, String> {
    let mut builder = TlsConnector::builder();
    builder.danger_accept_invalid_certs(true);
    builder.danger_accept_invalid_hostnames(true);
    let connector = builder
        .build()
        .map_err(|err| format!("failed to build TLS connector: {}", err))?;
    Ok(NativeTlsConnector::from(connector))
}

fn finalize_ftps_login(
    mut ftp_stream: NativeTlsFtpStream,
    username: &str,
    password: &str,
) -> Result<NativeTlsFtpStream, String> {
    ftp_stream
        .login(username, password)
        .map_err(|err| format!("failed to login to FTPS server: {}", err))?;
    ftp_stream
        .transfer_type(FileType::Binary)
        .map_err(|err| format!("failed to enable binary transfer mode: {}", err))?;
    Ok(ftp_stream)
}

fn connect_ftps(host: &str, username: &str, password: &str) -> Result<NativeTlsFtpStream, String> {
    match NativeTlsFtpStream::connect_secure_implicit((host, 990), ftps_connector()?, host) {
        Ok(ftp_stream) => return finalize_ftps_login(ftp_stream, username, password),
        Err(implicit_err) => {
            bambu_network_rs_log_debug(format!(
                "bambu_network_rs_tunnel_request: implicit FTPS connect failed host={} error={}; trying explicit FTPS",
                host, implicit_err
            ));
        }
    }

    let connector = ftps_connector()?;
    let ftp_stream = NativeTlsFtpStream::connect((host, 21))
        .map_err(|err| format!("failed explicit FTP control connect on port 21: {}", err))?;
    let ftp_stream = ftp_stream
        .into_secure(connector, host)
        .map_err(|err| format!("failed to switch explicit FTPS session to TLS: {}", err))?;
    finalize_ftps_login(ftp_stream, username, password)
}

fn build_reply_frame(sequence: i64, result: i64, reply: JsonValue, payload: &[u8]) -> Vec<u8> {
    let header = json!({
        "result": result,
        "sequence": sequence,
        "reply": reply
    })
    .to_string();
    let mut frame = Vec::with_capacity(header.len() + 2 + payload.len());
    frame.extend_from_slice(header.as_bytes());
    frame.extend_from_slice(b"\n\n");
    frame.extend_from_slice(payload);
    frame
}

fn preview_bytes(bytes: &[u8], limit: usize) -> String {
    let preview_len = bytes.len().min(limit);
    String::from_utf8_lossy(&bytes[..preview_len]).into_owned()
}

fn build_error_frame(sequence: i64, result: i64) -> Vec<u8> {
    build_reply_frame(sequence, result, json!({}), &[])
}

fn split_subfile_path(path: &str) -> (&str, Option<&str>) {
    match path.split_once('#') {
        Some((root, subpath)) => (root, Some(subpath)),
        None => (path, None),
    }
}

fn guess_mimetype(path: &str) -> &'static str {
    match path.rsplit('.').next().unwrap_or_default().to_ascii_lowercase().as_str() {
        "jpg" | "jpeg" => "image/jpeg",
        "png" => "image/png",
        "webp" => "image/webp",
        "model" => "application/vnd.ms-package.3dmanufacturing-3dmodel+xml",
        "rels" => "application/vnd.openxmlformats-package.relationships+xml",
        "config" => "text/plain",
        _ => "application/octet-stream",
    }
}

fn extract_zip_entry(zip_bytes: &[u8], subpath: &str) -> Result<Vec<u8>, String> {
    let reader = Cursor::new(zip_bytes);
    let mut archive =
        ZipArchive::new(reader).map_err(|err| format!("failed to open zip payload: {}", err))?;
    let mut entry = archive
        .by_name(subpath)
        .map_err(|err| format!("missing zip entry {}: {}", subpath, err))?;
    let mut bytes = Vec::new();
    entry.read_to_end(&mut bytes)
        .map_err(|err| format!("failed to read zip entry {}: {}", subpath, err))?;
    Ok(bytes)
}

fn timelapse_thumbnail_path(requested_path: &str) -> Option<String> {
    let (root, subpath) = split_subfile_path(requested_path);
    if subpath != Some("thumbnail") || !root.starts_with("timelapse/") {
        return None;
    }
    let filename = root.rsplit('/').next()?;
    let stem = filename.rsplit_once('.').map(|(base, _)| base).unwrap_or(filename);
    Some(format!("timelapse/thumbnail/{}.jpg", stem))
}

fn tunnel_download_file(
    tunnel_url: &str,
    device_id: &str,
    remote_path: &str,
) -> Result<Vec<u8>, String> {
    let host = printer_host_for_device(device_id)
        .or_else(|| tunnel_url_host(tunnel_url))
        .ok_or_else(|| "missing tunnel host".to_string())?;
    let username = tunnel_query_value(tunnel_url, "user").unwrap_or_else(|| "bblp".to_string());
    let password =
        tunnel_query_value(tunnel_url, "passwd").ok_or_else(|| "missing tunnel password".to_string())?;

    let normalized_path = remote_path.trim_start_matches('/');

    if let Ok(bytes) = tunnel_download_file_via_curl(&host, &username, &password, normalized_path) {
        return Ok(bytes);
    }

    let mut ftp_stream = connect_ftps(&host, &username, &password)?;
    let cursor = ftp_stream
        .retr_as_buffer(normalized_path)
        .map_err(|err| format!("failed to download via FTPS: {}", err))?;
    let _ = ftp_stream.quit();
    Ok(cursor.into_inner())
}

fn tunnel_download_file_via_curl(
    host: &str,
    username: &str,
    password: &str,
    remote_path: &str,
) -> Result<Vec<u8>, String> {
    let temp_path = std::env::temp_dir().join(format!(
        "bambu-tunnel-download-{}-{}.bin",
        process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|duration| duration.as_nanos())
            .unwrap_or(0)
    ));
    let remote_url = format!("ftps://{}/{}", host, remote_path);
    let status = Command::new("curl")
        .arg("--silent")
        .arg("--show-error")
        .arg("--fail")
        .arg("--ssl-reqd")
        .arg("--insecure")
        .arg("--user")
        .arg(format!("{}:{}", username, password))
        .arg("--output")
        .arg(&temp_path)
        .arg(&remote_url)
        .status()
        .map_err(|err| format!("failed to launch curl: {}", err))?;

    if !status.success() {
        let _ = fs::remove_file(&temp_path);
        return Err(format!("curl FTPS download failed with status {}", status));
    }

    let bytes = fs::read(&temp_path)
        .map_err(|err| format!("failed to read curl FTPS temp file: {}", err))?;
    let _ = fs::remove_file(&temp_path);
    Ok(bytes)
}

fn emit_disconnect_event(device_id: &str, status: i32, message: &str) {
    let_cxx_string!(device_id_cxx = device_id);
    let_cxx_string!(message_cxx = message);
    bambu_network_cb_disconnected(&device_id_cxx, status, &message_cxx);
}

fn emit_disconnect_event_if_active(device_id: &str, generation: u64, status: i32, message: &str) {
    let should_emit = ACTIVE_CONNECTION_STATE.lock().unwrap().as_ref().map(
        |(active_device_id, active_generation, _)| {
            active_device_id == device_id && *active_generation == generation
        },
    ) == Some(true);
    if should_emit {
        emit_disconnect_event(device_id, status, message);
    }
}

pub fn bambu_network_rs_init() {
    env_logger::init();
    debug!("Calling network init");
    RUNTIME.spawn(async {
        loop {
            MSG_TX.lock().unwrap().clear();
            debug!("Connecting to farm.");
            let mut client = match connect_client().await {
                Ok(client) => client,
                Err(_) => {
                    warn!("Failed to connect to farm.");
                    sleep(Duration::from_secs(1)).await;
                    continue;
                }
            };

            debug!("Requesting available printers.");
            let request = Request::new(PrinterOptionRequest {});
            let mut stream = match client.get_available_printers(request).await {
                Ok(stream) => stream.into_inner(),
                Err(_) => {
                    warn!("Error while fetching available printers.");
                    continue;
                }
            };
            if let Some(printer) = CURRENT_CONNECTED_PRINTER.lock().unwrap().as_ref() {
                bambu_network_rs_connect(printer.clone());
            }
            let mut failures = 0u32;
            loop {
                if failures > 3 {
                    break;
                }
                let list = stream.next().await;
                let list = match list {
                    Some(Ok(list)) => {
                        failures = 0;
                        list
                    }
                    _ => {
                        failures += 1;
                        sleep(Duration::from_secs(1)).await;
                        continue;
                    }
                };
                for printer in list.options {
                    let_cxx_string!(
                        json = format!(
                            "{}
                        \"dev_name\": \"{}\",
                        \"dev_id\": \"{}\",
                        \"dev_ip\": \"127.0.0.1\",
                        \"dev_type\": \"{}\",
                        \"dev_signal\": \"0dbm\",
                        \"connect_type\": \"lan\",
                        \"bind_state\": \"free\"
                        {}",
                            "{", printer.dev_name, printer.dev_id, printer.model, "}"
                        )
                        .trim()
                        .as_bytes()
                    );
                    bambu_network_cb_printer_available(&json);
                }
            }
        }
    });
}

pub fn bambu_network_rs_log_debug(message: String) {
    debug!("cxx: {}", message);
}

pub fn bambu_network_rs_connect(device_id: String) -> i32 {
    debug!("Attempting connection.");

    disconnect_active_connection();

    let (tx, mut rx) = mpsc::channel(10);
    MSG_TX.lock().unwrap().insert(device_id.clone(), tx);
    let (cancel_tx, cancel_rx) = watch::channel(false);
    let generation = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_nanos() as u64)
        .unwrap_or(0);
    ACTIVE_CONNECTION_STATE
        .lock()
        .unwrap()
        .replace((device_id.clone(), generation, cancel_tx));

    RUNTIME.spawn(async move {
        let mut recv_cancel_rx = cancel_rx.clone();
        let mut client = match connect_client().await {
            Ok(client) => client,
            Err(_) => {
                warn!("Failed to connect to farm.");
                emit_disconnect_event_if_active(&device_id, generation, 1, "Failed to connect to farm");
                clear_active_connection(&device_id, generation);
                return;
            }
        };

        let request = Request::new(ConnectRequest {
            dev_id: device_id.clone(),
        });
        let mut stream = match client.connect_printer(request).await {
            Ok(stream) => stream.into_inner(),
            Err(_) => {
                warn!("Error while fetching recieved messages.");
                emit_disconnect_event_if_active(&device_id, generation, 1, "Failed to connect to printer");
                clear_active_connection(&device_id, generation);
                return;
            }
        };
        CURRENT_CONNECTED_PRINTER
            .lock()
            .unwrap()
            .replace(device_id.clone());
        {
            let_cxx_string!(device_id_cxx = device_id.clone());
            bambu_network_cb_connected(&device_id_cxx);
        }
        let send_device_id = device_id.clone();
        let mut send_cancel_rx = cancel_rx.clone();
        spawn(async move {
            let mut failures = 0u32;
            loop {
                if failures > 3 {
                    break;
                }
                tokio::select! {
                    changed = send_cancel_rx.changed() => {
                        if changed.is_ok() && *send_cancel_rx.borrow() {
                            debug!("Send loop canceled for {}", send_device_id);
                            break;
                        }
                    }
                    maybe_message = rx.recv() => {
                        match maybe_message {
                            Some(message) => {
                                debug!("Sending message: {}", message.data);
                                let response = match client.send_message(message).await {
                                    Ok(response) => {
                                        failures = 0;
                                        response.into_inner()
                                    }
                                    Err(_) => {
                                        warn!("Error while sending message.");
                                        return;
                                    }
                                };
                                if !response.success {
                                    warn!("Message failed to send.");
                                    failures += 1;
                                }
                            }
                            None => break,
                        }
                    }
                }
            }
        });
        let mut failures = 0u32;
        loop {
            if failures > 3 {
                break;
            }
            let recv_message = tokio::select! {
                changed = recv_cancel_rx.changed() => {
                    if changed.is_ok() && *recv_cancel_rx.borrow() {
                        debug!("Receive loop canceled for {}", device_id);
                        emit_disconnect_event_if_active(&device_id, generation, 2, "Disconnected");
                    }
                    break;
                }
                recv_message = stream.next() => recv_message,
            };
            let recv_message = match recv_message {
                Some(Ok(recv_message)) => {
                    failures = 0;
                    recv_message
                }
                _ => {
                    failures += 1;
                    sleep(Duration::from_secs(10)).await;
                    continue;
                }
            };
            if !recv_message.connected {
                emit_disconnect_event_if_active(&device_id, generation, 2, "Disconnected");
                break;
            }
            let_cxx_string!(id = recv_message.dev_id);
            let_cxx_string!(json = recv_message.data);
            bambu_network_cb_message_recv(&id, &json);
        }
        clear_active_connection(&device_id, generation);
    });
    0
}

pub fn bambu_network_rs_disconnect(device_id: String) -> i32 {
    let active_device = ACTIVE_CONNECTION_STATE
        .lock()
        .unwrap()
        .as_ref()
        .map(|(active_device_id, _, _)| active_device_id.clone());
    if active_device.as_ref() == Some(&device_id) {
        disconnect_active_connection();
        emit_disconnect_event(&device_id, 2, "Disconnected");
    } else {
        MSG_TX.lock().unwrap().remove(&device_id);
        let mut current = CURRENT_CONNECTED_PRINTER.lock().unwrap();
        if current.as_ref() == Some(&device_id) {
            current.take();
        }
    }
    BAMBU_NETWORK_SUCCESS
}

pub fn bambu_network_rs_send(device_id: String, data: String) -> i32 {
    debug!("Sending {}", data);

    let sender = MSG_TX.lock().unwrap().get(&device_id).cloned();
    match sender {
        Some(sender) => match sender.try_send(SendMessageRequest {
            dev_id: device_id,
            data,
        }) {
            Ok(_) => BAMBU_NETWORK_SUCCESS,
            Err(_) => BAMBU_NETWORK_ERR_SEND_MSG_FAILED,
        },
        None => BAMBU_NETWORK_ERR_SEND_MSG_FAILED,
    }
}

pub fn bambu_network_rs_upload_file(
    device_id: String,
    local_filename: String,
    remote_filename: String,
) -> i32 {
    // This function may be called either from Studio's UI thread (normal case)
    // or from within a Tokio worker thread (when Studio calls back into us from
    // an MQTT/connected callback). block_on() panics on a Tokio worker thread,
    // so we use block_in_place + handle.block_on() when already inside the
    // runtime, and fall back to RUNTIME.block_on() otherwise.
    let upload = async move {
        bambu_network_rs_log_debug(format!(
            "bambu_network_rs_upload_file: local_filename={} remote_filename={}",
            local_filename, remote_filename
        ));

        let mut client = match connect_client().await {
            Ok(client) => client,
            Err(_) => {
                warn!("Failed to connect to farm.");
                bambu_network_rs_log_debug(
                    "bambu_network_rs_upload_file: failed to connect to farm".to_string(),
                );
                return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
            }
        };

        let file = match File::open(&local_filename).await {
            Ok(file) => {
                let bytes = file.metadata().await.map(|meta| meta.len()).unwrap_or(0);
                bambu_network_rs_log_debug(format!(
                    "bambu_network_rs_upload_file: opened local file bytes={}",
                    bytes
                ));
                file
            }
            Err(e) => {
                warn!("Failed to open local file {}: {}", local_filename, e);
                bambu_network_rs_log_debug(format!(
                    "bambu_network_rs_upload_file: failed to open local file error={}",
                    e
                ));
                return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
            }
        };

        let device_id_for_stream = device_id.clone();
        let remote_filename_for_stream = remote_filename.clone();
        let stream = stream::unfold(file, move |mut file| {
            let dev_id = device_id_for_stream.clone();
            let remote_path = remote_filename_for_stream.clone();
            let local_filename_for_chunk = local_filename.clone();
            async move {
                let mut buf = vec![0u8; UPLOAD_CHUNK_SIZE];
                match file.read(&mut buf).await {
                    Ok(0) => None,
                    Ok(bytes_read) => {
                        buf.truncate(bytes_read);
                        Some((
                            UploadFileChunk {
                                dev_id: dev_id,
                                blob: buf,
                                remote_path: remote_path,
                            },
                            file,
                        ))
                    }
                    Err(err) => {
                        warn!(
                            "Failed to read local file chunk {}: {}",
                            local_filename_for_chunk,
                            err
                        );
                        bambu_network_rs_log_debug(format!(
                            "bambu_network_rs_upload_file: failed to read local file chunk error={}",
                            err
                        ));
                        None
                    }
                }
            }
        });

        let result = client
            .upload_file_stream(stream)
            .await;
        match result {
            Ok(response) => {
                let success = response.into_inner().success;
                bambu_network_rs_log_debug(format!(
                    "bambu_network_rs_upload_file: upload stream RPC success_flag={}",
                    success
                ));
                if success {
                    BAMBU_NETWORK_SUCCESS
                } else {
                    BAMBU_NETWORK_ERR_SEND_MSG_FAILED
                }
            }
            Err(e) => {
                bambu_network_rs_log_debug(format!(
                    "bambu_network_rs_upload_file: upload stream RPC failed error={}",
                    e
                ));
                BAMBU_NETWORK_ERR_SEND_MSG_FAILED
            }
        }
    };

    match tokio::runtime::Handle::try_current() {
        Ok(handle) => tokio::task::block_in_place(|| handle.block_on(upload)),
        Err(_) => RUNTIME.block_on(upload),
    }
}

pub fn bambu_network_rs_tunnel_request(
    tunnel_url: String,
    device_id: String,
    data: String,
) -> Vec<u8> {
    let request_body = data.clone();
    let request = async move {
        let request_json = request_body
            .split("\n\n")
            .next()
            .unwrap_or(request_body.as_str());
        let parsed: JsonValue = match serde_json::from_str(request_json) {
            Ok(parsed) => parsed,
            Err(err) => {
                bambu_network_rs_log_debug(format!(
                    "bambu_network_rs_tunnel_request: request parse failed error={}",
                    err
                ));
                return Vec::new();
            }
        };
        let sequence = parsed.get("sequence").and_then(JsonValue::as_i64).unwrap_or(0);
        let cmdtype = parsed.get("cmdtype").and_then(JsonValue::as_i64).unwrap_or(-1);
        let req = parsed.get("req").cloned().unwrap_or_else(|| json!({}));

        if cmdtype == 0x0004 {
            let remote_path = req
                .get("path")
                .and_then(JsonValue::as_str)
                .or_else(|| req.get("file").and_then(JsonValue::as_str))
                .unwrap_or("");
            if remote_path.is_empty() {
                bambu_network_rs_log_debug(
                    "bambu_network_rs_tunnel_request: FILE_DOWNLOAD missing remote path".to_string(),
                );
                return build_error_frame(sequence, 10);
            }

            match tunnel_download_file(&tunnel_url, &device_id, remote_path) {
                Ok(blob) => {
                    let md5_hex = format!("{:x}", md5::compute(&blob));
                    bambu_network_rs_log_debug(format!(
                        "bambu_network_rs_tunnel_request: FILE_DOWNLOAD success path={} bytes={} md5={}",
                        remote_path,
                        blob.len(),
                        md5_hex
                    ));
                    return build_reply_frame(
                        sequence,
                        0,
                        json!({
                            "size": blob.len(),
                            "offset": 0,
                            "total": blob.len(),
                            "file_md5": md5_hex,
                            "ftp_file_md5": md5_hex,
                            "path": remote_path,
                        }),
                        &blob,
                    );
                }
                Err(err) => {
                    bambu_network_rs_log_debug(format!(
                        "bambu_network_rs_tunnel_request: FILE_DOWNLOAD failed path={} error={}",
                        remote_path, err
                    ));
                    return build_error_frame(sequence, 10);
                }
            }
        }

        if cmdtype == 0x0002 {
            let paths: Vec<String> = req
                .get("paths")
                .and_then(JsonValue::as_array)
                .map(|arr| {
                    arr.iter()
                        .filter_map(JsonValue::as_str)
                        .map(ToOwned::to_owned)
                        .collect()
                })
                .unwrap_or_default();
            if paths.is_empty() {
                bambu_network_rs_log_debug(
                    "bambu_network_rs_tunnel_request: SUB_FILE missing paths".to_string(),
                );
                return build_error_frame(sequence, 10);
            }

            let first_path = paths[0].clone();
            let zip_requested = req.get("zip").and_then(JsonValue::as_bool).unwrap_or(false);
            let (root_path, subpath) = split_subfile_path(&first_path);

            if zip_requested {
                match tunnel_download_file(&tunnel_url, &device_id, root_path) {
                    Ok(blob) => {
                        bambu_network_rs_log_debug(format!(
                            "bambu_network_rs_tunnel_request: SUB_FILE zip success path={} bytes={}",
                            root_path,
                            blob.len()
                        ));
                        return build_reply_frame(
                            sequence,
                            0,
                            json!({
                                "path": root_path,
                                "size": blob.len(),
                                "continue": false,
                            }),
                            &blob,
                        );
                    }
                    Err(err) => {
                        bambu_network_rs_log_debug(format!(
                            "bambu_network_rs_tunnel_request: SUB_FILE zip failed path={} error={}",
                            root_path, err
                        ));
                        return build_error_frame(sequence, 10);
                    }
                }
            }

            if let Some(thumbnail_remote_path) = timelapse_thumbnail_path(&first_path) {
                match tunnel_download_file(&tunnel_url, &device_id, &thumbnail_remote_path) {
                    Ok(blob) => {
                        bambu_network_rs_log_debug(format!(
                            "bambu_network_rs_tunnel_request: SUB_FILE timelapse thumbnail success path={} remote={} bytes={}",
                            first_path,
                            thumbnail_remote_path,
                            blob.len()
                        ));
                        return build_reply_frame(
                            sequence,
                            0,
                            json!({
                                "path": first_path,
                                "thumbnail": "thumbnail",
                                "mimetype": "image/jpeg",
                                "size": blob.len(),
                                "continue": false,
                            }),
                            &blob,
                        );
                    }
                    Err(err) => {
                        bambu_network_rs_log_debug(format!(
                            "bambu_network_rs_tunnel_request: SUB_FILE timelapse thumbnail failed path={} remote={} error={}",
                            first_path, thumbnail_remote_path, err
                        ));
                        return build_error_frame(sequence, 10);
                    }
                }
            }

            if let Some(subpath) = subpath {
                match tunnel_download_file(&tunnel_url, &device_id, root_path) {
                    Ok(blob) => match extract_zip_entry(&blob, subpath) {
                        Ok(entry_bytes) => {
                            bambu_network_rs_log_debug(format!(
                                "bambu_network_rs_tunnel_request: SUB_FILE zip entry success path={} bytes={}",
                                first_path,
                                entry_bytes.len()
                            ));
                            return build_reply_frame(
                                sequence,
                                0,
                                json!({
                                    "path": first_path,
                                    "thumbnail": subpath,
                                    "mimetype": guess_mimetype(subpath),
                                    "size": entry_bytes.len(),
                                    "continue": false,
                                }),
                                &entry_bytes,
                            );
                        }
                        Err(err) => {
                            bambu_network_rs_log_debug(format!(
                                "bambu_network_rs_tunnel_request: SUB_FILE zip entry failed path={} error={}",
                                first_path, err
                            ));
                            return build_error_frame(sequence, 10);
                        }
                    },
                    Err(err) => {
                        bambu_network_rs_log_debug(format!(
                            "bambu_network_rs_tunnel_request: SUB_FILE remote download failed path={} error={}",
                            root_path, err
                        ));
                        return build_error_frame(sequence, 10);
                    }
                }
            }

            bambu_network_rs_log_debug(format!(
                "bambu_network_rs_tunnel_request: SUB_FILE unsupported request path={} zip={}",
                first_path, zip_requested
            ));
            return build_error_frame(sequence, 10);
        }

        let mut client = match connect_client().await {
            Ok(client) => client,
            Err(err) => {
                bambu_network_rs_log_debug(format!(
                    "bambu_network_rs_tunnel_request: failed to connect to farm error={}",
                    err
                ));
                return Vec::new();
            }
        };

        match client
            .send_control_message(ControlMessageRequest {
                dev_id: device_id,
                data: request_body,
            })
            .await
        {
            Ok(response) => {
                let bytes = response.into_inner().data.into_bytes();
                bambu_network_rs_log_debug(format!(
                    "bambu_network_rs_tunnel_request: control RPC success cmdtype={} sequence={} bytes={} preview={}",
                    cmdtype,
                    sequence,
                    bytes.len(),
                    preview_bytes(&bytes, 240)
                ));
                bytes
            }
            Err(err) => {
                bambu_network_rs_log_debug(format!(
                    "bambu_network_rs_tunnel_request: control RPC failed error={}",
                    err
                ));
                Vec::new()
            }
        }
    };

    match tokio::runtime::Handle::try_current() {
        Ok(handle) => tokio::task::block_in_place(|| handle.block_on(request)),
        Err(_) => RUNTIME.block_on(request),
    }
}

pub fn bambu_network_rs_extract_3mf_thumbnail(
    local_filename: String,
    plate_index: i32,
    output_dir: String,
) -> String {
    let plate_number = if plate_index > 0 { plate_index as usize } else { 1usize };
    let candidates = [
        format!("Metadata/plate_{}.png", plate_number),
        format!("Metadata/plate_{}_small.png", plate_number),
        "Auxiliaries/.thumbnails/thumbnail_3mf.png".to_string(),
        "/Auxiliaries/.thumbnails/thumbnail_3mf.png".to_string(),
        "Metadata/plate_1.png".to_string(),
        "Metadata/plate_1_small.png".to_string(),
    ];

    let file = match fs::File::open(&local_filename) {
        Ok(file) => file,
        Err(err) => {
            bambu_network_rs_log_debug(format!(
                "bambu_network_rs_extract_3mf_thumbnail: open failed path={} error={}",
                local_filename, err
            ));
            return String::new();
        }
    };
    let mut archive = match ZipArchive::new(file) {
        Ok(archive) => archive,
        Err(err) => {
            bambu_network_rs_log_debug(format!(
                "bambu_network_rs_extract_3mf_thumbnail: zip open failed path={} error={}",
                local_filename, err
            ));
            return String::new();
        }
    };

    let mut png_bytes = Vec::new();
    let mut matched_name = String::new();
    for candidate in candidates {
        match archive.by_name(&candidate) {
            Ok(mut entry) => {
                if entry.read_to_end(&mut png_bytes).is_ok() && !png_bytes.is_empty() {
                    matched_name = candidate;
                    break;
                }
                png_bytes.clear();
            }
            Err(_) => {}
        }
    }

    if png_bytes.is_empty() {
        bambu_network_rs_log_debug(format!(
            "bambu_network_rs_extract_3mf_thumbnail: no thumbnail found path={} plate_index={}",
            local_filename, plate_index
        ));
        return String::new();
    }

    if let Err(err) = fs::create_dir_all(&output_dir) {
        bambu_network_rs_log_debug(format!(
            "bambu_network_rs_extract_3mf_thumbnail: mkdir failed path={} error={}",
            output_dir, err
        ));
        return String::new();
    }

    let mut hasher = DefaultHasher::new();
    local_filename.hash(&mut hasher);
    plate_index.hash(&mut hasher);
    png_bytes.hash(&mut hasher);
    let output_path = PathBuf::from(&output_dir).join(format!("{:016x}.png", hasher.finish()));

    if let Err(err) = fs::write(&output_path, &png_bytes) {
        bambu_network_rs_log_debug(format!(
            "bambu_network_rs_extract_3mf_thumbnail: write failed path={} error={}",
            output_path.display(),
            err
        ));
        return String::new();
    }

    let url = match Url::from_file_path(&output_path) {
        Ok(url) => url.to_string(),
        Err(_) => {
            bambu_network_rs_log_debug(format!(
                "bambu_network_rs_extract_3mf_thumbnail: url encode failed path={}",
                output_path.display()
            ));
            return String::new();
        }
    };

    bambu_network_rs_log_debug(format!(
        "bambu_network_rs_extract_3mf_thumbnail: extracted source={} output={} url={}",
        matched_name,
        output_path.display(),
        url
    ));
    url
}
