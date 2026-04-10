#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <Preferences.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN - ESP32 V3
// ════════════════════════════════════════════════════════════════

#define VEXT 36
#define VEXT_ON LOW

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN - BLE
// ════════════════════════════════════════════════════════════════

#define DEVICE_NAME "Heltec-TX-Broadcast-ESPNOW"
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_WRITE_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DATA_READ_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define PROGRESS_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26aa"

// ✅ Identificación de modo TX
#define IS_TX_MODE

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN - PROTOCOLO BROADCAST
// ════════════════════════════════════════════════════════════════

#define CHUNK_SIZE_BLE    200
#define CHUNK_SIZE_ESPNOW 240  // ESP-NOW MTU optimizado
#define MAX_FILENAME_LENGTH 64
#define MAX_RETRIES       3
#define FEC_BLOCK_SIZE    8
#define MANIFEST_REPEAT   5
#define MANIFEST_INTERVAL 50

#define ENABLE_INTERLEAVING false

// Magic bytes para protocolo
#define MANIFEST_MAGIC_1  0xAA
#define MANIFEST_MAGIC_2  0xBB
#define DATA_MAGIC_1      0xCC
#define DATA_MAGIC_2      0xDD
#define PARITY_MAGIC_1    0xEE
#define PARITY_MAGIC_2    0xFF
#define FILE_END_MAGIC_1  0x99
#define FILE_END_MAGIC_2  0x88

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - ESP-NOW
// ════════════════════════════════════════════════════════════════

// Dirección de broadcast de ESP-NOW
uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Configuración ESP-NOW
int   currentPower = 20;      // dBm (máximo 20)
int   currentChannel = 1;     // Canal Wi-Fi (1-13)
int   currentRate = 1;        // Velocidad: 0=1Mbps, 1=2Mbps (default), 2=5.5Mbps, 3=11Mbps

// Variables de transmisión ESP-NOW
volatile bool transmitting = false;
String  currentESPNowFile = "";
unsigned long espnowTransmissionStartTime = 0;
uint32_t totalESPNowPacketsSent = 0;
uint32_t totalESPNowRetries = 0;
uint32_t currentFileID = 0;
uint32_t lastFileSize = 0;

static uint32_t fileIDCounter = 0;

// ✅ Objeto Preferences para persistencia
Preferences preferences;

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - BLE
// ════════════════════════════════════════════════════════════════

BLEServer*         pServer               = NULL;
BLECharacteristic* pCmdCharacteristic    = NULL;
BLECharacteristic* pDataCharacteristic   = NULL;
BLECharacteristic* pProgressCharacteristic = NULL;
bool deviceConnected    = false;
bool oldDeviceConnected = false;

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - ESTADO DE TRANSFERENCIA BLE
// ════════════════════════════════════════════════════════════════

enum TransferState {
  STATE_IDLE,
  STATE_UPLOADING,
  STATE_DOWNLOADING
};

TransferState currentState   = STATE_IDLE;
String        currentFilename = "";
File          currentFile;
uint32_t      expectedFileSize = 0;
uint32_t      transferredBytes = 0;
uint16_t      expectedChunks   = 0;
uint16_t      receivedChunks   = 0;

// ════════════════════════════════════════════════════════════════
// 📝 DECLARACIÓN DE FUNCIONES
// ════════════════════════════════════════════════════════════════

void setupLittleFS();
void setupBLE();
void setupESPNow();
void applyESPNowConfig();
void enableVext(bool on);

// Funciones para persistencia
void loadESPNowConfig();
void saveESPNowConfig();

void handleCommand(String command);
void sendResponse(String response);
void sendProgress(uint8_t percentage);

void listFiles();
void deleteFile(String filename);
void startUpload(String filename, uint32_t fileSize);
void receiveChunk(String base64Data);
void startDownload(String filename);
void sendFileInChunks(String filename);

void setESPNowConfig(String jsonStr);
void sendCurrentESPNowConfig();
int  getInterPacketDelay();

void startESPNowTransmission(String filename);
void processESPNowTransmission();
bool sendFileViaESPNow(const char* path);
bool sendManifest(uint32_t fileID, uint32_t totalSize, uint16_t totalChunks, const String& fileName);
bool sendDataChunk(uint32_t fileID, uint16_t chunkIndex, uint16_t totalChunks, uint8_t* data, size_t len);
bool sendParityChunk(uint32_t fileID, uint16_t blockIndex, uint8_t* parityData, size_t len);
bool sendFileEnd(uint32_t fileID, uint16_t totalChunks);

