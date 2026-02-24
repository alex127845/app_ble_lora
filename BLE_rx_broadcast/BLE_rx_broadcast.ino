/**
 * ════════════════════════════════════════════════════════════════
 * 📡 File Transfer System RX BROADCAST - Heltec WiFi LoRa 32 V3 (ESP32-S3)
 * ════════════════════════════════════════════════════════════════
 * 
 * Sistema de recepción de archivos vía BLE + LoRa BROADCAST (sin ACK)
 * 
 * MODO: RECEPTOR (RX) - DATACASTING PURO
 * 
 * Características:
 * - LittleFS para almacenamiento persistente
 * - BLE para monitoreo desde Android
 * - LoRa BROADCAST (sin ACK) con carrusel
 * - FEC (Forward Error Correction) para recuperación
 * - Deinterleaving para reordenar chunks
 * - CRC16 para validación de integridad
 * - Reconstrucción de archivos con chunks perdidos
 * - Detección de múltiples vueltas
 * 
 * @author alex127845
 * @date 2025-01-31
 * @version 4.0 RX BROADCAST
 */

#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN - PINES HELTEC V3
// ════════════════════════════════════════════════════════════════

#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14
#define VEXT 36
#define VEXT_ON LOW

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN - BLE
// ════════════════════════════════════════════════════════════════

#define DEVICE_NAME "Heltec-RX-Broadcast"
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_WRITE_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DATA_READ_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define PROGRESS_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26aa"

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN - PROTOCOLO BROADCAST
// ════════════════════════════════════════════════════════════════

#define CHUNK_SIZE_BLE 200              // Chunks para BLE (bytes)
#define CHUNK_SIZE_LORA 240             // Chunks para LoRa (bytes)
#define MAX_FILENAME_LENGTH 64
#define RX_TIMEOUT 60000                // Timeout entre vueltas (60s)
#define FEC_BLOCK_SIZE 8                // Bloques FEC
#define MAX_CHUNKS 512                  // Máximo chunks (120KB archivo)

// Magic bytes para protocolo
#define MANIFEST_MAGIC_1 0xAA
#define MANIFEST_MAGIC_2 0xBB
#define DATA_MAGIC_1 0xCC
#define DATA_MAGIC_2 0xDD
#define PARITY_MAGIC_1 0xEE
#define PARITY_MAGIC_2 0xFF
#define FILE_END_MAGIC_1 0x99
#define FILE_END_MAGIC_2 0x88

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - RADIO LORA
// ════════════════════════════════════════════════════════════════

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Parámetros LoRa configurables
float currentBW = 125.0;           // Bandwidth en kHz
int currentSF = 9;                 // Spreading Factor
int currentCR = 7;                 // Coding Rate (4/7)
int currentREPEAT = 2;             // Repeticiones esperadas (informativo)
int currentPower = 17;             // Potencia en dBm

// Estado de recepción LoRa BROADCAST
volatile bool packetReceived = false;
bool receivingFile = false;
uint32_t currentFileID = 0;
String receivingFileName = "";
uint32_t receivingFileSize = 0;
uint16_t receivingChunkSize = 0;
uint16_t totalChunks = 0;
unsigned long lastPacketTime = 0;
unsigned long receptionStartTime = 0;

// Buffers de recepción
uint8_t* chunkBuffer[MAX_CHUNKS];           // Buffer de chunks recibidos
bool chunkReceived[MAX_CHUNKS];             // Flags de chunks recibidos
uint16_t chunkLengths[MAX_CHUNKS];          // Longitudes de cada chunk

// FEC - Parity chunks
uint8_t* parityBuffer[MAX_CHUNKS / FEC_BLOCK_SIZE];
bool parityReceived[MAX_CHUNKS / FEC_BLOCK_SIZE];
uint16_t parityLengths[MAX_CHUNKS / FEC_BLOCK_SIZE];

// Estadísticas
uint16_t receivedDataChunks = 0;
uint16_t receivedParityChunks = 0;
uint16_t manifestCount = 0;
uint16_t duplicateChunks = 0;
int16_t avgRSSI = 0;
float avgSNR = 0;
int rssiCount = 0;

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - BLE
// ════════════════════════════════════════════════════════════════

BLEServer* pServer = NULL;
BLECharacteristic* pCmdCharacteristic = NULL;
BLECharacteristic* pDataCharacteristic = NULL;
BLECharacteristic* pProgressCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - ESTADO DE TRANSFERENCIA BLE
// ════════════════════════════════════════════════════════════════

enum TransferState {
  STATE_IDLE,
  STATE_DOWNLOADING
};

TransferState currentState = STATE_IDLE;
String currentFilename = "";
File currentFile;
uint32_t expectedFileSize = 0;
uint32_t transferredBytes = 0;

// ════════════════════════════════════════════════════════════════
// 📝 DECLARACIÓN DE FUNCIONES
// ══��═════════════════════════════════════════════════════════════

