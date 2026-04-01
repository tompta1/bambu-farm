use bambu_farm::PrinterOption;
use bambu_farm::{
    bambu_farm_server::BambuFarm, ConnectRequest, PrinterOptionList, PrinterOptionRequest,
    RecvMessage, SendMessageRequest, SendMessageResponse, UploadFileChunk, UploadFileRequest,
    UploadFileResponse,
};
use config::{Config, ConfigError, Value};
use log::{error, info, warn};
use paho_mqtt::{
    AsyncClient, ConnectOptionsBuilder, CreateOptionsBuilder, Message, SslOptionsBuilder,
};
use suppaftp::native_tls::TlsConnector;
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

        let printers = self.printers.clone();
        tokio::spawn(async move {
            loop {
                let printers = match printers.lock() {
                    Ok(printers) => printers.clone(),
                    Err(_) => return,
                };
                let list = PrinterOptionList {
                    options: printers
                        .iter()
                        .map(|printer| PrinterOption {
                            dev_name: printer.name.clone(),
                            dev_id: printer.id.clone(),
                            model: printer.model.clone(),
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
        let (response_tx, response_rx) = mpsc::channel(4);

        let printers = match self.printers.lock() {
            Ok(printers) => printers.clone(),
            Err(_) => return Err(Status::unknown("Could not lock printer list.")),
        };
        let printer = printers
            .iter()
            .filter(|printer| printer.id == request.get_ref().dev_id)
            .next();
        if let Some(printer) = printer {
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

            let connection_token = client.connect(Some(
                ConnectOptionsBuilder::new()
                    .user_name("bblp")
                    .password(&printer.password)
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

            if let Err(e) = client
                .subscribe(format!("device/{}/report", request.get_ref().dev_id), 0)
                .await
            {
                error!("MQTT subscribe failed for {}: {}", dev_id, e);
                return Err(Status::internal("Failed to subscribe to printer topic"));
            }

            let stream = client.get_stream(100);

            let connections = self.connections.clone();
            spawn(async move {
                loop {
                    match stream.recv().await {
                        Ok(Some(message)) => {
                            let send_result = response_tx
                                .send(Ok(RecvMessage {
                                    connected: true,
                                    dev_id: dev_id.clone(),
                                    data: message.payload_str().to_string(),
                                }))
                                .await;
                            if send_result.is_err() {
                                info!("gRPC stream closed for {}", dev_id);
                                break;
                            }
                        }
                        _ => {
                            warn!("MQTT stream ended for {}", dev_id);
                            break;
                        }
                    }
                }
                connections.lock().unwrap().remove(&dev_id);
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
                            format!("device/{}/request", publish_dev_id),
                            recv_message.data,
                            0,
                        ))
                        .await
                    {
                        warn!("MQTT publish failed for {}: {}", publish_dev_id, e);
                    }
                }
            });
        } else {
            return Err(Status::unknown("No matching printer."));
        }

        Ok(Response::new(ReceiverStream::new(response_rx)))
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
        eprintln!("Missing `model` in printer config.");
        return None;
    };
    let host = if let Some(host) = printer.get("host") {
        host
    } else {
        eprintln!("Missing `host` in printer config.");
        return None;
    };
    let password = if let Some(password) = printer.get("password") {
        password
    } else {
        eprintln!("Missing `password` in printer config.");
        return None;
    };
    let name = if let Some(name) = printer.get("name") {
        name
    } else {
        eprintln!("Missing `name` in printer config.");
        return None;
    };
    Some(Printer {
        name: name.to_string(),
        id: dev_id.to_string(),
        ip: host.to_string(),
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