String encodeBase64(uint8_t* data, size_t length);
size_t decodeBase64(String input, uint8_t* output, size_t maxLen);
void   resetTransferState();
uint16_t crc16_ccitt(const uint8_t* data, size_t len);

// ════════════════════════════════════════════════════════════════
// 🔐 CRC16-CCITT
// ════════════════════════════════════════════════════════════════

uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else              crc = (crc << 1);
    }
  }
  return crc;
}

// ════════════════════════════════════════════════════════════════
// 🔌 ESP-NOW CALLBACKS
// ════════════════════════════════════════════════════════════════

void onESPNowSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    totalESPNowRetries++;
  }
}

// ════════════════════════════════════════════════════════════════
// 🔌 BLE CALLBACKS
// ════════════════════════════════════════════════════════════════

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("\n✅ Cliente BLE conectado");
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("\n❌ Cliente BLE desconectado");

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

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    uint8_t* pData = pCharacteristic->getData();
    size_t   len   = pCharacteristic->getValue().length();

    if (len > 0 && pData != nullptr) {
      String command = "";
      for (size_t i = 0; i < len; i++) command += (char)pData[i];
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
  Serial.println("  📡 File Transfer System v5.0 TX BROADCAST");
  Serial.println("  Heltec WiFi LoRa 32 V3");
  Serial.println("  MODO: TRANSMISOR BROADCAST ESP-NOW");
  Serial.println("  ✅ CON PERSISTENCIA DE CONFIGURACIÓN");
  Serial.println("════════════════════════════════════════════════");
  Serial.println();

  setupLittleFS();
  setupBLE();
  delay(1000);
  setupESPNow();

  // Cargar configuración guardada
  loadESPNowConfig();

  Serial.println("\n✅ Sistema TX BROADCAST ESP-NOW listo");
  Serial.println("👂 Esperando conexión BLE...");
  Serial.println("📡 ESP-NOW configurado para TX BROADCAST\n");
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

  // Procesar transmisión ESP-NOW si está activa
  if (transmitting) {
    processESPNowTransmission();
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
    while (1) delay(1000);
  }

  Serial.println("✅ LittleFS montado");

  uint32_t totalBytes = LittleFS.totalBytes();
  uint32_t usedBytes  = LittleFS.usedBytes();

  Serial.printf("   Total: %.2f MB\n", totalBytes / 1048576.0);
  Serial.printf("   Usado: %.2f MB\n", usedBytes  / 1048576.0);
  Serial.printf("   Libre: %.2f MB\n", (totalBytes - usedBytes) / 1048576.0);

  Serial.println("\n📁 Archivos:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int  count = 0;

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

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pCmdCharacteristic = pService->createCharacteristic(
    CMD_WRITE_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCmdCharacteristic->setCallbacks(new CmdCallbacks());

  pDataCharacteristic = pService->createCharacteristic(
    DATA_READ_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pDataCharacteristic->addDescriptor(new BLE2902());

  pProgressCharacteristic = pService->createCharacteristic(
    PROGRESS_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pProgressCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  Serial.println("✅ Servicio BLE creado: " + String(SERVICE_UUID));
  Serial.println("   - CMD_WRITE: " + String(CMD_WRITE_UUID));
  Serial.println("   - DATA_READ: " + String(DATA_READ_UUID));
  Serial.println("   - PROGRESS:  " + String(PROGRESS_UUID));

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("✅ BLE iniciado: " + String(DEVICE_NAME));
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════

void enableVext(bool on) {
  pinMode(VEXT, OUTPUT);
  digitalWrite(VEXT, on ? VEXT_ON : !VEXT_ON);
}

void setupESPNow() {
  Serial.println("\n📡 Inicializando ESP-NOW...");

  enableVext(true);
  delay(200);

  // Inicializar Wi-Fi en modo STA
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Inicializar ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error inicializando ESP-NOW");
    Serial.println("⚠️ Continuando sin ESP-NOW...");
    return;
  }

  Serial.println("✅ ESP-NOW inicializado");

  // Registrar callback
  esp_now_register_send_cb(onESPNowSent);

  // Agregar peer broadcast
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = currentChannel;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("❌ Error agregando peer broadcast");
    return;
  }

  Serial.println("✅ Peer broadcast agregado");
}

// ════════════════════════════════════════════════════════════════
// ✅ CARGAR CONFIGURACIÓN ESP-NOW DESDE MEMORIA FLASH
// ════════════════════════════════════════════════════════════════

void loadESPNowConfig() {
  Serial.println("\n💾 Cargando configuración ESP-NOW desde memoria flash...");
  
  preferences.begin("espnow-config", true);  // Modo solo lectura
  
  // Cargar valores (si no existen, usar defaults)
  currentPower  = preferences.getInt("power", 20);
  currentChannel = preferences.getInt("channel", 1);
  currentRate   = preferences.getInt("rate", 1);
  
  preferences.end();
  
  Serial.println("✅ Configuración ESP-NOW cargada:");
  Serial.printf("   Potencia: %d dBm\n", currentPower);
  Serial.printf("   Canal: %d\n", currentChannel);
  Serial.printf("   Velocidad: %s\n", 
    currentRate == 0 ? "1 Mbps" :
    currentRate == 1 ? "2 Mbps" :
    currentRate == 2 ? "5.5 Mbps" : "11 Mbps");
  
  // Aplicar configuración
  applyESPNowConfig();
}

// ════════════════════════════════════════════════════════════════
// ✅ GUARDAR CONFIGURACIÓN ESP-NOW EN MEMORIA FLASH
// ════════════════════════════════════════════════════════════════

void saveESPNowConfig() {
  preferences.begin("espnow-config", false);  // Modo escritura
  
  preferences.putInt("power", currentPower);
  preferences.putInt("channel", currentChannel);
  preferences.putInt("rate", currentRate);
  
  preferences.end();
  
  Serial.println("💾 ✅ Configuración ESP-NOW guardada en memoria flash");
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW - APLICAR CONFIGURACIÓN
// ════════════════════════════════════════════════════════════════

void applyESPNowConfig() {
  Serial.println("\n📻 Aplicando configuración ESP-NOW BROADCAST...");

  // Validar parámetros
  if (currentChannel < 1 || currentChannel > 13) currentChannel = 1;
  if (currentPower < 2 || currentPower > 20) currentPower = 20;
  if (currentRate < 0 || currentRate > 3) currentRate = 1;

  // Cambiar canal
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  delay(100);

  // Configurar potencia de transmisión
  esp_wifi_set_max_tx_power(currentPower * 4);  // Convert dBm to internal units
  delay(100);

  Serial.println("📻 Configuración ESP-NOW TX:");
  Serial.printf("   Potencia: %d dBm\n", currentPower);
  Serial.printf("   Canal: %d\n", currentChannel);
  Serial.printf("   Velocidad: %s\n",
    currentRate == 0 ? "1 Mbps" :
    currentRate == 1 ? "2 Mbps" :
    currentRate == 2 ? "5.5 Mbps" : "11 Mbps");
  Serial.printf("   Delay inter-pkt: %dms\n", getInterPacketDelay());
  Serial.println("✅ Configuración aplicada (TX BROADCAST)\n");
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW - CALCULAR DELAYS
// ════════════════════════════════════════════════════════════════

int getInterPacketDelay() {
  // ESP-NOW es más rápido que LoRa
  if (currentRate == 0) return 10;     // 1 Mbps
  if (currentRate == 1) return 5;      // 2 Mbps (default)
  if (currentRate == 2) return 3;      // 5.5 Mbps
  return 2;                            // 11 Mbps
}

// ════════════════════════════════════════════════════════════════
// 🎯 MANEJO DE COMANDOS BLE
// ════════════════════════════════════════════════════════════════

void handleCommand(String command) {
  command.trim();

  if (command == "CMD:LIST") {
    listFiles();
  }
  else if (command == "CMD:GET_MODE") {
    #ifdef IS_TX_MODE
      sendResponse("MODE:TX");
      Serial.println("📤 Modo identificado: TX");
    #else
      sendResponse("MODE:RX");
      Serial.println("📥 Modo identificado: RX");
    #endif
  }
  else if (command.startsWith("CMD:DELETE:")) {
    deleteFile(command.substring(11));
  }
  else if (command.startsWith("CMD:UPLOAD_START:")) {
    int firstColon = command.indexOf(':', 17);
    if (firstColon > 0) {
      String   filename = command.substring(17, firstColon);
      uint32_t fileSize = command.substring(firstColon + 1).toInt();
      startUpload(filename, fileSize);
    } else {
      sendResponse("ERROR:INVALID_UPLOAD_COMMAND");
    }
  }
  else if (command.startsWith("CMD:UPLOAD_CHUNK:")) {
    receiveChunk(command.substring(17));
  }
  else if (command.startsWith("CMD:DOWNLOAD:")) {
    startDownload(command.substring(13));
  }
  else if (command.startsWith("CMD:SET_ESPNOW_CONFIG:")) {
    setESPNowConfig(command.substring(22));
  }
  else if (command == "CMD:GET_ESPNOW_CONFIG") {
    sendCurrentESPNowConfig();
  }
  else if (command.startsWith("CMD:TX_FILE:")) {
    startESPNowTransmission(command.substring(12));
  }
  else if (command == "CMD:PING") {
    sendResponse("PONG");
  }
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

// ══════════���═════════════════════════════════════════════════════
// 📋 LISTAR ARCHIVOS
// ════════════════════════════════════════════════════════════════

void listFiles() {
  sendResponse("FILES_START");

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int  count = 0;

  while (file) {
    if (!file.isDirectory()) {
      String filename = String(file.name());
      if (filename.startsWith("/")) filename = filename.substring(1);
      sendResponse("FILE:" + filename + ":" + String(file.size()));
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

  if (!LittleFS.exists(filename)) { sendResponse("ERROR:FILE_NOT_FOUND"); return; }
  if (transmitting && currentESPNowFile == filename) { sendResponse("ERROR:FILE_IN_USE"); return; }
  if (currentState != STATE_IDLE && currentFilename == filename) { sendResponse("ERROR:FILE_IN_USE"); return; }

  delay(100);

  if (LittleFS.remove(filename)) {
    Serial.println("✅ Eliminado: " + filename);
    sendResponse("OK:DELETED");
  } else {
    sendResponse("ERROR:CANT_DELETE");
  }
}

// ════════════════════════════════════════════════════════════════
// 📤 UPLOAD BLE - INICIAR
// ════════════════════════════════════════════════════════════════

void startUpload(String filename, uint32_t fileSize) {
  if (currentState != STATE_IDLE) { sendResponse("ERROR:TRANSFER_IN_PROGRESS"); return; }

  if (!filename.startsWith("/")) filename = "/" + filename;

  uint32_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (fileSize > freeSpace) { sendResponse("ERROR:NO_SPACE"); return; }

  if (LittleFS.exists(filename)) LittleFS.remove(filename);

  currentFile = LittleFS.open(filename, "w");
  if (!currentFile) { sendResponse("ERROR:CREATE_FAILED"); return; }

  currentState    = STATE_UPLOADING;
  currentFilename = filename;
  expectedFileSize = fileSize;
  transferredBytes = 0;
  expectedChunks   = (fileSize + CHUNK_SIZE_BLE - 1) / CHUNK_SIZE_BLE;
  receivedChunks   = 0;

  Serial.printf("✅ Upload BLE iniciado: %s (%u bytes)\n", filename.c_str(), fileSize);
  sendResponse("OK:UPLOAD_READY");
  sendProgress(0);
}

// ════════════════════════════════════════════════════════════════
// 📦 UPLOAD BLE - RECIBIR CHUNK
// ════════════════════════════════════════════════════════════════

void receiveChunk(String base64Data) {
  if (currentState != STATE_UPLOADING) { sendResponse("ERROR:NOT_UPLOADING"); return; }

  uint8_t buffer[CHUNK_SIZE_BLE + 10];
  size_t  decodedLen = decodeBase64(base64Data, buffer, sizeof(buffer));

  if (decodedLen == 0) { sendResponse("ERROR:DECODE_FAILED"); return; }

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
  if (receivedChunks % 10 == 0 || receivedChunks >= expectedChunks) sendProgress(progress);

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
  if (currentState != STATE_IDLE) { sendResponse("ERROR:TRANSFER_IN_PROGRESS"); return; }

  if (!filename.startsWith("/")) filename = "/" + filename;
  if (!LittleFS.exists(filename)) { sendResponse("ERROR:FILE_NOT_FOUND"); return; }

  File file = LittleFS.open(filename, "r");
  if (!file) { sendResponse("ERROR:OPEN_FAILED"); return; }

  uint32_t fileSize = file.size();
  file.close();

  currentState     = STATE_DOWNLOADING;
  currentFilename  = filename;
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
  if (!file) { sendResponse("ERROR:FILE_OPEN_FAILED"); resetTransferState(); return; }

  uint32_t totalSize   = file.size();
  uint16_t totalChunks = (totalSize + CHUNK_SIZE_BLE - 1) / CHUNK_SIZE_BLE;
  uint16_t chunkNum    = 0;

  sendProgress(0);

  uint8_t buffer[CHUNK_SIZE_BLE];

  while (file.available()) {
    size_t bytesRead = file.read(buffer, CHUNK_SIZE_BLE);
    if (bytesRead == 0) break;

    String encoded  = encodeBase64(buffer, bytesRead);
    String chunkMsg = "CHUNK:" + String(chunkNum) + ":" + encoded;
    sendResponse(chunkMsg);

    transferredBytes += bytesRead;
    chunkNum++;

    uint8_t progress = (transferredBytes * 100) / totalSize;
    if (chunkNum % 5 == 0 || chunkNum >= totalChunks) sendProgress(progress);

    delay(20);
  }

  file.close();
  sendResponse("DOWNLOAD_END:" + String(transferredBytes));
  sendProgress(100);
  Serial.printf("✅ Download BLE completo: %u bytes\n", transferredBytes);
  resetTransferState();
}

// ════════════════════════════════════════════════════════════════
// ⚙️  CONFIGURACIÓN ESP-NOW - SET
// ════════════════════════════════════════════════════════════════

void setESPNowConfig(String jsonStr) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);

  if (error) {
    Serial.println("❌ JSON inválido: " + String(error.c_str()));
    sendResponse("ERROR:INVALID_JSON");
    return;
  }

  if (transmitting) {
    sendResponse("ERROR:TRANSMITTING");
    return;
  }

  // Validar y actualizar parámetros
  if (doc.containsKey("power")) {
    int pwr = doc["power"].as<int>();
    if (pwr >= 2 && pwr <= 20) {
      currentPower = pwr;
    } else {
      Serial.printf("⚠️  POWER inválido (%d), ignorado\n", pwr);
    }
  }

  if (doc.containsKey("channel")) {
    int ch = doc["channel"].as<int>();
    if (ch >= 1 && ch <= 13) {
      currentChannel = ch;
    } else {
      Serial.printf("⚠️  CHANNEL inválido (%d), ignorado\n", ch);
    }
  }

  if (doc.containsKey("rate")) {
    int rate = doc["rate"].as<int>();
    if (rate >= 0 && rate <= 3) {
      currentRate = rate;
    } else {
      Serial.printf("⚠️  RATE inválido (%d), ignorado\n", rate);
    }
  }

  // Guardar en memoria flash
  saveESPNowConfig();
  
  applyESPNowConfig();
  sendResponse("OK:ESPNOW_CONFIG_SET");
  Serial.println("✅ Configuración ESP-NOW actualizada y guardada");
}

// ════════════════════════════════════════════════════════════════
// ⚙️  CONFIGURACIÓN ESP-NOW - GET
// ════════════════════════════════════════════════════════════════

void sendCurrentESPNowConfig() {
  StaticJsonDocument<200> doc;
  doc["power"]   = currentPower;
  doc["channel"] = currentChannel;
  doc["rate"]    = currentRate;

  String json;
  serializeJson(doc, json);
  sendResponse("ESPNOW_CONFIG:" + json);
  Serial.println("✅ Config ESP-NOW enviada: " + json);
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW TX - INICIAR TRANSMISIÓN BROADCAST
// ════════════════════════════════════════════════════════════════

void startESPNowTransmission(String filename) {
  if (!filename.startsWith("/")) filename = "/" + filename;

  if (!LittleFS.exists(filename)) { sendResponse("ERROR:FILE_NOT_FOUND"); return; }
  if (transmitting)               { sendResponse("ERROR:ALREADY_TRANSMITTING"); return; }

  currentESPNowFile     = filename;
  transmitting          = true;
  totalESPNowPacketsSent = 0;
  totalESPNowRetries    = 0;

  sendResponse("OK:TX_STARTING_BROADCAST");
  Serial.println("📡 Iniciando transmisión ESP-NOW BROADCAST...");
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW TX - PROCESAR TRANSMISIÓN BROADCAST
// ════════════════════════════════════════════════════════════════

void processESPNowTransmission() {
  Serial.printf("\n📡 Transmitiendo BROADCAST por ESP-NOW: %s\n", currentESPNowFile.c_str());
  espnowTransmissionStartTime = millis();

  bool result = sendFileViaESPNow(currentESPNowFile.c_str());

  float transmissionTime = (millis() - espnowTransmissionStartTime) / 1000.0;
  float speed = (lastFileSize * 8.0) / (transmissionTime * 1000.0);

  if (result) {
    String status = "TX_COMPLETE_BROADCAST:" + String(lastFileSize) + ":" +
                    String(transmissionTime, 2) + ":" +
                    String(speed, 2);
    sendResponse(status);

    Serial.println("\n✅ Transmisión ESP-NOW BROADCAST exitosa");
    Serial.printf("⏱️  Tiempo: %.2f s\n",       transmissionTime);
    Serial.printf("⚡ Velocidad: %.2f kbps\n",   speed);
    Serial.printf("📦 Paquetes: %u\n",           totalESPNowPacketsSent);
    Serial.printf("🔄 Fallos: %u\n",             totalESPNowRetries);
    Serial.println("╔════════════════════════════════════════╗");
    Serial.printf("║  ⚡ VELOCIDAD: %.2f kbps              ║\n", speed);
    Serial.println("╚════════════════════════════════════════╝");
  } else {
    sendResponse("TX_FAILED:TRANSMISSION_ERROR");
    Serial.println("\n❌ Transmisión ESP-NOW BROADCAST fallida");
  }

  transmitting      = false;
  currentESPNowFile = "";
}

// ════════════════════════════════════════════════════════════════
// ✅ TRANSMITIR MANIFEST (con CRC16)
// ════════════════════════════════════════════════════════════════

bool sendManifest(uint32_t fileID, uint32_t totalSize, uint16_t totalChunks, const String& fileName) {
  uint8_t nameLen = (uint8_t)min((size_t)fileName.length(), (size_t)100);
  uint8_t manifestPkt[2 + 4 + 4 + 2 + 2 + 1 + 100 + 2];
  size_t idx = 0;

  manifestPkt[idx++] = MANIFEST_MAGIC_1;
  manifestPkt[idx++] = MANIFEST_MAGIC_2;
  memcpy(manifestPkt + idx, &fileID,      4); idx += 4;
  memcpy(manifestPkt + idx, &totalSize,   4); idx += 4;
  memcpy(manifestPkt + idx, &totalChunks, 2); idx += 2;
  uint16_t chunkSize = CHUNK_SIZE_ESPNOW;
  memcpy(manifestPkt + idx, &chunkSize,   2); idx += 2;
  manifestPkt[idx++] = nameLen;
  memcpy(manifestPkt + idx, fileName.c_str(), nameLen); idx += nameLen;

  uint16_t crc = crc16_ccitt(manifestPkt, idx);
  memcpy(manifestPkt + idx, &crc, 2); idx += 2;

  Serial.printf("📤 TX MANIFEST (%zu bytes, fileID=0x%08X)... ", idx, fileID);

  int state = esp_now_send(broadcastAddress, manifestPkt, idx);
  if (state == ESP_OK) {
    Serial.println("✅ OK");
    return true;
  }

  Serial.printf("❌ FALLO código: %d\n", state);
  return false;
}

// ════════════════════════════════════════════════════════════════
// ✅ TRANSMITIR DATA CHUNK (con CRC16)
// ════════════════════════════════════════════════════════════════

bool sendDataChunk(uint32_t fileID, uint16_t chunkIndex, uint16_t totalChunks, uint8_t* data, size_t len) {
  uint8_t dataPkt[2 + 4 + 2 + 2 + CHUNK_SIZE_ESPNOW + 2];
  size_t idx = 0;

  dataPkt[idx++] = DATA_MAGIC_1;
  dataPkt[idx++] = DATA_MAGIC_2;
  memcpy(dataPkt + idx, &fileID,      4); idx += 4;
  memcpy(dataPkt + idx, &chunkIndex,  2); idx += 2;
  memcpy(dataPkt + idx, &totalChunks, 2); idx += 2;
  memcpy(dataPkt + idx, data, len);       idx += len;

  uint16_t crc = crc16_ccitt(dataPkt, idx);
  memcpy(dataPkt + idx, &crc, 2); idx += 2;

  for (int retries = 0; retries < MAX_RETRIES; retries++) {
    int state = esp_now_send(broadcastAddress, dataPkt, idx);
    if (state == ESP_OK) return true;

    totalESPNowRetries++;
    Serial.printf("⚠️  Reintento %d/%d en chunk %u\n", retries + 1, MAX_RETRIES, chunkIndex);
    delay(10);
    yield();
  }

  Serial.printf("❌ CRÍTICO: Fallo persistente en chunk %u\n", chunkIndex);
  return false;
}

// ══════════════════════════════════════════════════��═════════════
// ✅ TRANSMITIR PARITY (FEC - XOR simple)
// ════════════════════════════════════════════════════════════════

bool sendParityChunk(uint32_t fileID, uint16_t blockIndex, uint8_t* parityData, size_t len) {
  uint8_t parityPkt[2 + 4 + 2 + CHUNK_SIZE_ESPNOW + 2];
  size_t idx = 0;

  parityPkt[idx++] = PARITY_MAGIC_1;
  parityPkt[idx++] = PARITY_MAGIC_2;
  memcpy(parityPkt + idx, &fileID,      4); idx += 4;
  memcpy(parityPkt + idx, &blockIndex,  2); idx += 2;
  memcpy(parityPkt + idx, parityData, len); idx += len;

  uint16_t crc = crc16_ccitt(parityPkt, idx);
  memcpy(parityPkt + idx, &crc, 2); idx += 2;

  int state = esp_now_send(broadcastAddress, parityPkt, idx);
  if (state != ESP_OK) {
    Serial.printf("⚠️  Parity TX error: %d\n", state);
    return false;
  }
  return true;
}

// ════════════════════════════════════════════════════════════════
// ✅ TRANSMITIR FILE_END
// ════════════════════════════════════════════════════════════════

bool sendFileEnd(uint32_t fileID, uint16_t totalChunks) {
  uint8_t endPkt[2 + 4 + 2 + 2];
  size_t idx = 0;

  endPkt[idx++] = FILE_END_MAGIC_1;
  endPkt[idx++] = FILE_END_MAGIC_2;
  memcpy(endPkt + idx, &fileID,      4); idx += 4;
  memcpy(endPkt + idx, &totalChunks, 2); idx += 2;

  uint16_t crc = crc16_ccitt(endPkt, idx);
  memcpy(endPkt + idx, &crc, 2); idx += 2;

  Serial.print("📤 TX FILE_END... ");
  bool sent = false;

  for (int attempt = 0; attempt < 5; attempt++) {
    int state = esp_now_send(broadcastAddress, endPkt, idx);
    if (state == ESP_OK) {
      sent = true;
      Serial.printf("✅ OK (intento %d)\n", attempt + 1);
      break;
    }
    Serial.printf("⚠️  FILE_END error %d (intento %d/5)\n", state, attempt + 1);
    delay(20);
    yield();
  }

  if (!sent) Serial.println("❌ FILE_END fallido tras 5 intentos");
  return sent;
}

// ════════════════════════════════════════════════════════════════
// ✅ ENVIAR ARCHIVO CON CARRUSEL + FEC
// ════════════════════════════════════════════════════════════════

bool sendFileViaESPNow(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("❌ Archivo no existe: %s\n", path);
    return false;
  }

  uint32_t totalSize   = f.size();
  lastFileSize         = totalSize;

  String fileName = String(path);
  if (fileName.startsWith("/")) fileName = fileName.substring(1);

  uint16_t totalChunks = (totalSize + CHUNK_SIZE_ESPNOW - 1) / CHUNK_SIZE_ESPNOW;

  currentFileID = (uint32_t)millis() ^ (totalSize << 8) ^ (++fileIDCounter);

  Serial.printf("╔════════════════════════════════════════╗\n");
  Serial.printf("║  📁 Archivo: %s\n", fileName.c_str());
  Serial.printf("║  📊 Tamaño: %u bytes (%.2f KB)\n", totalSize, totalSize / 1024.0);
  Serial.printf("║  📦 Chunks: %u\n", totalChunks);
  Serial.printf("║  🔀 Interleaving: %s\n", ENABLE_INTERLEAVING ? "ACTIVADO" : "DESACTIVADO");
  Serial.printf("║  🆔 File ID: 0x%08X\n", currentFileID);
  Serial.printf("╚════════════════════════════════════════╝\n\n");

  int dynamicDelay = getInterPacketDelay();

  Serial.printf("\n╔════════════════════════════════════════╗\n");
  Serial.printf("║       📡 TRANSMISIÓN ESP-NOW\n");
  Serial.printf("╚════════════════════════════════════════╝\n\n");

  Serial.println("📤 Enviando MANIFEST (5 repeticiones)...");
  for (int m = 0; m < MANIFEST_REPEAT; m++) {
    if (!sendManifest(currentFileID, totalSize, totalChunks, fileName)) {
      f.close();
      return false;
    }
    delay(dynamicDelay + 10);
  }
  Serial.println("✅ MANIFEST OK\n");
  delay(100);

  f.seek(0);

  uint8_t fecBlock[FEC_BLOCK_SIZE][CHUNK_SIZE_ESPNOW];
  size_t  fecLengths[FEC_BLOCK_SIZE];
  int     fecIndex = 0;
  uint16_t lastProgressPercent = 0;

  for (uint16_t i = 0; i < totalChunks; i++) {

    uint16_t index = i;

    f.seek((uint32_t)index * CHUNK_SIZE_ESPNOW);

    uint8_t buffer[CHUNK_SIZE_ESPNOW];
    size_t  bytesRead = f.read(buffer, CHUNK_SIZE_ESPNOW);
    if (bytesRead == 0) break;

    memcpy(fecBlock[fecIndex], buffer, bytesRead);
    fecLengths[fecIndex] = bytesRead;
    fecIndex++;

    if (!sendDataChunk(currentFileID, index, totalChunks, buffer, bytesRead)) {
      f.close();
      return false;
    }

    totalESPNowPacketsSent++;

    uint16_t currentPercent = ((i + 1) * 100) / totalChunks;
    if (currentPercent >= lastProgressPercent + 5 || i + 1 == totalChunks) {
      Serial.printf("📦 Progreso: %u/%u (%.1f%%)\n",
                    i + 1, totalChunks,
                    (float)(i + 1) * 100.0 / totalChunks);

      uint8_t bleProgress = currentPercent;
      sendProgress(bleProgress);
      lastProgressPercent = currentPercent;
    }

    delay(dynamicDelay);

    if (fecIndex == FEC_BLOCK_SIZE || i + 1 == totalChunks) {
      uint8_t parityData[CHUNK_SIZE_ESPNOW];
      memset(parityData, 0, CHUNK_SIZE_ESPNOW);

      size_t maxLen = 0;
      for (int j = 0; j < fecIndex; j++)
        if (fecLengths[j] > maxLen) maxLen = fecLengths[j];

      for (int j = 0; j < fecIndex; j++)
        for (size_t k = 0; k < fecLengths[j]; k++)
          parityData[k] ^= fecBlock[j][k];

      uint16_t blockIndex = i / FEC_BLOCK_SIZE;

      if (!sendParityChunk(currentFileID, blockIndex, parityData, maxLen))
        Serial.println("⚠️  Parity falló (continuando)");

      totalESPNowPacketsSent++;
      fecIndex = 0;
      delay(dynamicDelay);
    }

    if ((i + 1) % MANIFEST_INTERVAL == 0) {
      sendManifest(currentFileID, totalSize, totalChunks, fileName);
      delay(dynamicDelay + 10);
    }

    yield();
  }

  Serial.printf("\n🏁 Enviando FILE_END...\n");
  sendFileEnd(currentFileID, totalChunks);
  delay(200);

  f.close();

  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║     🎉 TRANSMISIÓN COMPLETA           ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("📊 Total paquetes: %u\n", totalESPNowPacketsSent);
  Serial.printf("📈 Fallos: %u\n", totalESPNowRetries);

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
    output, maxLen, &outputLen,
    (const unsigned char*)input.c_str(), input.length()
  );
  if (ret != 0) return 0;
  return outputLen;
}

// ════════════════════════════════════════════════════════════════
// 🔄 RESETEAR ESTADO DE TRANSFERENCIA BLE
// ════════════════════════════════════════════════════════════════

void resetTransferState() {
  if (currentFile) currentFile.close();
  currentState     = STATE_IDLE;
  currentFilename  = "";
  expectedFileSize = 0;
  transferredBytes = 0;
  expectedChunks   = 0;
  receivedChunks   = 0;
}