// Setup
void setupLittleFS();
void setupBLE();
void setupLoRa();
void applyLoRaConfig();
void enableVext(bool on);

// BLE - Comandos
void handleCommand(String command);
void sendResponse(String response);
void sendProgress(uint8_t percentage);

// BLE - Gestión de archivos
void listFiles();
void deleteFile(String filename);
void startDownload(String filename);
void sendFileInChunks(String filename);

// LoRa - Configuración
void setLoRaConfig(String jsonStr);
void sendCurrentLoRaConfig();

// LoRa - Recepción BROADCAST
void processLoRaPacket();
void handleManifest(uint8_t* data, size_t len);
void handleDataChunk(uint8_t* data, size_t len);
void handleParityChunk(uint8_t* data, size_t len);
void handleFileEnd(uint8_t* data, size_t len);
void assembleFile();
void recoverMissingChunks();
void cancelReception(String reason);
void resetReceptionBuffers();

// Utilidades
String encodeBase64(uint8_t* data, size_t length);
size_t decodeBase64(String input, uint8_t* output, size_t maxLen);
void resetTransferState();
uint16_t crc16_ccitt(const uint8_t* data, size_t len);

// ════════════════════════════════════════════════════════════════
// 🔐 CRC16-CCITT
// ════════════════════════════════════════════════════════════════

uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc = crc << 1;
    }
  }
  return crc;
}

// ════════════════════════════════════════════════════════════════
// 🔌 BLE CALLBACKS
// ════════════════════════════════════════════════════════════════

class ServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("\n✅ Cliente BLE conectado");
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("\n❌ Cliente BLE desconectado");
    
    // Limpiar estado de transferencia BLE
    if (currentState != STATE_IDLE) {
      Serial.println("⚠️  Transferencia BLE interrumpida, limpiando...");
      if (currentFile) currentFile.close();
      resetTransferState();
    }
  }
};

class CmdCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    uint8_t* pData = pCharacteristic->getData();
    size_t len = pCharacteristic->getValue().length();
    
    if (len > 0 && pData != nullptr) {
      String command = "";
      for (size_t i = 0; i < len; i++) {
        command += (char)pData[i];
      }
      command.trim();
      
      if (command.length() > 0) {
        Serial.println("\n📩 Comando BLE recibido: " + command);
        handleCommand(command);
      }
    }
  }
};

// ════════════════════════════════════════════════════════════════
// 📡 LORA CALLBACK - Interrupción DIO1
// ════════════════════════════════════════════════════════════════

void IRAM_ATTR setFlag(void) {
  packetReceived = true;
}

// ════════════════════════════════════════════════════════════════
// 🚀 SETUP
// ════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n");
  Serial.println("════════════════════════════════════════════════");
  Serial.println("  📡 File Transfer System v4.0 RX BROADCAST");
  Serial.println("  Heltec WiFi LoRa 32 V3");
  Serial.println("  MODO: RECEPTOR BROADCAST (SIN ACK)");
  Serial.println("════════════════════════════════════════════════");
  Serial.println();
  
  setupLittleFS();
  setupBLE();
  
  delay(1000);
  
  setupLoRa();
  
  Serial.println("\n✅ Sistema RX BROADCAST listo");
  Serial.println("👂 Esperando conexión BLE...");
  Serial.println("📡 Radio LoRa en modo escucha continua\n");
}

// ════════════════════════════════════════════════════════════════
// 🔁 LOOP
// ════════════════════════════════════════════════════════════════

void loop() {
  // Manejar reconexión BLE
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("🔄 Esperando reconexión BLE...");
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
  
  // Procesar paquetes LoRa recibidos
  if (packetReceived) {
    packetReceived = false;
    processLoRaPacket();
  }
  
  // Timeout de recepción (entre vueltas)
  if (receivingFile) {
    if (millis() - lastPacketTime > RX_TIMEOUT) {
      Serial.println("\n⏱️  Timeout - Finalizando recepción con datos actuales");
      assembleFile();
    }
  }
  
  yield();
  delay(10);
}

// ════════════════════════════════════════════════════════════════
// 💾 LITTLEFS - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════

void setupLittleFS() {
  Serial.println("💾 Inicializando LittleFS...");
  
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while(1) delay(1000);
  }
  
  Serial.println("✅ LittleFS montado");
  
  uint32_t totalBytes = LittleFS.totalBytes();
  uint32_t usedBytes = LittleFS.usedBytes();
  
  Serial.printf("   Total: %.2f MB\n", totalBytes / 1048576.0);
  Serial.printf("   Usado: %.2f MB\n", usedBytes / 1048576.0);
  Serial.printf("   Libre: %.2f MB\n", (totalBytes - usedBytes) / 1048576.0);
  
  // Listar archivos
  Serial.println("\n📁 Archivos:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int count = 0;
  
  while (file) {
    if (!file.isDirectory()) {
      Serial.printf("   - %s (%.2f KB)\n", file.name(), file.size() / 1024.0);
      count++;
    }
    file = root.openNextFile();
  }
  
  if (count == 0) Serial.println("   (vacío)");
}

