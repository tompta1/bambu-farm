use bambu_farm::PrinterOption;
use bambu_farm::{
    bambu_farm_server::BambuFarm, ConnectRequest, ControlMessageRequest, ControlMessageResponse,
    PrinterOptionList, PrinterOptionRequest, RecvMessage, SendMessageRequest, SendMessageResponse,
    UploadFileChunk, UploadFileRequest, UploadFileResponse,
};
use config::{Config, ConfigError, Value};
use if_addrs::{IfAddr, get_if_addrs};
use futures::stream;
use futures::StreamExt;
use log::{error, info, warn};
use paho_mqtt::{
    AsyncClient, Client, ConnectOptionsBuilder, CreateOptionsBuilder, Message, SslOptionsBuilder,
};
use serde_json::{json, Value as JsonValue};
use std::convert::TryFrom;
use suppaftp::native_tls::TlsConnector;
use suppaftp::list::File as FtpListFile;
use suppaftp::types::FileType;
use suppaftp::{NativeTlsConnector, NativeTlsFtpStream};
use tempfile::NamedTempFile;
use tokio::spawn;
use tokio::task::spawn_blocking;
use tokio::time::sleep;

use crate::bambu_farm::bambu_farm_server::BambuFarmServer;
use std::collections::HashMap;
use std::env::current_dir;
use std::io::{Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::path::Path;
use std::process::Command;
use std::sync::Arc;
use std::sync::Mutex;
use std::time::Duration;
use tokio::sync::mpsc;
use tokio_stream::wrappers::ReceiverStream;
use tonic::{transport::Server, Request, Response, Status};

pub mod bambu_farm {
    tonic::include_proto!("_");
}

const GRPC_MAX_MESSAGE_SIZE: usize = 32 * 1024 * 1024;
const CONTROL_CMD_LIST_INFO: i64 = 0x0001;
const CONTROL_CMD_REQUEST_MEDIA_ABILITY: i64 = 0x0007;
const MQTT_MIN_RECONNECT_DELAY: Duration = Duration::from_secs(1);
const MQTT_MAX_RECONNECT_DELAY: Duration = Duration::from_secs(30);
const MQTT_KEEP_ALIVE: Duration = Duration::from_secs(20);
const DISCOVERY_TCP_TIMEOUT: Duration = Duration::from_millis(150);
const DISCOVERY_MQTT_TIMEOUT: Duration = Duration::from_secs(2);
const DISCOVERY_PROBE_SLEEP: Duration = Duration::from_millis(200);
const DISCOVERY_SCAN_CONCURRENCY: usize = 64;

#[derive(Debug, Clone)]
struct DiscoveryMatch {
    ip: Ipv4Addr,
    name: Option<String>,
    model: Option<String>,
}

fn local_ipv4_candidates() -> Vec<Ipv4Addr> {
    let mut candidates = Vec::new();

    let interfaces = match get_if_addrs() {
        Ok(interfaces) => interfaces,
        Err(err) => {
            warn!("failed to enumerate local interfaces for discovery: {}", err);
            return candidates;
        }
    };

    for iface in interfaces {
        let IfAddr::V4(v4) = iface.addr else {
            continue;
        };
        let ip = v4.ip;
        if ip.is_loopback() || !ip.is_private() {
            continue;
        }

        let ip_u32 = u32::from(ip);
        let netmask_u32 = u32::from(v4.netmask);
        let prefix_len = netmask_u32.count_ones();
        let effective_mask = if prefix_len < 24 {
            0xFFFF_FF00
        } else {
            netmask_u32
        };
        let network = ip_u32 & effective_mask;
        let broadcast = network | !effective_mask;
        if broadcast <= network + 1 {
            continue;
        }

        for host in (network + 1)..broadcast {
            let candidate = Ipv4Addr::from(host);
            if candidate != ip {
                candidates.push(candidate);
            }
        }
    }

    candidates.sort_unstable();
    candidates.dedup();
    candidates
}

async fn mqtt_reachable_hosts() -> Vec<Ipv4Addr> {
    stream::iter(local_ipv4_candidates())
        .map(|ip| async move {
            let addr = SocketAddr::new(IpAddr::V4(ip), 8883);
            match tokio::time::timeout(DISCOVERY_TCP_TIMEOUT, tokio::net::TcpStream::connect(addr)).await {
                Ok(Ok(stream)) => {
                    drop(stream);
                    Some(ip)
                }
                _ => None,
            }
        })
        .buffer_unordered(DISCOVERY_SCAN_CONCURRENCY)
        .filter_map(|ip| async move { ip })
        .collect()
        .await
}

fn model_code_from_product_name(product_name: &str) -> Option<&'static str> {
    let product_name = product_name.trim().to_ascii_lowercase();
    if product_name.contains("x1 carbon") || product_name.contains("x1c") {
        Some("3DPrinter-X1-Carbon")
    } else if product_name.contains("x1") {
        Some("3DPrinter-X1")
    } else if product_name.contains("p1s") {
        Some("C12")
    } else if product_name.contains("p1p") {
        Some("C11")
    } else if product_name.contains("a1 mini") || product_name.contains("a1mini") {
        Some("N1")
    } else if product_name.contains("a1") {
        Some("N2S")
    } else {
        None
    }
}

