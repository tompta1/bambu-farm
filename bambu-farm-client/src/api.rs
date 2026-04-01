use std::collections::HashMap;
use std::env::current_dir;
use std::env::var_os;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::Duration;

use cxx::let_cxx_string;

use config::{Config, ConfigError};
use futures::StreamExt;
use futures::stream;
use lazy_static::lazy_static;
use log::debug;
use log::warn;
use tokio::spawn;
use tokio::fs::File;
use tokio::io::AsyncReadExt;
use tokio::sync::mpsc;
use tokio::sync::mpsc::Sender;
use tokio::sync::watch;
use tokio::time::sleep;
use tonic::Request;

use crate::api::bambu_farm::{ConnectRequest, SendMessageRequest, UploadFileChunk};
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
