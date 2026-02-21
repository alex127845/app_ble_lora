/**
 * ════════════════════════════════════════════════════════════════
 * 📡 File Transfer System TX BROADCAST - Heltec WiFi LoRa 32 V3 (ESP32-S3)
 * ════════════════════════════════════════════════════════════════
 * 
 * Sistema de transferencia de archivos vía BLE + LoRa BROADCAST (sin ACK)
 * 
 * MODO: TRANSMISOR (TX) - DATACASTING PURO
 * 
 * Características:
 * - LittleFS para almacenamiento persistente
 * - BLE para control desde Android
 * - LoRa BROADCAST (sin ACK) con carrusel
 * - FEC (Forward Error Correction) para confiabilidad
 * - Interleaving para resistir pérdidas en ráfaga
 * - Manifest repetido para receptores tardíos
 * - CRC16 para validación de integridad
 * - Configuración dinámica de parámetros LoRa
 * - Progress tracking en tiempo real
 * 
 * @author alex127845
 * @date 2025-01-31
 * @version 4.0 TX BROADCAST
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

#define DEVICE_NAME "Heltec-TX-Broadcast"
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
#define MAX_RETRIES 3                   // Reintentos por fallo de radio
#define FEC_BLOCK_SIZE 8                // Bloques FEC (1 parity cada 8 chunks)
#define MANIFEST_REPEAT 5               // Repeticiones del manifest
#define MANIFEST_INTERVAL 50            // Re-enviar manifest cada N chunks
#define ENABLE_INTERLEAVING true        // Interleaving para ráfagas

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
int currentREPEAT = 2;             // Repeticiones carrusel (vueltas)
int currentPower = 17;             // Potencia en dBm

// Estado de transmisión LoRa
volatile bool transmitting = false;
String currentLoRaFile = "";
unsigned long loraTransmissionStartTime = 0;
uint32_t totalLoRaPacketsSent = 0;
uint32_t totalLoRaRetries = 0;
uint32_t currentFileID = 0;
uint32_t lastFileSize = 0;

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
  STATE_UPLOADING,
  STATE_DOWNLOADING
};

TransferState currentState = STATE_IDLE;
String currentFilename = "";
File currentFile;
uint32_t expectedFileSize = 0;
uint32_t transferredBytes = 0;
uint16_t expectedChunks = 0;
uint16_t receivedChunks = 0;

// ════════════════════════════════════════════════════════════════
// 📝 DECLARACIÓN DE FUNCIONES
// ════════════════════════════════════════════════════════════════

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
void startUpload(String filename, uint32_t fileSize);
void receiveChunk(String base64Data);
void startDownload(String filename);
void sendFileInChunks(String filename);

// LoRa - Configuración
void setLoRaConfig(String jsonStr);
void sendCurrentLoRaConfig();
int getInterPacketDelay();

// LoRa - Transmisión BROADCAST
void startLoRaTransmission(String filename);
void processLoRaTransmission();
bool sendFileViaLoRa(const char* path);
bool sendManifest(uint32_t fileID, uint32_t totalSize, uint16_t totalChunks, const String& fileName);
bool sendDataChunk(uint32_t fileID, uint16_t chunkIndex, uint16_t totalChunks, uint8_t* data, size_t len);
bool sendParityChunk(uint32_t fileID, uint16_t blockIndex, uint8_t* parityData, size_t len);
bool sendFileEnd(uint32_t fileID, uint16_t totalChunks);

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
      
      if (currentState == STATE_UPLOADING && LittleFS.exists(currentFilename)) {
        LittleFS.remove(currentFilename);
        Serial.println("🗑️  Archivo incompleto eliminado");
      }
      
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
// 🚀 SETUP
// ════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n");
  Serial.println("════════════════════════════════════════════════");
  Serial.println("  📡 File Transfer System v4.0 TX BROADCAST");
  Serial.println("  Heltec WiFi LoRa 32 V3");
  Serial.println("  MODO: TRANSMISOR BROADCAST (SIN ACK)");
  Serial.println("════════════════════════════════════════════════");
  Serial.println();
  
  setupLittleFS();
  setupBLE();
  
  delay(1000);
  
  setupLoRa();
  
  Serial.println("\n✅ Sistema TX BROADCAST listo");
  Serial.println("👂 Esperando conexión BLE...");
  Serial.println("📡 Radio LoRa configurado para TX BROADCAST\n");
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
  
  // Procesar transmisión LoRa si está activa
  if (transmitting) {
    processLoRaTransmission();
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
  
  // Test inicial
  radio.standby();
  delay(100);
  
  applyLoRaConfig();
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
  
  Serial.println("📻 Configuración LoRa TX:");
  Serial.printf("   BW: %.0f kHz\n", currentBW);
  Serial.printf("   SF: %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.printf("   Power: %d dBm\n", currentPower);
  Serial.printf("   REPETICIONES: %d vueltas\n", currentREPEAT);
  Serial.printf("   INTERLEAVING: %s\n", ENABLE_INTERLEAVING ? "ACTIVADO" : "DESACTIVADO");
  Serial.printf("   Delay inter-pkt: %dms\n", getInterPacketDelay());
  Serial.println("✅ Configuración aplicada (TX ONLY - SIN ACK)\n");
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA - CALCULAR DELAYS
// ════════════════════════════════════════════════════════════════

int getInterPacketDelay() {
  if (currentBW >= 500.0) {
    if (currentSF <= 7) return 60;
    if (currentSF == 9) return 100;
    return 130;
  } else if (currentBW >= 250.0) {
    if (currentSF <= 7) return 80;
    if (currentSF == 9) return 120;
    return 150;
  } else {  // BW = 125
    if (currentSF <= 7) return 100;
    if (currentSF == 9) return 130;
    return 180;
  }
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
  
  // Comando: UPLOAD_START:filename:size
  else if (command.startsWith("CMD:UPLOAD_START:")) {
    int firstColon = command.indexOf(':', 17);
    if (firstColon > 0) {
      String filename = command.substring(17, firstColon);
      uint32_t fileSize = command.substring(firstColon + 1).toInt();
      startUpload(filename, fileSize);
    } else {
      sendResponse("ERROR:INVALID_UPLOAD_COMMAND");
    }
  }
  
  // Comando: UPLOAD_CHUNK:base64data
  else if (command.startsWith("CMD:UPLOAD_CHUNK:")) {
    String base64Data = command.substring(17);
    receiveChunk(base64Data);
  }
  
  // Comando: DOWNLOAD:filename
  else if (command.startsWith("CMD:DOWNLOAD:")) {
    String filename = command.substring(13);
    Serial.println("📥 Procesando: DOWNLOAD - " + filename);
    startDownload(filename);
  }
  
  // Comando: SET_LORA_CONFIG (JSON con repeticiones)
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
  
  // Comando: TX_FILE:filename (Broadcast)
  else if (command.startsWith("CMD:TX_FILE:")) {
    String filename = command.substring(12);
    Serial.println("📡 Procesando: TX_FILE BROADCAST - " + filename);
    startLoRaTransmission(filename);
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
  
  if (transmitting && currentLoRaFile == filename) {
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
// 📤 UPLOAD BLE - INICIAR
// ════════════════════════════════════════════════════════════════

void startUpload(String filename, uint32_t fileSize) {
  if (currentState != STATE_IDLE) {
    sendResponse("ERROR:TRANSFER_IN_PROGRESS");
    return;
  }
  
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  uint32_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (fileSize > freeSpace) {
    sendResponse("ERROR:NO_SPACE");
    return;
  }
  
  if (LittleFS.exists(filename)) {
    LittleFS.remove(filename);
  }
  
  currentFile = LittleFS.open(filename, "w");
  if (!currentFile) {
    sendResponse("ERROR:CREATE_FAILED");
    return;
  }
  
  currentState = STATE_UPLOADING;
  currentFilename = filename;
  expectedFileSize = fileSize;
  transferredBytes = 0;
  expectedChunks = (fileSize + CHUNK_SIZE_BLE - 1) / CHUNK_SIZE_BLE;
  receivedChunks = 0;
  
  Serial.printf("✅ Upload BLE iniciado: %s (%u bytes)\n", filename.c_str(), fileSize);
  
  sendResponse("OK:UPLOAD_READY");
  sendProgress(0);
}

// ════════════════════════════════════════════════════════════════
// 📦 UPLOAD BLE - RECIBIR CHUNK
// ════════════════════════════════════════════════════════════════

void receiveChunk(String base64Data) {
  if (currentState != STATE_UPLOADING) {
    sendResponse("ERROR:NOT_UPLOADING");
    return;
  }
  
  uint8_t buffer[CHUNK_SIZE_BLE + 10];
  size_t decodedLen = decodeBase64(base64Data, buffer, sizeof(buffer));
  
  if (decodedLen == 0) {
    sendResponse("ERROR:DECODE_FAILED");
    return;
  }
  
  size_t written = currentFile.write(buffer, decodedLen);
  if (written != decodedLen) {
    currentFile.close();
    LittleFS.remove(currentFilename);
    resetTransferState();
    sendResponse("ERROR:WRITE_FAILED");
    return;
  }
  
  transferredBytes += written;
  receivedChunks++;
  
  uint8_t progress = (transferredBytes * 100) / expectedFileSize;
  
  if (receivedChunks % 10 == 0 || receivedChunks >= expectedChunks) {
    sendProgress(progress);
  }
  
  sendResponse("ACK:" + String(receivedChunks));
  
  if (receivedChunks >= expectedChunks || transferredBytes >= expectedFileSize) {
    currentFile.flush();
    currentFile.close();
    
    Serial.printf("✅ Upload BLE completo: %s\n", currentFilename.c_str());
    
    sendResponse("OK:UPLOAD_COMPLETE:" + String(transferredBytes));
    sendProgress(100);
    
    resetTransferState();
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
  
  if (transmitting) {
    sendResponse("ERROR:TRANSMITTING");
    return;
  }
  
  currentBW = doc["bw"];
  currentSF = doc["sf"];
  currentCR = doc["cr"];
  currentREPEAT = doc["repeat"];  // ✅ Nuevo: repeticiones carrusel
  currentPower = doc["power"];
  
  applyLoRaConfig();
  
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
  doc["repeat"] = currentREPEAT;  // ✅ Nuevo: repeticiones carrusel
  doc["power"] = currentPower;
  
  String json;
  serializeJson(doc, json);
  
  sendResponse("LORA_CONFIG:" + json);
  Serial.println("✅ Config LoRa enviada");
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA TX - INICIAR TRANSMISIÓN BROADCAST
// ════════════════════════════════════════════════════════════════

void startLoRaTransmission(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;
  
  if (!LittleFS.exists(filename)) {
    sendResponse("ERROR:FILE_NOT_FOUND");
    return;
  }
  
  if (transmitting) {
    sendResponse("ERROR:ALREADY_TRANSMITTING");
    return;
  }
  
  currentLoRaFile = filename;
  transmitting = true;
  totalLoRaPacketsSent = 0;
  totalLoRaRetries = 0;
  
  sendResponse("OK:TX_STARTING_BROADCAST");
  Serial.println("📡 Iniciando transmisión LoRa BROADCAST...");
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA TX - PROCESAR TRANSMISIÓN BROADCAST
// ════════════════════════════════════════════════════════════════

void processLoRaTransmission() {
  Serial.printf("\n📡 Transmitiendo BROADCAST por LoRa: %s\n", currentLoRaFile.c_str());
  loraTransmissionStartTime = millis();
  
  bool result = sendFileViaLoRa(currentLoRaFile.c_str());
  
  float transmissionTime = (millis() - loraTransmissionStartTime) / 1000.0;
  
  float speed = (lastFileSize * 8.0 * currentREPEAT) / (transmissionTime * 1000.0);
  
  if (result) {
    String status = "TX_COMPLETE_BROADCAST:" + String(lastFileSize) + ":" + 
                   String(transmissionTime, 2) + ":" + 
                   String(speed, 2) + ":" +
                   String(currentREPEAT);
    sendResponse(status);
    
    Serial.println("\n✅ Transmisión LoRa BROADCAST exitosa");
    Serial.printf("⏱️  Tiempo: %.2f s\n", transmissionTime);
    Serial.printf("⚡ Velocidad: %.2f kbps\n", speed);
    Serial.printf("📦 Paquetes: %u\n", totalLoRaPacketsSent);
    Serial.printf("🔄 Fallos radio: %u\n", totalLoRaRetries);
    Serial.printf("🔁 Vueltas: %u\n", currentREPEAT);
    
    Serial.println("╔════════════════════════════════════════╗");
    Serial.printf("║  ⚡ VELOCIDAD: %.2f kbps              ║\n", speed);
    Serial.println("╚════════════════════════════════════════╝");
  } else {
    sendResponse("TX_FAILED:TRANSMISSION_ERROR");
    Serial.println("\n❌ Transmisión LoRa BROADCAST fallida");
  }
  
  transmitting = false;
  currentLoRaFile = "";
}

// ════════════════════════════════════════════════════════════════
// ✅ TRANSMITIR MANIFEST (con CRC16)
// ════════════════════════════════════════════════════════════════

bool sendManifest(uint32_t fileID, uint32_t totalSize, uint16_t totalChunks, const String& fileName) {
  uint8_t nameLen = min((size_t)fileName.length(), (size_t)100);
  uint8_t manifestPkt[2 + 4 + 4 + 2 + 2 + 1 + nameLen + 2];
  size_t idx = 0;
  
  manifestPkt[idx++] = MANIFEST_MAGIC_1;
  manifestPkt[idx++] = MANIFEST_MAGIC_2;
  memcpy(manifestPkt + idx, &fileID, 4); idx += 4;
  memcpy(manifestPkt + idx, &totalSize, 4); idx += 4;
  memcpy(manifestPkt + idx, &totalChunks, 2); idx += 2;
  uint16_t chunkSize = CHUNK_SIZE_LORA;
  memcpy(manifestPkt + idx, &chunkSize, 2); idx += 2;
  manifestPkt[idx++] = nameLen;
  memcpy(manifestPkt + idx, fileName.c_str(), nameLen); idx += nameLen;
  
  uint16_t crc = crc16_ccitt(manifestPkt, idx);
  memcpy(manifestPkt + idx, &crc, 2); idx += 2;
  
  Serial.printf("📤 TX MANIFEST (%zu bytes, fileID=0x%08X)... ", idx, fileID);
  
  radio.standby();
  delay(10);
  
  int state = radio.transmit(manifestPkt, idx);
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("✅ OK");
    return true;
  } else {
    Serial.printf("❌ FALLO código: %d\n", state);
    return false;
  }
}

// ════════════════════════════════════════════════════════════════
// ✅ TRANSMITIR DATA CHUNK (con CRC16)
// ════════════════════════════════════════════════════════════════

bool sendDataChunk(uint32_t fileID, uint16_t chunkIndex, uint16_t totalChunks, uint8_t* data, size_t len) {
  uint8_t dataPkt[2 + 4 + 2 + 2 + CHUNK_SIZE_LORA + 2];
  size_t idx = 0;
  
  dataPkt[idx++] = DATA_MAGIC_1;
  dataPkt[idx++] = DATA_MAGIC_2;
  memcpy(dataPkt + idx, &fileID, 4); idx += 4;
  memcpy(dataPkt + idx, &chunkIndex, 2); idx += 2;
  memcpy(dataPkt + idx, &totalChunks, 2); idx += 2;
  memcpy(dataPkt + idx, data, len); idx += len;
  
  uint16_t crc = crc16_ccitt(dataPkt, idx);
  memcpy(dataPkt + idx, &crc, 2); idx += 2;
  
  radio.standby();
  delay(10);
  
  int retries = 0;
  while (retries < MAX_RETRIES) {
    int state = radio.transmit(dataPkt, idx);
    if (state == RADIOLIB_ERR_NONE) {
      return true;
    }
    
    retries++;
    totalLoRaRetries++;
    
    radio.standby();
    delay(100);
  }
  
  Serial.printf("❌ CRÍTICO: Fallo persistente en chunk %u\n", chunkIndex);
  return false;
}

// ════════════════════════════════════════════════════════════════
// ✅ TRANSMITIR PARITY (FEC - XOR simple)
// ════════════════════════════════════════════════════════════════

bool sendParityChunk(uint32_t fileID, uint16_t blockIndex, uint8_t* parityData, size_t len) {
  uint8_t parityPkt[2 + 4 + 2 + CHUNK_SIZE_LORA + 2];
  size_t idx = 0;
  
  parityPkt[idx++] = PARITY_MAGIC_1;
  parityPkt[idx++] = PARITY_MAGIC_2;
  memcpy(parityPkt + idx, &fileID, 4); idx += 4;
  memcpy(parityPkt + idx, &blockIndex, 2); idx += 2;
  memcpy(parityPkt + idx, parityData, len); idx += len;
  
  uint16_t crc = crc16_ccitt(parityPkt, idx);
  memcpy(parityPkt + idx, &crc, 2); idx += 2;
  
  int state = radio.transmit(parityPkt, idx);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  Parity TX error: %d\n", state);
    return false;
  }
  
  return true;
}

// ✅ TRANSMITIR FILE_END
bool sendFileEnd(uint32_t fileID, uint16_t totalChunks) {
  uint8_t endPkt[2 + 4 + 2 + 2];
  size_t idx = 0;
  
  endPkt[idx++] = FILE_END_MAGIC_1;
  endPkt[idx++] = FILE_END_MAGIC_2;
  memcpy(endPkt + idx, &fileID, 4); idx += 4;
  memcpy(endPkt + idx, &totalChunks, 2); idx += 2;
  
  uint16_t crc = crc16_ccitt(endPkt, idx);
  memcpy(endPkt + idx, &crc, 2); idx += 2;
  
  int state = radio.transmit(endPkt, idx);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("⚠️  FILE_END error: %d\n", state);
    return false;
  }
  
  return true;
}

// ════════════════════════════════════════════════════════════════
// ✅ ENVIAR ARCHIVO CON CARRUSEL + INTERLEAVING + FEC
// ════════════════════════════════════════════════════════════════

bool sendFileViaLoRa(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("❌ Archivo no existe: %s\n", path);
    return false;
  }

  uint32_t totalSize = f.size();
  lastFileSize = totalSize;
  
  String fileName = String(path);
  if (fileName.startsWith("/")) fileName = fileName.substring(1);
  
  uint16_t totalChunks = (totalSize + CHUNK_SIZE_LORA - 1) / CHUNK_SIZE_LORA;
  
  // ✅ File ID único
  currentFileID = (uint32_t)millis() ^ totalSize;
  
  Serial.printf("║  📁 Archivo: %s\n", fileName.c_str());
  Serial.printf("║  📊 Tamaño: %u bytes (%.2f KB)\n", totalSize, totalSize/1024.0);
  Serial.printf("║  📦 Chunks: %u\n", totalChunks);
  Serial.printf("║  🔁 Repeticiones: %u vueltas\n", currentREPEAT);
  Serial.printf("║  🔀 Interleaving: %s\n", ENABLE_INTERLEAVING ? "ACTIVADO" : "DESACTIVADO");
  Serial.printf("║  🆔 File ID: 0x%08X\n", currentFileID);
  
  // ✅ Calcular tiempo estimado
  float timePerChunk = 0.0;
  if (currentBW >= 500.0 && currentSF <= 7) timePerChunk = 0.15;
  else if (currentBW >= 250.0 && currentSF == 9) timePerChunk = 0.25;
  else if (currentBW == 125.0 && currentSF == 9) timePerChunk = 0.35;
  else timePerChunk = 0.8;  // SF12
  
  float estimatedTimePerRound = totalChunks * timePerChunk;
  float estimatedTotal = estimatedTimePerRound * currentREPEAT;
  
  Serial.printf("║  ⏱️  Tiempo estimado: %.1f min (%.1f min/vuelta)\n", 
                estimatedTotal/60.0, estimatedTimePerRound/60.0);
  Serial.printf("╚════════════════════════════════════════╝\n\n");
  
  int dynamicDelay = getInterPacketDelay();
  
  // ============================================
  // ✅ CARRUSEL: Repetir N vueltas
  // ============================================
  for (int round = 1; round <= currentREPEAT; round++) {
    Serial.printf("\n╔════════════════════════════════════════╗\n");
    Serial.printf("║       🔁 VUELTA %d de %d                \n", round, currentREPEAT);
    Serial.printf("╚════════════════════════════════════════╝\n\n");
    
    // ✅ Enviar MANIFEST repetido
    Serial.println("📤 Enviando MANIFEST (5 repeticiones)...");
    for (int m = 0; m < MANIFEST_REPEAT; m++) {
      if (!sendManifest(currentFileID, totalSize, totalChunks, fileName)) {
        f.close();
        return false;
      }
      delay(dynamicDelay + 50);
    }
    Serial.println("✅ MANIFEST OK\n");
    
    delay(300);
    
    // ✅ Transmitir chunks con interleaving
    f.seek(0);
    
    uint8_t fecBlock[FEC_BLOCK_SIZE][CHUNK_SIZE_LORA];
    size_t fecLengths[FEC_BLOCK_SIZE];
    int fecIndex = 0;
    
    // ✅ Progress tracker
    uint16_t lastProgressPercent = 0;
    
    for (uint16_t i = 0; i < totalChunks; i++) {
      // ✅ INTERLEAVING: orden pseudoaleatorio
      uint16_t index;
      #if ENABLE_INTERLEAVING
        // Stride prime para distribuir uniformemente
        index = (i * 37) % totalChunks;
      #else
        index = i;
      #endif
      
      // Leer chunk en la posición correcta
      f.seek((uint32_t)index * CHUNK_SIZE_LORA);
      
      uint8_t buffer[CHUNK_SIZE_LORA];
      size_t bytesRead = f.read(buffer, CHUNK_SIZE_LORA);
      
      if (bytesRead == 0) break;
      
      // ✅ Guardar en buffer FEC
      memcpy(fecBlock[fecIndex], buffer, bytesRead);
      fecLengths[fecIndex] = bytesRead;
      fecIndex++;
      
      // ✅ Transmitir chunk
      if (!sendDataChunk(currentFileID, index, totalChunks, buffer, bytesRead)) {
        f.close();
        return false;
      }
      
      totalLoRaPacketsSent++;
      
      // ✅ Mostrar progreso cada 5%
      uint16_t currentPercent = ((i + 1) * 100) / totalChunks;
      if (currentPercent >= lastProgressPercent + 5 || i + 1 == totalChunks) {
        Serial.printf("📦 Progreso: %u/%u (%.1f%%) - Vuelta %d\n", 
                      i + 1, totalChunks, (float)(i + 1) * 100.0 / totalChunks, round);
        
        // ✅ Notificar progreso por BLE
        uint8_t bleProgress = ((round - 1) * 100 + currentPercent) / currentREPEAT;
        sendProgress(bleProgress);
        
        lastProgressPercent = currentPercent;
      }
      
      delay(dynamicDelay);
      
      // ✅ Enviar parity cada FEC_BLOCK_SIZE chunks
      if (fecIndex == FEC_BLOCK_SIZE || i + 1 == totalChunks) {
        uint8_t parityData[CHUNK_SIZE_LORA];
        memset(parityData, 0, CHUNK_SIZE_LORA);
        
        size_t maxLen = 0;
        for (int j = 0; j < fecIndex; j++) {
          if (fecLengths[j] > maxLen) maxLen = fecLengths[j];
        }
        
        for (int j = 0; j < fecIndex; j++) {
          for (size_t k = 0; k < fecLengths[j]; k++) {
            parityData[k] ^= fecBlock[j][k];
          }
        }
        
        uint16_t blockIndex = i / FEC_BLOCK_SIZE;
        
        if (!sendParityChunk(currentFileID, blockIndex, parityData, maxLen)) {
          Serial.println("⚠️  Parity falló (continuando)");
        }
        
        totalLoRaPacketsSent++;
        fecIndex = 0;
        delay(dynamicDelay);
      }
      
      // ✅ Re-enviar manifest periódicamente
      if ((i + 1) % MANIFEST_INTERVAL == 0) {
        sendManifest(currentFileID, totalSize, totalChunks, fileName);
        delay(dynamicDelay + 30);
      }
      
      yield();  // ✅ Evitar WDT reset en archivos grandes
    }
    
    // ✅ FILE_END al terminar cada vuelta
    Serial.printf("\n🏁 Enviando FILE_END (vuelta %d)...\n", round);
    sendFileEnd(currentFileID, totalChunks);
    delay(500);
  }

  f.close();

  Serial.println("║     🎉 TRANSMISIÓN COMPLETA           ║");
  Serial.printf("📊 Total paquetes: %u\n", totalLoRaPacketsSent);
  Serial.printf("📈 Fallos de radio: %u\n", totalLoRaRetries);
  Serial.printf("🔁 Vueltas completadas: %u\n", currentREPEAT);
  
  return true;
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
  expectedChunks = 0;
  receivedChunks = 0;
}