fn discovery_match_from_payload(candidate_ip: Ipv4Addr, payload: &str) -> DiscoveryMatch {
    let root: JsonValue = serde_json::from_str(payload).unwrap_or(JsonValue::Null);
    let modules = root
        .get("info")
        .and_then(|info| info.get("module"))
        .and_then(JsonValue::as_array);
    let product_name = modules
        .and_then(|modules| {
            modules
                .iter()
                .find(|module| {
                    module
                        .get("visible")
                        .and_then(JsonValue::as_bool)
                        .unwrap_or(false)
                        && module
                            .get("product_name")
                            .and_then(JsonValue::as_str)
                            .map(|name| !name.trim().is_empty())
                            .unwrap_or(false)
                })
                .or_else(|| {
                    modules.iter().find(|module| {
                        module
                            .get("product_name")
                            .and_then(JsonValue::as_str)
                            .map(|name| !name.trim().is_empty())
                            .unwrap_or(false)
                    })
                })
        })
        .and_then(|module| module.get("product_name"))
        .and_then(JsonValue::as_str)
        .map(str::trim)
        .filter(|name| !name.is_empty())
        .map(str::to_string);

    DiscoveryMatch {
        ip: candidate_ip,
        model: product_name
            .as_deref()
            .and_then(model_code_from_product_name)
            .map(str::to_string),
        name: product_name,
    }
}

fn probe_printer_identity(printer: &Printer, candidate_ip: Ipv4Addr) -> Option<DiscoveryMatch> {
    let server_uri = format!("mqtts://{}:8883", candidate_ip);
    let client_id = format!("bambu-farm-discovery-{}-{}", printer.id, candidate_ip);
    let cli = match Client::new(
        CreateOptionsBuilder::new()
            .server_uri(server_uri)
            .client_id(client_id)
            .finalize(),
    ) {
        Ok(cli) => cli,
        Err(err) => {
            warn!("discovery probe client create failed for {}: {}", candidate_ip, err);
            return None;
        }
    };

    let rx = cli.start_consuming();
    let connect_opts = ConnectOptionsBuilder::new()
        .user_name("bblp")
        .password(&printer.password)
        .keep_alive_interval(Duration::from_secs(5))
        .connect_timeout(Duration::from_secs(2))
        .clean_session(true)
        .ssl_options(
            SslOptionsBuilder::new()
                .verify(false)
                .enable_server_cert_auth(false)
                .finalize(),
        )
        .finalize();

    if let Err(err) = cli.connect(Some(connect_opts)) {
        warn!(
            "discovery probe connect failed for printer={} candidate_ip={}: {}",
            printer.id, candidate_ip, err
        );
        return None;
    }

    let report_topic = format!("device/{}/report", printer.id);
    let request_topic = format!("device/{}/request", printer.id);
    let subscribe_ok = cli.subscribe(&report_topic, 0).is_ok();
    let publish_ok = subscribe_ok
        && cli
            .publish(Message::new(
                &request_topic,
                json!({"info":{"command":"get_version","sequence_id":"discovery"}}).to_string(),
                0,
            ))
            .is_ok();

    let mut matched = None;
    if publish_ok {
        let deadline = std::time::Instant::now() + DISCOVERY_MQTT_TIMEOUT;
        while std::time::Instant::now() < deadline {
            if let Ok(Some(message)) = rx.recv_timeout(DISCOVERY_PROBE_SLEEP) {
                if message.topic() == report_topic {
                    matched = Some(discovery_match_from_payload(
                        candidate_ip,
                        &message.payload_str(),
                    ));
                    break;
                }
            }
        }
    }

    let _ = cli.disconnect(None);
    cli.stop_consuming();
    matched
}

async fn discover_printer(printer: &Printer) -> Option<DiscoveryMatch> {
    let candidates = mqtt_reachable_hosts().await;
    info!(
        "discovery scanning {} MQTT-reachable hosts for printer={}",
        candidates.len(),
        printer.id
    );

    for candidate in candidates {
        if let Some(discovered) = probe_printer_identity(printer, candidate) {
            info!("discovery matched printer={} at {}", printer.id, candidate);
            return Some(discovered);
        }
    }

    None
}

fn remote_media_dir(requested_type: &str) -> Option<&'static str> {
    match requested_type {
        "timelapse" => Some("timelapse"),
        "video" => Some("ipcam"),
        _ => None,
    }
}

fn control_response(sequence: i64, result: i64, reply: JsonValue) -> String {
    json!({
        "result": result,
        "sequence": sequence,
        "reply": reply
    })
    .to_string()
}

fn control_error_response(sequence: i64, result: i64) -> String {
    control_response(sequence, result, json!({}))
}

type UploadHandler =
    Arc<dyn Fn(&Printer, &str, &Path, usize) -> Result<bool, Status> + Send + Sync + 'static>;

#[derive(Debug, Clone)]
struct Printer {
    name: String,
    id: String,
    ip: String,
    model: String,
    password: String,
}

#[derive(Clone)]
pub struct Farm {
    printers: Arc<Mutex<Vec<Printer>>>,
    connections: Arc<Mutex<HashMap<String, tokio::sync::mpsc::Sender<SendMessageRequest>>>>,
    upload_handler: UploadHandler,
}

fn ftps_connector() -> Result<NativeTlsConnector, String> {
    let mut builder = TlsConnector::builder();
    builder.danger_accept_invalid_certs(true);
    builder.danger_accept_invalid_hostnames(true);
    let connector = builder
        .build()
        .map_err(|e| format!("failed to build TLS connector: {}", e))?;
    Ok(NativeTlsConnector::from(connector))
}

fn finalize_rust_ftps_login(
    mut ftp_stream: NativeTlsFtpStream,
    printer: &Printer,
) -> Result<NativeTlsFtpStream, String> {
    ftp_stream
        .login("bblp", &printer.password)
        .map_err(|e| format!("failed to login to FTPS server: {}", e))?;
    ftp_stream
        .transfer_type(FileType::Binary)
        .map_err(|e| format!("failed to enable binary transfer mode: {}", e))?;
    Ok(ftp_stream)
}

fn connect_rust_ftps_implicit(printer: &Printer) -> Result<NativeTlsFtpStream, String> {
    let connector = ftps_connector()?;
    let ftp_stream =
        NativeTlsFtpStream::connect_secure_implicit((printer.ip.as_str(), 990), connector, printer.ip.as_str())
            .map_err(|e| format!("failed implicit FTPS connect on port 990: {}", e))?;
    finalize_rust_ftps_login(ftp_stream, printer)
}