// ════════════════════════════════════════════════════════════════
// 📡 BLE - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════

void setupBLE() {
  Serial.println("\n📡 Inicializando BLE...");
  
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(517);
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // Característica CMD (WRITE)
  pCmdCharacteristic = pService->createCharacteristic(
    CMD_WRITE_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCmdCharacteristic->setCallbacks(new CmdCallbacks());
  
  // Característica DATA (READ/NOTIFY)
  pDataCharacteristic = pService->createCharacteristic(
    DATA_READ_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pDataCharacteristic->addDescriptor(new BLE2902());
  
  // Característica PROGRESS (NOTIFY)
  pProgressCharacteristic = pService->createCharacteristic(
    PROGRESS_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pProgressCharacteristic->addDescriptor(new BLE2902());
  
  pService->start();
  
  Serial.println("✅ Servicio BLE creado: " + String(SERVICE_UUID));
  Serial.println("   - CMD_WRITE: " + String(CMD_WRITE_UUID));
  Serial.println("   - DATA_READ: " + String(DATA_READ_UUID));
  Serial.println("   - PROGRESS: " + String(PROGRESS_UUID));
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  
  Serial.println("✅ BLE iniciado: " + String(DEVICE_NAME));
  Serial.println("✅ Advertising con UUID: " + String(SERVICE_UUID));
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════

void enableVext(bool on) {
  pinMode(VEXT, OUTPUT);
  digitalWrite(VEXT, on ? VEXT_ON : !VEXT_ON);
}

void setupLoRa() {
  Serial.println("\n📡 Inicializando radio LoRa...");
  
  enableVext(true);
  delay(200);
  
  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262, código: %d\n", state);
    Serial.println("⚠️ Continuando sin LoRa...");
    return;
  }
  
  Serial.println("✅ SX1262 inicializado");
  
  applyLoRaConfig();
  
  // Configurar interrupción
  radio.setDio1Action(setFlag);
  
  // Iniciar recepción continua
  state = radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("📻 Radio en modo RX continuo BROADCAST");
  } else {
    Serial.printf("❌ Error iniciando RX: %d\n", state);
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA - APLICAR CONFIGURACIÓN
// ════════════════════════════════════════════════════════════════

void applyLoRaConfig() {
  Serial.println("\n📻 Aplicando configuración LoRa BROADCAST...");
  
  radio.standby();
  delay(100);
  
  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(currentPower);
  
  delay(100);
  
  Serial.println("📻 Configuración LoRa RX:");
  Serial.printf("   BW: %.0f kHz\n", currentBW);
  Serial.printf("   SF: %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.printf("   Power: %d dBm\n", currentPower);
  Serial.printf("   Repeticiones esperadas: %d vueltas\n", currentREPEAT);
  Serial.println("✅ Configuración aplicada (RX ONLY - SIN ACK)\n");
}

// ════════════════════════════════════════════════════════════════
// 🎯 MANEJO DE COMANDOS BLE
// ════════════════════════════════════════════════════════════════

void handleCommand(String command) {
  command.trim();
  
  // Comando: LIST
  if (command == "CMD:LIST") {
    Serial.println("📋 Procesando: LIST");
    listFiles();
  }
  
  // Comando: DELETE:filename
  else if (command.startsWith("CMD:DELETE:")) {
    String filename = command.substring(11);
    Serial.println("🗑️  Procesando: DELETE - " + filename);
    deleteFile(filename);
  }
  
  // Comando: DOWNLOAD:filename
  else if (command.startsWith("CMD:DOWNLOAD:")) {
    String filename = command.substring(13);
    Serial.println("📥 Procesando: DOWNLOAD - " + filename);
    startDownload(filename);
  }
  
  // Comando: SET_LORA_CONFIG
  else if (command.startsWith("CMD:SET_LORA_CONFIG:")) {
    String jsonStr = command.substring(20);
    Serial.println("⚙️  Procesando: SET_LORA_CONFIG");
    setLoRaConfig(jsonStr);
  }
  
  // Comando: GET_LORA_CONFIG
  else if (command == "CMD:GET_LORA_CONFIG") {
    Serial.println("⚙️  Procesando: GET_LORA_CONFIG");
    sendCurrentLoRaConfig();
  }
  
  // Comando: PING
  else if (command == "CMD:PING") {
    Serial.println("🏓 Procesando: PING");
    sendResponse("PONG");
  }
  
  // Comando desconocido
  else {
    Serial.println("⚠️  Comando desconocido: " + command);
    sendResponse("ERROR:UNKNOWN_COMMAND");
  }
}

// ════════════════════════════════════════════════════════════════
// 📤 ENVIAR RESPUESTA BLE
// ════════════════════════════════════════════════════════════════

void sendResponse(String response) {
  if (!deviceConnected || pDataCharacteristic == NULL) return;
  
  response += "\n";
  pDataCharacteristic->setValue(response.c_str());
  pDataCharacteristic->notify();
  delay(10);
}

// ════════════════════════════════════════════════════════════════
// 📊 ENVIAR PROGRESO BLE
// ════════════════════════════════════════════════════════════════

void sendProgress(uint8_t percentage) {
  if (!deviceConnected || pProgressCharacteristic == NULL) return;
  
  uint8_t data[1] = { percentage };
  pProgressCharacteristic->setValue(data, 1);
  pProgressCharacteristic->notify();
  delay(5);
}

// ════════════════════════════════════════════════════════════════
// 📋 LISTAR ARCHIVOS
// ════════════════════════════════════════════════════════════════

void listFiles() {
  sendResponse("FILES_START");
  
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int count = 0;
  
  while (file) {
    if (!file.isDirectory()) {
      String filename = String(file.name());
      if (filename.startsWith("/")) filename = filename.substring(1);
      
      String fileInfo = "FILE:" + filename + ":" + String(file.size());
      sendResponse(fileInfo);
      count++;
    }
    file = root.openNextFile();
  }
  
  sendResponse("FILES_END:" + String(count));
  Serial.printf("✅ Lista enviada: %d archivo(s)\n", count);
}

// ════════════════════════════════════════════════════════════════
// 🗑️  ELIMINAR ARCHIVO
// ════════════════════════════════════════════════════════════════

void deleteFile(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (!LittleFS.exists(filename)) {
    sendResponse("ERROR:FILE_NOT_FOUND");
    return;
  }
  
  if (receivingFile && receivingFileName == filename) {
    sendResponse("ERROR:FILE_IN_USE");
    return;
  }
  
  if (currentState != STATE_IDLE && currentFilename == filename) {
    sendResponse("ERROR:FILE_IN_USE");
    return;
  }
  
  delay(100);
  
  if (LittleFS.remove(filename)) {
    Serial.println("✅ Eliminado: " + filename);
    sendResponse("OK:DELETED");
  } else {
    Serial.println("❌ Error eliminando");
    sendResponse("ERROR:CANT_DELETE");
  }
}

// ════════════════════════════════════════════════════════════════
// 📥 DOWNLOAD BLE - INICIAR
// ════════════════════════════════════════════════════════════════

void startDownload(String filename) {
  if (currentState != STATE_IDLE) {
    sendResponse("ERROR:TRANSFER_IN_PROGRESS");
    return;
  }
  
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (!LittleFS.exists(filename)) {
    sendResponse("ERROR:FILE_NOT_FOUND");
    return;
  }
  
  File file = LittleFS.open(filename, "r");
  if (!file) {
    sendResponse("ERROR:OPEN_FAILED");
    return;
  }
  
  uint32_t fileSize = file.size();
  file.close();
  
  currentState = STATE_DOWNLOADING;
  currentFilename = filename;
  expectedFileSize = fileSize;
  transferredBytes = 0;
  
  String cleanName = filename;
  if (cleanName.startsWith("/")) cleanName = cleanName.substring(1);
  
  sendResponse("DOWNLOAD_START:" + cleanName + ":" + String(fileSize));
  delay(100);
  
  sendFileInChunks(filename);
}

// ════════════════════════════════════════════════════════════════
// 📤 DOWNLOAD BLE - ENVIAR EN CHUNKS
// ════════════════════════════════════════════════════════════════

void sendFileInChunks(String filename) {
  File file = LittleFS.open(filename, "r");
  if (!file) {
    sendResponse("ERROR:FILE_OPEN_FAILED");
    resetTransferState();
    return;
  }
  
  uint32_t totalSize = file.size();
  uint16_t totalChunks = (totalSize + CHUNK_SIZE_BLE - 1) / CHUNK_SIZE_BLE;
  uint16_t chunkNum = 0;
  
  sendProgress(0);
  
  uint8_t buffer[CHUNK_SIZE_BLE];
  
  while (file.available()) {
    size_t bytesRead = file.read(buffer, CHUNK_SIZE_BLE);
    if (bytesRead == 0) break;
    
    String encoded = encodeBase64(buffer, bytesRead);
    String chunkMsg = "CHUNK:" + String(chunkNum) + ":" + encoded;
    sendResponse(chunkMsg);
    
    transferredBytes += bytesRead;
    chunkNum++;
    
    uint8_t progress = (transferredBytes * 100) / totalSize;
    if (chunkNum % 5 == 0 || chunkNum >= totalChunks) {
      sendProgress(progress);
    }
    
    delay(20);
  }
  
  file.close();
  
  sendResponse("DOWNLOAD_END:" + String(transferredBytes));
  sendProgress(100);
  
  Serial.printf("✅ Download BLE completo: %u bytes\n", transferredBytes);
  
  resetTransferState();
}

// ════════════════════════════════════════════════════════════════
// ⚙️  CONFIGURACIÓN LORA - SET
// ════════════════════════════════════════════════════════════════

void setLoRaConfig(String jsonStr) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    sendResponse("ERROR:INVALID_JSON");
    return;
  }
  
  if (receivingFile) {
    sendResponse("ERROR:RECEIVING");
    return;
  }
  
  currentBW = doc["bw"];
  currentSF = doc["sf"];
  currentCR = doc["cr"];
  currentREPEAT = doc["repeat"];  // ✅ Nuevo: repeticiones esperadas
  currentPower = doc["power"];
  
  applyLoRaConfig();
  
  // Reiniciar recepción
  radio.setDio1Action(setFlag);
  radio.startReceive();
  
  sendResponse("OK:LORA_CONFIG_SET");
  Serial.println("✅ Configuración LoRa actualizada");
}

// ════════════════════════════════════════════════════════════════
// ⚙️  CONFIGURACIÓN LORA - GET
// ════════════════════════════════════════════════════════════════

void sendCurrentLoRaConfig() {
  StaticJsonDocument<200> doc;
  doc["bw"] = (int)currentBW;
  doc["sf"] = currentSF;
  doc["cr"] = currentCR;
  doc["repeat"] = currentREPEAT;  // ✅ Nuevo: repeticiones
  doc["power"] = currentPower;
  
  String json;
  serializeJson(doc, json);
  
  sendResponse("LORA_CONFIG:" + json);
  Serial.println("✅ Config LoRa enviada");
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - PROCESAR PAQUETE BROADCAST
// ════════════════════════════════════════════════════════════════

void processLoRaPacket() {
  uint8_t buffer[300];
  
  int state = radio.readData(buffer, sizeof(buffer));
  
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  Error leyendo paquete: %d\n", state);
    radio.startReceive();
    return;
  }
  
  size_t len = radio.getPacketLength();
  
  // Obtener RSSI y SNR
  int16_t rssi = radio.getRSSI();
  float snr = radio.getSNR();
  
  // Actualizar estadísticas
  avgRSSI = (avgRSSI * rssiCount + rssi) / (rssiCount + 1);
  avgSNR = (avgSNR * rssiCount + snr) / (rssiCount + 1);
  rssiCount++;
  
  lastPacketTime = millis();
  
  // ✅ Detectar tipo de paquete por magic bytes
  
  if (len >= 10 && buffer[0] == MANIFEST_MAGIC_1 && buffer[1] == MANIFEST_MAGIC_2) {
    handleManifest(buffer, len);
  }
  else if (len >= 10 && buffer[0] == DATA_MAGIC_1 && buffer[1] == DATA_MAGIC_2) {
    handleDataChunk(buffer, len);
  }
  else if (len >= 8 && buffer[0] == PARITY_MAGIC_1 && buffer[1] == PARITY_MAGIC_2) {
    handleParityChunk(buffer, len);
  }
  else if (len >= 8 && buffer[0] == FILE_END_MAGIC_1 && buffer[1] == FILE_END_MAGIC_2) {
    handleFileEnd(buffer, len);
  }
  
  // Reiniciar recepción
  radio.startReceive();
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - MANEJAR MANIFEST
// ══════════���═════════════════════════════════════════════════════

void handleManifest(uint8_t* data, size_t len) {
  // Validar CRC
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + len - 2, 2);
  uint16_t calculatedCRC = crc16_ccitt(data, len - 2);
  
  if (receivedCRC != calculatedCRC) {
    Serial.println("⚠️  Manifest CRC inválido");
    return;
  }
  
  // Extraer datos
  uint32_t fileID;
  uint32_t fileSize;
  uint16_t totalChunksRx;
  uint16_t chunkSize;
  uint8_t nameLen;
  
  size_t idx = 2;  // Saltar magic bytes
  memcpy(&fileID, data + idx, 4); idx += 4;
  memcpy(&fileSize, data + idx, 4); idx += 4;
  memcpy(&totalChunksRx, data + idx, 2); idx += 2;
  memcpy(&chunkSize, data + idx, 2); idx += 2;
  nameLen = data[idx++];
  
  if (nameLen > 100) nameLen = 100;
  
  char fileName[101];
  memcpy(fileName, data + idx, nameLen);
  fileName[nameLen] = '\0';
  
  manifestCount++;
  
  // Si es un nuevo archivo, inicializar
  if (!receivingFile || currentFileID != fileID) {
    if (receivingFile) {
      Serial.println("\n⚠️  Nuevo archivo detectado, finalizando anterior");
      assembleFile();
    }
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.printf("║  📦 NUEVO MANIFEST (ID: 0x%08X)    \n", fileID);
    Serial.println("╚════════════════════════════════════════╝");
    Serial.printf("📄 Archivo: %s\n", fileName);
    Serial.printf("📊 Tamaño: %u bytes (%.2f KB)\n", fileSize, fileSize/1024.0);
    Serial.printf("📦 Chunks: %u\n", totalChunksRx);
    Serial.printf("📶 RSSI: %d dBm, SNR: %.2f dB\n", radio.getRSSI(), radio.getSNR());
    
    // Verificar espacio
    uint32_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (fileSize > freeSpace) {
      Serial.printf("❌ Espacio insuficiente (libre: %u, necesario: %u)\n", 
                   freeSpace, fileSize);
      sendResponse("RX_FAILED:NO_SPACE");
      return;
    }
    
    // Inicializar recepción
    currentFileID = fileID;
    receivingFileName = "/" + String(fileName);
    receivingFileSize = fileSize;
    receivingChunkSize = chunkSize;
    totalChunks = totalChunksRx;
    receivingFile = true;
    receptionStartTime = millis();
    
    receivedDataChunks = 0;
    receivedParityChunks = 0;
    duplicateChunks = 0;
    rssiCount = 0;
    
    resetReceptionBuffers();
    
    Serial.printf("✅ Listo para recibir (Manifest #%u)\n\n", manifestCount);
    
    String cleanName = String(fileName);
    sendResponse("RX_START:" + cleanName + ":" + String(fileSize));
    sendProgress(0);
  } else {
    // Manifest duplicado (otra vuelta del carrusel)
    Serial.printf("🔁 Manifest duplicado (vuelta %u)\n", manifestCount);
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - MANEJAR DATA CHUNK
// ════════════════════════════════════════════════════════════════

void handleDataChunk(uint8_t* data, size_t len) {
  if (!receivingFile) return;
  
  // Validar CRC
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + len - 2, 2);
  uint16_t calculatedCRC = crc16_ccitt(data, len - 2);
  
  if (receivedCRC != calculatedCRC) {
    Serial.println("⚠️  Data CRC inválido");
    return;
  }
  
  // Extraer datos
  uint32_t fileID;
  uint16_t chunkIndex;
  uint16_t totalChunksRx;
  
  size_t idx = 2;  // Saltar magic bytes
  memcpy(&fileID, data + idx, 4); idx += 4;
  memcpy(&chunkIndex, data + idx, 2); idx += 2;
  memcpy(&totalChunksRx, data + idx, 2); idx += 2;
  
  // Validar FileID
  if (fileID != currentFileID) {
    return;  // Chunk de otro archivo
  }
  
  // Validar índice
  if (chunkIndex >= MAX_CHUNKS || chunkIndex >= totalChunks) {
    Serial.printf("⚠️  Chunk index inválido: %u\n", chunkIndex);
    return;
  }
  
  // Ya recibido?
  if (chunkReceived[chunkIndex]) {
    duplicateChunks++;
    return;  // Duplicado
  }
  
  // Guardar chunk
  size_t dataLen = len - idx - 2;  // - 2 del CRC
  chunkBuffer[chunkIndex] = (uint8_t*)malloc(dataLen);
  if (chunkBuffer[chunkIndex] != nullptr) {
    memcpy(chunkBuffer[chunkIndex], data + idx, dataLen);
    chunkLengths[chunkIndex] = dataLen;
    chunkReceived[chunkIndex] = true;
    receivedDataChunks++;
    
    // Mostrar progreso cada 10 chunks
    if (receivedDataChunks % 10 == 0) {
      uint8_t progress = (receivedDataChunks * 100) / totalChunks;
      Serial.printf("📦 Chunk %u/%u (%.1f%%) - Total: %u\n", 
                    chunkIndex, totalChunks, progress, receivedDataChunks);
      sendProgress(progress);
    }
  } else {
    Serial.println("❌ Error malloc chunk");
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - MANEJAR PARITY CHUNK
// ════════════════════════════════════════════════════════════════

void handleParityChunk(uint8_t* data, size_t len) {
  if (!receivingFile) return;
  
  // Validar CRC
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + len - 2, 2);
  uint16_t calculatedCRC = crc16_ccitt(data, len - 2);
  
  if (receivedCRC != calculatedCRC) {
    return;
  }
  
  // Extraer datos
  uint32_t fileID;
  uint16_t blockIndex;
  
  size_t idx = 2;  // Saltar magic bytes
  memcpy(&fileID, data + idx, 4); idx += 4;
  memcpy(&blockIndex, data + idx, 2); idx += 2;
  
  if (fileID != currentFileID) return;
  
  if (blockIndex >= (MAX_CHUNKS / FEC_BLOCK_SIZE)) return;
  
  if (parityReceived[blockIndex]) return;  // Duplicado
  
  // Guardar parity
  size_t dataLen = len - idx - 2;
  parityBuffer[blockIndex] = (uint8_t*)malloc(dataLen);
  if (parityBuffer[blockIndex] != nullptr) {
    memcpy(parityBuffer[blockIndex], data + idx, dataLen);
    parityLengths[blockIndex] = dataLen;
    parityReceived[blockIndex] = true;
    receivedParityChunks++;
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - MANEJAR FILE_END
// ════════════════════════════════════════════════════════════════

void handleFileEnd(uint8_t* data, size_t len) {
  if (!receivingFile) return;
  
  // Validar CRC
  uint16_t receivedCRC;
  memcpy(&receivedCRC, data + len - 2, 2);
  uint16_t calculatedCRC = crc16_ccitt(data, len - 2);
  
  if (receivedCRC != calculatedCRC) {
    return;
  }
  
  uint32_t fileID;
  memcpy(&fileID, data + 2, 4);
  
  if (fileID != currentFileID) return;
  
  Serial.println("\n🏁 FILE_END recibido");
  
  // Dar tiempo para recibir los últimos chunks de esta vuelta
  delay(1000);
  
  // Ensamblar archivo con lo que tenemos
  assembleFile();
}

// ════════════════════════════════════════════════════════════════
// 🔧 RECUPERAR CHUNKS PERDIDOS CON FEC
// ════════════════════════════════════════════════════════════════

void recoverMissingChunks() {
  Serial.println("\n🔧 Intentando recuperar chunks con FEC...");
  
  uint16_t recovered = 0;
  
  for (uint16_t block = 0; block < (totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE; block++) {
    // Verificar si tenemos parity para este bloque
    if (!parityReceived[block]) continue;
    
    // Contar chunks perdidos en este bloque
    uint16_t baseIdx = block * FEC_BLOCK_SIZE;
    int missing = -1;
    int missingCount = 0;
    
    for (int i = 0; i < FEC_BLOCK_SIZE && (baseIdx + i) < totalChunks; i++) {
      if (!chunkReceived[baseIdx + i]) {
        missing = i;
        missingCount++;
      }
    }
    
    // Solo podemos recuperar si falta exactamente 1 chunk
    if (missingCount == 1) {
      uint16_t missingIdx = baseIdx + missing;
      size_t maxLen = parityLengths[block];
      
      // Crear buffer para el chunk recuperado
      chunkBuffer[missingIdx] = (uint8_t*)malloc(maxLen);
      if (chunkBuffer[missingIdx] != nullptr) {
        memcpy(chunkBuffer[missingIdx], parityBuffer[block], maxLen);
        
        // XOR con todos los chunks recibidos del bloque
        for (int i = 0; i < FEC_BLOCK_SIZE && (baseIdx + i) < totalChunks; i++) {
          if (i != missing && chunkReceived[baseIdx + i]) {
            size_t len = min(maxLen, (size_t)chunkLengths[baseIdx + i]);
            for (size_t k = 0; k < len; k++) {
              chunkBuffer[missingIdx][k] ^= chunkBuffer[baseIdx + i][k];
            }
          }
        }
        
        chunkLengths[missingIdx] = maxLen;
        chunkReceived[missingIdx] = true;
        receivedDataChunks++;
        recovered++;
        
        Serial.printf("✅ Chunk %u recuperado con FEC (bloque %u)\n", missingIdx, block);
      }
    }
  }
  
  if (recovered > 0) {
    Serial.printf("🎉 Recuperados %u chunks con FEC!\n", recovered);
  } else {
    Serial.println("ℹ️  No se pudieron recuperar chunks adicionales");
  }
}

// ════════════════════════════════════════════════════════════════
// 📝 ENSAMBLAR ARCHIVO
// ════════════════════════════════════════════════════════════════

void assembleFile() {
  if (!receivingFile) return;
  
  Serial.println("\n═══════════════════════════════════════");
  Serial.println("📝 Ensamblando archivo...");
  
  // Intentar recuperar chunks perdidos con FEC
  recoverMissingChunks();
  
  // Contar chunks finales
  uint16_t missingChunks = 0;
  for (uint16_t i = 0; i < totalChunks; i++) {
    if (!chunkReceived[i]) missingChunks++;
  }
  
  Serial.printf("📊 Chunks recibidos: %u/%u\n", receivedDataChunks, totalChunks);
  Serial.printf("📊 Chunks perdidos: %u (%.1f%%)\n", missingChunks, (missingChunks * 100.0) / totalChunks);
  Serial.printf("📦 Parity chunks: %u\n", receivedParityChunks);
  Serial.printf("🔄 Duplicados: %u\n", duplicateChunks);
  Serial.printf("🔁 Manifests: %u\n", manifestCount);
  
  // Eliminar archivo si existe
  if (LittleFS.exists(receivingFileName)) {
    LittleFS.remove(receivingFileName);
  }
  
  // Crear archivo
  File outFile = LittleFS.open(receivingFileName, "w");
  if (!outFile) {
    Serial.println("❌ Error creando archivo");
    cancelReception("CANT_CREATE");
    return;
  }
  
  // Escribir chunks en orden
  uint32_t writtenBytes = 0;
  for (uint16_t i = 0; i < totalChunks; i++) {
    if (chunkReceived[i]) {
      size_t written = outFile.write(chunkBuffer[i], chunkLengths[i]);
      writtenBytes += written;
    } else {
      // Chunk perdido - rellenar con zeros
      uint8_t zeros[CHUNK_SIZE_LORA] = {0};
      size_t fillSize = min((uint32_t)CHUNK_SIZE_LORA, receivingFileSize - writtenBytes);
      outFile.write(zeros, fillSize);
      writtenBytes += fillSize;
      Serial.printf("⚠️  Chunk %u perdido (rellenado con zeros)\n", i);
    }
  }
  
  outFile.flush();
  outFile.close();
  
  float receptionTime = (millis() - receptionStartTime) / 1000.0;
  float completeness = (receivedDataChunks * 100.0) / totalChunks;
  
  Serial.println("\n✅ Archivo ensamblado");
  Serial.printf("📄 Archivo: %s\n", receivingFileName.c_str());
  Serial.printf("📊 Bytes escritos: %u / %u\n", writtenBytes, receivingFileSize);
  Serial.printf("⏱️  Tiempo: %.2f s\n", receptionTime);
  Serial.printf("📈 Completitud: %.1f%%\n", completeness);
  Serial.printf("📶 RSSI promedio: %d dBm\n", avgRSSI);
  Serial.printf("📶 SNR promedio: %.2f dB\n", avgSNR);
  Serial.println("═══════════════════════════════════════\n");
  
  String cleanName = receivingFileName;
  if (cleanName.startsWith("/")) cleanName = cleanName.substring(1);
  
  String status = "RX_COMPLETE:" + cleanName + ":" + 
                 String(writtenBytes) + ":" + String(receptionTime, 2) + ":" +
                 String(completeness, 1);
  sendResponse(status);
  sendProgress(100);
  
  // Resetear estado
  resetReceptionBuffers();
  receivingFile = false;
  currentFileID = 0;
  
  Serial.println("👂 Esperando nueva transmisión...\n");
}

// ════════════════════════════════════════════════════════════════
// 🔄 RESETEAR BUFFERS DE RECEPCIÓN
// ════════════════════════════════════════════════════════════════

void resetReceptionBuffers() {
  // Liberar memoria de chunks
  for (uint16_t i = 0; i < MAX_CHUNKS; i++) {
    if (chunkBuffer[i] != nullptr) {
      free(chunkBuffer[i]);
      chunkBuffer[i] = nullptr;
    }
    chunkReceived[i] = false;
    chunkLengths[i] = 0;
  }
  
  // Liberar memoria de parity
  for (uint16_t i = 0; i < (MAX_CHUNKS / FEC_BLOCK_SIZE); i++) {
    if (parityBuffer[i] != nullptr) {
      free(parityBuffer[i]);
      parityBuffer[i] = nullptr;
    }
    parityReceived[i] = false;
    parityLengths[i] = 0;
  }
  
  receivedDataChunks = 0;
  receivedParityChunks = 0;
  manifestCount = 0;
  duplicateChunks = 0;
}

// ════════════════════════════════════════════════════════════════
// ❌ CANCELAR RECEPCIÓN
// ════════════════════════════════════════════════════════════════

void cancelReception(String reason) {
  Serial.println("❌ Cancelando recepción: " + reason);
  
  sendResponse("RX_FAILED:" + reason);
  sendProgress(0);
  
  resetReceptionBuffers();
  receivingFile = false;
  currentFileID = 0;
  
  Serial.println("👂 Esperando nueva transmisión...\n");
}

// ════════════════════════════════════════════════════════════════
// 🔐 BASE64 - CODIFICACIÓN
// ════════════════════════════════════════════════════════════════

String encodeBase64(uint8_t* data, size_t length) {
  size_t outputLen;
  mbedtls_base64_encode(NULL, 0, &outputLen, data, length);
  
  uint8_t* encoded = (uint8_t*)malloc(outputLen + 1);
  mbedtls_base64_encode(encoded, outputLen + 1, &outputLen, data, length);
  encoded[outputLen] = '\0';
  
  String result = String((char*)encoded);
  free(encoded);
  
  return result;
}

// ════════════════════════════════════════════════════════════════
// 🔐 BASE64 - DECODIFICACIÓN
// ════════════════════════════════════════════════════════════════

size_t decodeBase64(String input, uint8_t* output, size_t maxLen) {
  size_t outputLen;
  
  int ret = mbedtls_base64_decode(
    output, 
    maxLen, 
    &outputLen, 
    (const unsigned char*)input.c_str(), 
    input.length()
  );
  
  if (ret != 0) return 0;
  
  return outputLen;
}

// ════════════════════════════════════════════════════════════════
// 🔄 RESETEAR ESTADO DE TRANSFERENCIA BLE
// ════════════════════════════════════════════════════════════════

void resetTransferState() {
  if (currentFile) currentFile.close();
  
  currentState = STATE_IDLE;
  currentFilename = "";
  expectedFileSize = 0;
  transferredBytes = 0;
}