fn connect_rust_ftps_explicit(printer: &Printer) -> Result<NativeTlsFtpStream, String> {
    let connector = ftps_connector()?;
    let ftp_stream = NativeTlsFtpStream::connect((printer.ip.as_str(), 21))
        .map_err(|e| format!("failed explicit FTP control connect on port 21: {}", e))?;
    let ftp_stream = ftp_stream
        .into_secure(connector, printer.ip.as_str())
        .map_err(|e| format!("failed to switch explicit FTPS session to TLS: {}", e))?;
    finalize_rust_ftps_login(ftp_stream, printer)
}

fn connect_rust_ftps(printer: &Printer) -> Result<NativeTlsFtpStream, String> {
    match connect_rust_ftps_implicit(printer) {
        Ok(ftp_stream) => Ok(ftp_stream),
        Err(implicit_err) => {
            info!(
                "implicit FTPS connect failed for printer={} on port 990: {}; trying explicit FTPS on port 21",
                printer.ip, implicit_err
            );
            connect_rust_ftps_explicit(printer).map_err(|explicit_err| {
                format!(
                    "implicit FTPS on port 990 failed: {}; explicit FTPS on port 21 failed: {}",
                    implicit_err, explicit_err
                )
            })
        }
    }
}

fn list_files_via_ftps(printer: &Printer, requested_type: &str) -> Result<JsonValue, String> {
    if requested_type != "model" && requested_type != "timelapse" && requested_type != "video" {
        return Ok(json!({ "file_lists": [] }));
    }
    let remote_dir = remote_media_dir(requested_type);

    let mut ftp_stream = connect_rust_ftps(printer)?;
    let raw_entries = match ftp_stream.mlsd(remote_dir) {
        Ok(entries) => entries,
        Err(mlsd_err) => {
            info!(
                "MLSD failed for printer={} type={} dir={}: {}; reconnecting and falling back to LIST",
                printer.ip,
                requested_type,
                remote_dir.unwrap_or("/"),
                mlsd_err
            );
            let _ = ftp_stream.quit();
            let mut fallback_stream = connect_rust_ftps(printer)?;
            let entries = fallback_stream
                .list(remote_dir)
                .map_err(|list_err| format!("failed to list remote files: {}", list_err))?;
            let _ = fallback_stream.quit();
            entries
        }
    };

    let mut entries = Vec::new();

    for line in raw_entries {
        let file = match FtpListFile::try_from(line.as_str()) {
            Ok(file) => file,
            Err(parse_err) => {
                warn!("failed to parse FTPS listing line {:?}: {}", line, parse_err);
                continue;
            }
        };
        if !file.is_file() {
            continue;
        }
        let name = file.name().to_string();
        if requested_type == "model" {
            if name.starts_with('.') {
                continue;
            }
            if name == "check_access_code.txt" || name == "verify_job" {
                continue;
            }
            if !(name.ends_with(".gcode.3mf") || name.ends_with(".3mf")) {
                continue;
            }
        }
        let remote_path = match remote_dir {
            Some(remote_dir) => format!("{}/{}", remote_dir, file.name()),
            None => file.name().to_string(),
        };
        let modified = file
            .modified()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|duration| duration.as_secs() as i64)
            .unwrap_or(0);
        entries.push(json!({
            "name": name,
            "path": remote_path,
            "time": modified,
            "size": file.size() as u64
        }));
    }

    let _ = ftp_stream.quit();
    info!(
        "LIST_INFO succeeded for printer={} type={} count={}",
        printer.id,
        requested_type,
        entries.len()
    );
    Ok(json!({ "file_lists": entries }))
}

fn handle_control_message(printer: &Printer, data: &str) -> String {
    let root: JsonValue = match serde_json::from_str(data) {
        Ok(root) => root,
        Err(err) => {
            warn!("control request parse failed: {}", err);
            return control_error_response(0, 2);
        }
    };

    let sequence = root.get("sequence").and_then(JsonValue::as_i64).unwrap_or(0);
    let cmdtype = root.get("cmdtype").and_then(JsonValue::as_i64).unwrap_or(-1);
    let req = root.get("req").cloned().unwrap_or_else(|| json!({}));

    match cmdtype {
        CONTROL_CMD_REQUEST_MEDIA_ABILITY => {
            control_response(sequence, 0, json!({ "storage": ["internal"] }))
        }
        CONTROL_CMD_LIST_INFO => {
            let requested_type = req
                .get("type")
                .and_then(JsonValue::as_str)
                .unwrap_or("model");
            match list_files_via_ftps(printer, requested_type) {
                Ok(reply) => control_response(sequence, 0, reply),
                Err(err) => {
                    warn!(
                        "LIST_INFO failed for printer={} type={}: {}",
                        printer.id, requested_type, err
                    );
                    control_error_response(sequence, 17)
                }
            }
        }
        _ => {
            info!(
                "unsupported control cmdtype={} for printer={} payload={}",
                cmdtype, printer.id, data
            );
            control_error_response(sequence, 18)
        }
    }
}

fn upload_path_via_rust_ftps(printer: &Printer, remote_path: &str, local_path: &Path) -> Result<(), String> {
    let mut ftp_stream = connect_rust_ftps(printer)?;
    if let Err(e) = ftp_stream.rm(remote_path) {
        info!(
            "rust FTPS pre-delete skipped for printer={} remote_path={}: {}",
            printer.ip, remote_path, e
        );
    }

    let mut reader = std::fs::File::open(local_path)
        .map_err(|e| format!("failed to open local upload file: {}", e))?;
    ftp_stream
        .put_file(remote_path, &mut reader)
        .map_err(|e| format!("failed to upload via rust FTPS: {}", e))?;
    ftp_stream
        .quit()
        .map_err(|e| format!("failed to close rust FTPS session: {}", e))?;
    Ok(())
}

fn upload_path_via_curl(printer: &Printer, remote_path: &str, local_path: &Path) -> Result<bool, Status> {
    let auth = format!("bblp:{}", printer.password);
    let base_url = format!("ftps://{}/", printer.ip);
    let target_url = format!("ftps://{}/{}", printer.ip, remote_path);

    let _ = Command::new("curl")
        .args([
            "--fail",
            "--silent",
            "--show-error",
            "--ssl-reqd",
            "--insecure",
            "--user",
            &auth,
            &base_url,
            "--quote",
            &format!("DELE {}", remote_path),
        ])
        .output();

    info!("curl FTPS upload to printer={} remote_path={}", printer.ip, remote_path);
    let output = Command::new("curl")
        .args([
            "--fail",
            "--silent",
            "--show-error",
            "--ssl-reqd",
            "--insecure",
            "--user",
            &auth,
            "--upload-file",
            local_path.to_str().unwrap(),
            &target_url,
        ])
        .output()
        .map_err(|e| {
            error!("curl exec failed: {}", e);
            Status::internal("curl exec failed")
        })?;

    info!("Upload curl status for remote_path={}: {}", remote_path, output.status);
    if !output.stdout.is_empty() {
        info!("Upload curl stdout: {}", String::from_utf8_lossy(&output.stdout));
    }
    if !output.stderr.is_empty() {
        info!("Upload curl stderr: {}", String::from_utf8_lossy(&output.stderr));
    }

    Ok(output.status.success())
}

fn default_upload_handler(printer: &Printer, remote_path: &str, local_path: &Path, total_bytes: usize) -> Result<bool, Status> {
    info!(
        "Uploading file for printer={} remote_path={} bytes={}",
        printer.id,
        remote_path,
        total_bytes
    );

    match upload_path_via_rust_ftps(printer, remote_path, local_path) {
        Ok(()) => {
            info!(
                "rust FTPS upload succeeded for printer={} remote_path={}",
                printer.ip, remote_path
            );
            Ok(true)
        }
        Err(err) => {
            warn!(
                "rust FTPS upload failed for printer={} remote_path={}: {}; falling back to curl",
                printer.ip, remote_path, err
            );
            upload_path_via_curl(printer, remote_path, local_path)
        }
    }
}

impl Default for Farm {
    fn default() -> Self {
        Self {
            printers: Arc::new(Mutex::new(Vec::new())),
            connections: Arc::new(Mutex::new(HashMap::new())),
            upload_handler: Arc::new(default_upload_handler),
        }
    }
}

impl Farm {
    #[cfg(test)]
    fn with_upload_handler(printers: Vec<Printer>, upload_handler: UploadHandler) -> Self {
        Self {
            printers: Arc::new(Mutex::new(printers)),
            connections: Arc::new(Mutex::new(HashMap::new())),
            upload_handler,
        }
    }

    fn lookup_printer(&self, dev_id: &str) -> Result<Printer, Status> {
        let printers = self
            .printers
            .lock()
            .map_err(|_| Status::unknown("Could not lock printer list."))?
            .clone();
        printers
            .into_iter()
            .find(|printer| printer.id == dev_id)
            .ok_or_else(|| {
                warn!("upload_file: no matching printer for dev_id={}", dev_id);
                Status::unknown("Could not find matching printer")
            })
    }

    fn update_printer_discovery(&self, dev_id: &str, discovered: &DiscoveryMatch) {
        if let Ok(mut printers) = self.printers.lock() {
            if let Some(printer) = printers.iter_mut().find(|printer| printer.id == dev_id) {
                printer.ip = discovered.ip.to_string();
                if printer.name.trim().is_empty() {
                    if let Some(name) = discovered.name.as_deref().map(str::trim).filter(|name| !name.is_empty()) {
                        printer.name = name.to_string();
                    }
                }
                if printer.model.trim().is_empty() {
                    if let Some(model) = discovered.model.as_deref().map(str::trim).filter(|model| !model.is_empty()) {
                        printer.model = model.to_string();
                    }
                }
            }
        }
    }

    async fn refresh_discovery_metadata(&self) {
        let printers = match self.printers.lock() {
            Ok(printers) => printers.clone(),
            Err(_) => return,
        };

        for printer in printers {
            if !printer.ip.trim().is_empty() && !printer.name.trim().is_empty() && !printer.model.trim().is_empty() {
                continue;
            }

            if let Some(discovered) = discover_printer(&printer).await {
                self.update_printer_discovery(&printer.id, &discovered);
            }
        }
    }

    async fn resolve_printer_for_connect(&self, printer: Printer) -> Result<Printer, Status> {
        if !printer.ip.trim().is_empty() {
            return Ok(printer);
        }

        let Some(discovered) = discover_printer(&printer).await else {
            return Err(Status::unavailable("Could not discover printer host"));
        };

        self.update_printer_discovery(&printer.id, &discovered);
        let mut resolved = printer;
        resolved.ip = discovered.ip.to_string();
        if resolved.name.trim().is_empty() {
            resolved.name = discovered
                .name
                .clone()
                .unwrap_or_else(|| format!("Bambu {}", resolved.id));
        }
        if resolved.model.trim().is_empty() {
            resolved.model = discovered.model.clone().unwrap_or_default();
        }
        Ok(resolved)
    }

    async fn connect_printer_session(
        &self,
        printer: &Printer,
        request: &Request<ConnectRequest>,
    ) -> Result<Response<ReceiverStream<Result<RecvMessage, Status>>>, Status> {
        let (response_tx, response_rx) = mpsc::channel(4);
        let dev_id = printer.id.clone();
        let options =
            CreateOptionsBuilder::new().server_uri(format!("mqtts://{}:8883", printer.ip));
        let mut client = match AsyncClient::new(options.finalize()) {
            Ok(c) => c,
            Err(e) => {
                error!("Failed to create MQTT client for {}: {}", dev_id, e);
                return Err(Status::internal("Failed to create MQTT client"));
            }
        };

        let (message_tx, mut message_rx) = tokio::sync::mpsc::channel(10);
        let report_topic = format!("device/{}/report", request.get_ref().dev_id);
        let request_topic = format!("device/{}/request", request.get_ref().dev_id);
        let callback_dev_id = dev_id.clone();
        let callback_report_topic = report_topic.clone();
        client.set_connected_callback(move |cli| {
            info!("MQTT connected for {}", callback_dev_id);
            let _ = cli.subscribe(&callback_report_topic, 0);
        });
        let lost_dev_id = dev_id.clone();
        client.set_connection_lost_callback(move |_| {
            warn!("MQTT connection lost for {}", lost_dev_id);
        });

        let connection_token = client.connect(Some(
            ConnectOptionsBuilder::new()
                .user_name("bblp")
                .password(&printer.password)
                .keep_alive_interval(MQTT_KEEP_ALIVE)
                .clean_session(false)
                .automatic_reconnect(MQTT_MIN_RECONNECT_DELAY, MQTT_MAX_RECONNECT_DELAY)
                .ssl_options(
                    SslOptionsBuilder::new()
                        .verify(false)
                        .enable_server_cert_auth(false)
                        .finalize(),
                )
                .finalize(),
        ));
        if let Err(e) = connection_token.await {
            error!("MQTT connect failed for {} at {}: {}", dev_id, printer.ip, e);
            return Err(Status::unavailable("Could not connect to printer"));
        }

        if let Err(e) = client.subscribe(&report_topic, 0).await {
            error!("MQTT subscribe failed for {}: {}", dev_id, e);
            return Err(Status::internal("Failed to subscribe to printer topic"));
        }

        let stream = client.get_stream(100);

        let connections = self.connections.clone();
        let stream_dev_id = dev_id.clone();
        spawn(async move {
            loop {
                match stream.recv().await {
                    Ok(Some(message)) => {
                        let send_result = response_tx
                            .send(Ok(RecvMessage {
                                connected: true,
                                dev_id: stream_dev_id.clone(),
                                data: message.payload_str().to_string(),
                            }))
                            .await;
                        if send_result.is_err() {
                            info!("gRPC stream closed for {}", stream_dev_id);
                            break;
                        }
                    }
                    _ => {
                        warn!("MQTT stream ended for {}; waiting for auto-reconnect", stream_dev_id);
                        sleep(Duration::from_secs(1)).await;
                        continue;
                    }
                }
            }
            connections.lock().unwrap().remove(&stream_dev_id);
        });

        self.connections
            .lock()
            .unwrap()
            .insert(request.get_ref().dev_id.clone(), message_tx);

        let publish_dev_id = request.get_ref().dev_id.clone();
        spawn(async move {
            loop {
                let recv_message = match message_rx.recv().await {
                    Some(m) => m,
                    None => {
                        info!("Send channel closed for {}", publish_dev_id);
                        break;
                    }
                };
                info!("Publishing to device/{}/request", publish_dev_id);
                if let Err(e) = client
                    .publish(Message::new(
                        request_topic.clone(),
                        recv_message.data,
                        0,
                    ))
                    .await
                {
                    warn!("MQTT publish failed for {}: {}", publish_dev_id, e);
                }
            }
        });

        Ok(Response::new(ReceiverStream::new(response_rx)))
    }

    async fn upload_temp_file(
        &self,
        printer: Printer,
        remote_path: String,
        total_bytes: usize,
        temp: NamedTempFile,
    ) -> Result<Response<UploadFileResponse>, Status> {
        let upload_handler = self.upload_handler.clone();
        match spawn_blocking(move || {
            let success = upload_handler(&printer, &remote_path, temp.path(), total_bytes)?;
            Ok(Response::new(UploadFileResponse { success }))
        })
        .await
        {
            Ok(result) => result,
            Err(_) => Err(Status::internal("Upload task panicked")),
        }
    }
}

#[tonic::async_trait]
impl BambuFarm for Farm {
    type GetAvailablePrintersStream = ReceiverStream<Result<PrinterOptionList, Status>>;

    async fn get_available_printers(
        &self,
        _request: Request<PrinterOptionRequest>,
    ) -> Result<Response<Self::GetAvailablePrintersStream>, Status> {
        let (tx, rx) = mpsc::channel(4);

        let farm = self.clone();
        tokio::spawn(async move {
            farm.refresh_discovery_metadata().await;
            loop {
                let printers = match farm.printers.lock() {
                    Ok(printers) => printers.clone(),
                    Err(_) => return,
                };
                let list = PrinterOptionList {
                    options: printers
                        .iter()
                        .map(|printer| PrinterOption {
                            dev_name: if printer.name.trim().is_empty() {
                                format!("Bambu {}", printer.id)
                            } else {
                                printer.name.clone()
                            },
                            dev_id: printer.id.clone(),
                            model: if printer.model.trim().is_empty() {
                                "unknown".to_string()
                            } else {
                                printer.model.clone()
                            },
                        })
                        .collect(),
                };
                if tx.send(Ok(list)).await.is_err() {
                    return;
                }
                sleep(Duration::from_secs(1)).await;
            }
        });

        Ok(Response::new(ReceiverStream::new(rx)))
    }

    type ConnectPrinterStream = ReceiverStream<Result<RecvMessage, Status>>;

    async fn connect_printer(
        &self,
        request: Request<ConnectRequest>,
    ) -> Result<Response<Self::ConnectPrinterStream>, Status> {
        let printer = self.lookup_printer(&request.get_ref().dev_id)?;
        let printer = self.resolve_printer_for_connect(printer).await?;

        match self.connect_printer_session(&printer, &request).await {
            Ok(response) => Ok(response),
            Err(connect_err) => {
                warn!(
                    "connect_printer: initial connect failed for printer={} host={} error={}; attempting host rediscovery",
                    printer.id, printer.ip, connect_err
                );
                let Some(discovered) = discover_printer(&printer).await else {
                    return Err(connect_err);
                };
                if discovered.ip.to_string() == printer.ip {
                    return Err(connect_err);
                }

                self.update_printer_discovery(&printer.id, &discovered);
                let mut rediscovered = printer;
                rediscovered.ip = discovered.ip.to_string();
                if rediscovered.name.trim().is_empty() {
                    if let Some(name) = discovered.name {
                        rediscovered.name = name;
                    }
                }
                if rediscovered.model.trim().is_empty() {
                    if let Some(model) = discovered.model {
                        rediscovered.model = model;
                    }
                }
                self.connect_printer_session(&rediscovered, &request).await
            }
        }
    }

    async fn send_message(
        &self,
        request: Request<SendMessageRequest>,
    ) -> Result<Response<SendMessageResponse>, Status> {
        let client = {
            let req = request.get_ref();
            let connections = self.connections.lock().unwrap();
            match connections.get(&req.dev_id).cloned() {
                Some(c) => c,
                None => {
                    warn!("send_message: no connection for dev_id={}", req.dev_id);
                    return Ok(Response::new(SendMessageResponse { success: false }));
                }
            }
        };
        let request = request.get_ref();
        match client.try_send(request.clone()) {
            Ok(_) => Ok(Response::new(SendMessageResponse { success: true })),
            Err(_) => Ok(Response::new(SendMessageResponse { success: false })),
        }
    }

    async fn send_control_message(
        &self,
        request: Request<ControlMessageRequest>,
    ) -> Result<Response<ControlMessageResponse>, Status> {
        let req = request.into_inner();
        let printer = self.lookup_printer(&req.dev_id)?;
        let data = spawn_blocking(move || handle_control_message(&printer, &req.data))
            .await
            .map_err(|_| Status::internal("Control task panicked"))?;
        Ok(Response::new(ControlMessageResponse { data }))
    }

    async fn upload_file(
        &self,
        request: Request<UploadFileRequest>,
    ) -> Result<Response<UploadFileResponse>, Status> {
        let req = request.into_inner();
        let printer = self.lookup_printer(&req.dev_id)?;
        let mut temp = NamedTempFile::new().map_err(|e| {
            error!("Failed to create temp file: {}", e);
            Status::internal("Failed to create temp file")
        })?;
        temp.write_all(&req.blob)
            .and_then(|_| temp.flush())
            .map_err(|e| {
                error!("Failed to write temp file: {}", e);
                Status::internal("Failed to write temp file")
            })?;
        self.upload_temp_file(printer, req.remote_path, req.blob.len(), temp)
            .await
    }

    async fn upload_file_stream(
        &self,
        request: Request<tonic::Streaming<UploadFileChunk>>,
    ) -> Result<Response<UploadFileResponse>, Status> {
        let mut stream = request.into_inner();
        let mut temp = NamedTempFile::new().map_err(|e| {
            error!("Failed to create temp file: {}", e);
            Status::internal("Failed to create temp file")
        })?;
        let mut dev_id: Option<String> = None;
        let mut remote_path: Option<String> = None;
        let mut total_bytes = 0usize;

        while let Some(chunk) = stream.message().await? {
            if dev_id.is_none() {
                dev_id = Some(chunk.dev_id.clone());
                remote_path = Some(chunk.remote_path.clone());
            } else if dev_id.as_deref() != Some(chunk.dev_id.as_str())
                || remote_path.as_deref() != Some(chunk.remote_path.as_str())
            {
                return Err(Status::invalid_argument("Upload stream metadata changed mid-stream"));
            }

            temp.write_all(&chunk.blob)
                .map_err(|e| {
                    error!("Failed to write upload chunk: {}", e);
                    Status::internal("Failed to write upload chunk")
                })?;
            total_bytes += chunk.blob.len();
        }

        let dev_id = dev_id.ok_or_else(|| Status::invalid_argument("Upload stream was empty"))?;
        let remote_path =
            remote_path.ok_or_else(|| Status::invalid_argument("Upload stream was empty"))?;
        temp.flush().map_err(|e| {
            error!("Failed to flush temp upload file: {}", e);
            Status::internal("Failed to flush temp upload file")
        })?;

        let printer = self.lookup_printer(&dev_id)?;
        self.upload_temp_file(printer, remote_path, total_bytes, temp)
            .await
    }
}

fn construct_printer(config: Value) -> Option<Printer> {
    let printer = config.into_table().unwrap_or_default();

    let dev_id = if let Some(dev_id) = printer.get("dev_id") {
        dev_id
    } else {
        eprintln!("Missing `dev_id` in printer config.");
        return None;
    };
    let model = if let Some(model) = printer.get("model") {
        match model.to_string().to_lowercase().as_str() {
            "x1c" => "3DPrinter-X1-Carbon",
            "x1" => "3DPrinter-X1",
            "p1p" => "C11",
            "p1s" => "C12",
            "a1mini" | "a1_mini" | "a1m" => "N1",
            "a1" => "N2S",
            _ => {
                eprintln!("Expected printer field `model` to be one of [`p1p`, `p1s`, `x1`, `x1c`, `a1mini`, `a1`].");
                return None;
            }
        }
    } else {
        ""
    };
    let host = printer
        .get("host")
        .map(|host| host.to_string())
        .unwrap_or_default();
    let password = if let Some(password) = printer.get("password") {
        password
    } else {
        eprintln!("Missing `password` in printer config.");
        return None;
    };
    let name = printer.get("name").map(ToString::to_string).unwrap_or_default();
    Some(Printer {
        name,
        id: dev_id.to_string(),
        ip: host,
        model: model.to_string(),
        password: password.to_string(),
    })
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::init();

    let config_file_path = if let Ok(dir) = current_dir() {
        dir.join("bambufarm.toml")
    } else {
        "bambufarm.toml".into()
    };

    let config = match Config::builder()
        .add_source(config::File::with_name(&config_file_path.to_string_lossy()).required(false))
        .add_source(config::Environment::with_prefix("BAMBU_FARM"))
        .build()
    {
        Ok(config) => config,
        Err(err) => {
            match err {
                ConfigError::NotFound(_) => {
                    error!(
                        "No config file found. Try adding one at `{}`",
                        config_file_path.to_string_lossy()
                    );
                }
                ConfigError::FileParse { uri, cause } => {
                    if let Some(uri) = uri {
                        error!("Error parsing config file at {}\nCause:{}", uri, cause);
                    } else {
                        error!("Error parsing config file.")
                    }
                }
                _ => {
                    error!("Unknown error parsing config file {:?}", err)
                }
            }
            return Ok(());
        }
    };
    let addr = config
        .get_string("endpoint")
        .unwrap_or("[::1]:47403".into());
    let addr = addr.parse().unwrap();

    let farm = Farm::default();
    for printer in config.get_array("printers").unwrap_or_default() {
        if let Some(printer) = construct_printer(printer) {
            farm.printers.lock().unwrap().push(printer);
        }
    }
    if farm.printers.lock().unwrap().is_empty() {
        error!(
            "No printers found, or printer config was invalid. Try adding some to the config file at `{}`",
            config_file_path.to_string_lossy()
        );
        return Ok(());
    }

    info!("BambuFarmServer listening on {}", addr);

    Server::builder()
        .add_service(
            BambuFarmServer::new(farm)
                .max_decoding_message_size(GRPC_MAX_MESSAGE_SIZE)
                .max_encoding_message_size(GRPC_MAX_MESSAGE_SIZE),
        )
        .serve(addr)
        .await?;

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use config::Map;
    use std::net::{SocketAddr, TcpListener};
    use std::time::{SystemTime, UNIX_EPOCH};
    use tokio::sync::oneshot;
    use tonic::Code;

    fn test_printer(dev_id: &str) -> Printer {
        Printer {
            name: "Test Printer".to_string(),
            id: dev_id.to_string(),
            ip: "127.0.0.1".to_string(),
            model: "N1".to_string(),
            password: "secret".to_string(),
        }
    }

    async fn spawn_test_server(
        farm: Farm,
    ) -> (
        SocketAddr,
        oneshot::Sender<()>,
        tokio::task::JoinHandle<Result<(), tonic::transport::Error>>,
    ) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        drop(listener);

        let (shutdown_tx, shutdown_rx) = oneshot::channel();
        let handle = tokio::spawn(async move {
            Server::builder()
                .add_service(
                    BambuFarmServer::new(farm)
                        .max_decoding_message_size(GRPC_MAX_MESSAGE_SIZE)
                        .max_encoding_message_size(GRPC_MAX_MESSAGE_SIZE),
                )
                .serve_with_shutdown(addr, async {
                    let _ = shutdown_rx.await;
                })
                .await
        });

        tokio::time::sleep(Duration::from_millis(50)).await;
        (addr, shutdown_tx, handle)
    }

    fn env_var(name: &str) -> Result<String, String> {
        std::env::var(name).map_err(|_| format!("missing required env var {}", name))
    }

    fn real_printer_from_env() -> Result<Printer, String> {
        if std::env::var("BAMBU_FARM_RUN_PRINTER_TESTS").as_deref() != Ok("1") {
            return Err(
                "set BAMBU_FARM_RUN_PRINTER_TESTS=1 to enable real printer integration tests"
                    .to_string(),
            );
        }

        Ok(Printer {
            name: std::env::var("BAMBU_FARM_TEST_PRINTER_NAME")
                .unwrap_or_else(|_| "Integration Test Printer".to_string()),
            id: env_var("BAMBU_FARM_TEST_PRINTER_DEV_ID")?,
            ip: env_var("BAMBU_FARM_TEST_PRINTER_HOST")?,
            model: std::env::var("BAMBU_FARM_TEST_PRINTER_MODEL")
                .unwrap_or_else(|_| "N1".to_string()),
            password: env_var("BAMBU_FARM_TEST_PRINTER_PASSWORD")?,
        })
    }

    fn unique_remote_name() -> String {
        let millis = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_millis();
        format!("codex-integration-{}.txt", millis)
    }

    fn test_printer_config(host: Option<&str>) -> Value {
        let mut printer = Map::new();
        printer.insert("dev_id".to_string(), Value::from("TESTDEVICE123456"));
        printer.insert("password".to_string(), Value::from("secret"));
        if let Some(host) = host {
            printer.insert("host".to_string(), Value::from(host));
        }
        Value::from(printer)
    }

    fn delete_remote_path_best_effort(printer: &Printer, remote_path: &str) {
        match connect_rust_ftps(printer) {
            Ok(mut ftp_stream) => {
                let _ = ftp_stream.rm(remote_path);
                let _ = ftp_stream.quit();
            }
            Err(err) => {
                warn!(
                    "real printer test cleanup via rust FTPS failed for printer={} remote_path={}: {}",
                    printer.ip, remote_path, err
                );
                let auth = format!("bblp:{}", printer.password);
                let base_url = format!("ftps://{}/", printer.ip);
                let _ = Command::new("curl")
                    .args([
                        "--fail",
                        "--silent",
                        "--show-error",
                        "--ssl-reqd",
                        "--insecure",
                        "--user",
                        &auth,
                        &base_url,
                        "--quote",
                        &format!("DELE {}", remote_path),
                    ])
                    .output();
            }
        }
    }

    #[test]
    fn construct_printer_allows_missing_host() {
        let printer = construct_printer(test_printer_config(None)).expect("printer config");

        assert_eq!(printer.name, "");
        assert_eq!(printer.id, "TESTDEVICE123456");
        assert_eq!(printer.model, "");
        assert_eq!(printer.password, "secret");
        assert!(printer.ip.is_empty());
    }

    #[test]
    fn construct_printer_preserves_explicit_host() {
        let printer =
            construct_printer(test_printer_config(Some("192.0.2.10"))).expect("printer config");

        assert_eq!(printer.ip, "192.0.2.10");
    }

    #[test]
    fn construct_printer_maps_explicit_model() {
        let mut config = test_printer_config(None).into_table().expect("printer table");
        config.insert("model".to_string(), Value::from("a1mini"));

        let printer = construct_printer(Value::from(config)).expect("printer config");

        assert_eq!(printer.model, "N1");
    }

    #[test]
    fn discovery_match_parses_product_name_and_model() {
        let matched = discovery_match_from_payload(
            Ipv4Addr::new(192, 168, 1, 247),
            r#"{
                "info": {
                    "module": [
                        {"visible": false, "product_name": ""},
                        {"visible": true, "product_name": "Bambu Lab A1 mini"}
                    ]
                }
            }"#,
        );

        assert_eq!(matched.ip, Ipv4Addr::new(192, 168, 1, 247));
        assert_eq!(matched.name.as_deref(), Some("Bambu Lab A1 mini"));
        assert_eq!(matched.model.as_deref(), Some("N1"));
    }

    #[tokio::test]
    async fn upload_file_stream_rpc_transfers_large_blob_to_handler() {
        let captured = Arc::new(Mutex::new(Vec::<(String, Vec<u8>, usize)>::new()));
        let captured_for_handler = captured.clone();
        let upload_handler: UploadHandler = Arc::new(move |_printer, remote_path, local_path, total_bytes| {
            captured_for_handler
                .lock()
                .unwrap()
                .push((
                    remote_path.to_string(),
                    std::fs::read(local_path).unwrap(),
                    total_bytes,
                ));
            Ok(true)
        });

        let farm = Farm::with_upload_handler(vec![test_printer("printer-1")], upload_handler);
        let (addr, shutdown_tx, server_handle) = spawn_test_server(farm).await;

        let mut client = bambu_farm::bambu_farm_client::BambuFarmClient::connect(format!(
            "http://{}",
            addr
        ))
        .await
        .unwrap()
        .max_decoding_message_size(GRPC_MAX_MESSAGE_SIZE)
        .max_encoding_message_size(GRPC_MAX_MESSAGE_SIZE);

        let blob = vec![b'x'; 5 * 1024 * 1024];
        let chunks = tokio_stream::iter(vec![
            UploadFileChunk {
                dev_id: "printer-1".to_string(),
                blob: blob[..(2 * 1024 * 1024)].to_vec(),
                remote_path: "integration-job.gcode.3mf".to_string(),
            },
            UploadFileChunk {
                dev_id: "printer-1".to_string(),
                blob: blob[(2 * 1024 * 1024)..].to_vec(),
                remote_path: "integration-job.gcode.3mf".to_string(),
            },
        ]);
        let response = client
            .upload_file_stream(chunks)
            .await
            .unwrap()
            .into_inner();

        assert!(response.success);

        let uploads = captured.lock().unwrap();
        assert_eq!(uploads.len(), 1);
        assert_eq!(uploads[0].0, "integration-job.gcode.3mf");
        assert_eq!(uploads[0].1, blob);
        assert_eq!(uploads[0].2, 5 * 1024 * 1024);
        drop(uploads);

        let _ = shutdown_tx.send(());
        server_handle.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn upload_file_stream_rpc_rejects_unknown_printer() {
        let upload_handler: UploadHandler = Arc::new(|_, _, _, _| Ok(true));
        let farm = Farm::with_upload_handler(vec![test_printer("printer-1")], upload_handler);
        let (addr, shutdown_tx, server_handle) = spawn_test_server(farm).await;

        let mut client = bambu_farm::bambu_farm_client::BambuFarmClient::connect(format!(
            "http://{}",
            addr
        ))
        .await
        .unwrap()
        .max_decoding_message_size(GRPC_MAX_MESSAGE_SIZE)
        .max_encoding_message_size(GRPC_MAX_MESSAGE_SIZE);

        let error = client
            .upload_file_stream(tokio_stream::iter(vec![UploadFileChunk {
                dev_id: "missing-printer".to_string(),
                blob: vec![1, 2, 3],
                remote_path: "missing.gcode.3mf".to_string(),
            }]))
            .await
            .unwrap_err();

        assert_eq!(error.code(), Code::Unknown);

        let _ = shutdown_tx.send(());
        server_handle.await.unwrap().unwrap();
    }

    #[tokio::test]
    #[ignore = "requires explicit real-printer environment variables"]
    async fn upload_file_stream_rpc_reaches_real_printer() {
        let printer = real_printer_from_env().unwrap();
        let remote_name = unique_remote_name();
        let upload_handler: UploadHandler = Arc::new(default_upload_handler);
        let farm = Farm::with_upload_handler(vec![printer.clone()], upload_handler);
        let (addr, shutdown_tx, server_handle) = spawn_test_server(farm).await;

        let mut client = bambu_farm::bambu_farm_client::BambuFarmClient::connect(format!(
            "http://{}",
            addr
        ))
        .await
        .unwrap()
        .max_decoding_message_size(GRPC_MAX_MESSAGE_SIZE)
        .max_encoding_message_size(GRPC_MAX_MESSAGE_SIZE);

        let payload = format!(
            "codex integration upload {}\n",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs()
        )
        .into_bytes();
        let chunks = tokio_stream::iter(vec![UploadFileChunk {
            dev_id: printer.id.clone(),
            blob: payload,
            remote_path: remote_name.clone(),
        }]);

        let response = client
            .upload_file_stream(chunks)
            .await
            .unwrap()
            .into_inner();

        assert!(response.success);

        delete_remote_path_best_effort(&printer, &remote_name);

        let _ = shutdown_tx.send(());
        server_handle.await.unwrap().unwrap();
    }
}
