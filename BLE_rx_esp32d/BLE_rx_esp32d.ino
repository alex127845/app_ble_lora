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
// 🔧 CONFIGURACIÓN - ESP32-WROOM-32D
// ════════════════════════════════════════════════════════════════

// ⚠️ ESP32-WROOM-32D NO TIENE VEXT - Se ignora
// #define VEXT      3
// #define VEXT_ON   LOW

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN BLE
// ════════════════════════════════════════════════════════════════

#define DEVICE_NAME    "ESP32-WROOM-RX-Broadcast-ESPNOW"
#define SERVICE_UUID   "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_WRITE_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DATA_READ_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define PROGRESS_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26aa"

// ════════════════════════════════════════════════════════════════
// 🔧 PROTOCOLO BROADCAST
// ════════════════════════════════════════════════════════════════

#define CHUNK_SIZE_BLE    200
#define CHUNK_SIZE_ESPNOW 240  // ESP-NOW MTU optimizado
#define RX_TIMEOUT        120000
#define ESPNOW_MAX_PACKET_LEN 255
#define ESPNOW_RX_QUEUE_SIZE  24

#define MAX_CHUNKS      4096

#define FEC_BLOCK_SIZE  8
#define MANIFEST_REPEAT 5
#define REPEAT_COUNT    3
#define MAX_FEC_PASSES  3
#define MAX_TOLERABLE_GAPS 2  // Permite ensamblar con pocos gaps rellenados en cero (tradeoff calidad/completitud)

// Magic bytes
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

// Configuración ESP-NOW
int   currentPower = 20;      // dBm (máximo 20)
int   currentChannel = 1;     // Canal Wi-Fi (1-13)
int   currentRate = 1;        // Velocidad: 0=1Mbps, 1=2Mbps, 2=5.5Mbps, 3=11Mbps

// Estado de recepción ESP-NOW
volatile bool packetReceived  = false;
bool          receivingFile   = false;
uint32_t      currentFileID   = 0;
String        receivingFileName = "";
uint32_t      receivingFileSize = 0;
uint16_t      receivingChunkSize = 0;
uint16_t      totalChunks     = 0;
unsigned long lastPacketTime  = 0;
unsigned long receptionStartTime = 0;

uint8_t**  chunkBuffer   = nullptr;
bool*      chunkReceived = nullptr;
uint16_t*  chunkLengths  = nullptr;

// FEC — Parity buffers
#define MAX_PARITY_BLOCKS (MAX_CHUNKS / FEC_BLOCK_SIZE)
uint8_t**  parityBuffer   = nullptr;
bool*      parityReceived = nullptr;
uint16_t*  parityLengths  = nullptr;

// Estadísticas de recepción
uint16_t lastProgressPct = 0;

uint16_t receivedDataChunks   = 0;
uint16_t receivedParityChunks = 0;
uint16_t manifestCount        = 0;
uint16_t duplicateChunks      = 0;
uint8_t  currentRound         = 1;
uint16_t chunksByRound[REPEAT_COUNT]     = {0};
uint16_t duplicatesByRound[REPEAT_COUNT] = {0};
int16_t  avgRSSI              = 0;
float    avgSNR               = 0;
int      rssiCount            = 0;

#ifndef FILE_ID_COOLDOWN
// Cooldown corto para permitir retransmisiones rápidas del mismo archivo en pruebas.
#define FILE_ID_COOLDOWN       5000
#endif
uint32_t      lastProcessedFileID    = 0;
unsigned long lastFileCompletionTime = 0;

// Estadísticas de pipeline RX
volatile uint32_t packetsSeen               = 0;
volatile uint32_t packetsQueued             = 0;
volatile uint32_t packetsProcessed          = 0;
volatile uint32_t packetsDroppedInvalidLen  = 0;
volatile uint32_t packetsDroppedQueueFull   = 0;
volatile uint32_t packetsDroppedCRC         = 0;
volatile uint32_t packetsDroppedUnknownType = 0;
volatile uint32_t packetsDroppedBounds      = 0;
volatile uint32_t packetsDroppedManifest    = 0;
volatile uint32_t packetsDroppedFileMismatch = 0;

typedef struct {
  uint8_t mac[6];
  uint8_t data[ESPNOW_MAX_PACKET_LEN];
  uint16_t len;
  bool    ready;
} PendingRxPacket;

PendingRxPacket rxQueue[ESPNOW_RX_QUEUE_SIZE];
volatile uint8_t rxQueueHead = 0;
volatile uint8_t rxQueueTail = 0;
volatile uint8_t rxQueueCount = 0;
portMUX_TYPE rxQueueMux = portMUX_INITIALIZER_UNLOCKED;

// Objeto Preferences para persistencia
Preferences preferences;

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - BLE
// ════════════════════════════════════════════════════════════════

BLEServer*         pServer                 = NULL;
BLECharacteristic* pCmdCharacteristic      = NULL;
BLECharacteristic* pDataCharacteristic     = NULL;
BLECharacteristic* pProgressCharacteristic = NULL;
bool deviceConnected    = false;
bool oldDeviceConnected = false;

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - TRANSFERENCIA BLE
// ════════════════════════════════════════════════════════════════

enum TransferState { STATE_IDLE, STATE_DOWNLOADING };

TransferState currentState    = STATE_IDLE;
String        currentFilename = "";
File          currentFile;
uint32_t      expectedFileSize  = 0;
uint32_t      transferredBytes  = 0;

// ════════════════════════════════════════════════════════════════
// 📝 DECLARACIONES ADELANTADAS
// ════════════════════════════════════════════════════════════════

void setupLittleFS();
void setupBLE();
void setupESPNow();
void setupESPNowBuffers();
void applyESPNowConfig();
// void enableVext(bool on);  // ⚠️ NO DISPONIBLE EN ESP32-WROOM-32D

// Funciones para persistencia
void loadESPNowConfig();
void saveESPNowConfig();

void handleCommand(String command);
void sendResponse(String response);
void sendProgress(uint8_t percentage);

void listFiles();
void deleteFile(String filename);
void startDownload(String filename);
void sendFileInChunks(String filename);

void setESPNowConfig(String jsonStr);
void sendCurrentESPNowConfig();

void processESPNowPacket(uint8_t *mac, uint8_t *incomingData, int len);
bool dequeueESPNowPacket(PendingRxPacket &packet);
void printPacketDropStats(const char *context);
void handleManifest(uint8_t* data, size_t len);
void handleDataChunk(uint8_t* data, size_t len);
void handleParityChunk(uint8_t* data, size_t len);
void handleFileEnd(uint8_t* data, size_t len);
void assembleFile();
uint16_t recoverMissingChunks();
void cancelReception(String reason);
void resetReceptionBuffers();

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
// 🔌 ESP-NOW CALLBACK
// ════════════════════════════════════════════════════════════════

void OnDataRecv(const esp_now_recv_info *recv_info, const uint8_t *incomingData, int len) {
  if (len <= 0 || len > ESPNOW_MAX_PACKET_LEN || recv_info == nullptr || incomingData == nullptr) {
    portENTER_CRITICAL_ISR(&rxQueueMux);
    packetsDroppedInvalidLen++;
    portEXIT_CRITICAL_ISR(&rxQueueMux);
    return;
  }

  portENTER_CRITICAL_ISR(&rxQueueMux);
  packetsSeen++;
  if (rxQueueCount >= ESPNOW_RX_QUEUE_SIZE) {
    packetsDroppedQueueFull++;
    portEXIT_CRITICAL_ISR(&rxQueueMux);
    return;
  }

  PendingRxPacket &slot = rxQueue[rxQueueHead];
  memcpy(slot.mac, recv_info->src_addr, sizeof(slot.mac));
  memcpy(slot.data, incomingData, len);
  slot.len = (uint16_t)len;
  slot.ready = true;

  rxQueueHead = (rxQueueHead + 1) % ESPNOW_RX_QUEUE_SIZE;
  rxQueueCount++;
  packetsQueued++;
  portEXIT_CRITICAL_ISR(&rxQueueMux);

  packetReceived = true;
  lastPacketTime = millis();
}

bool dequeueESPNowPacket(PendingRxPacket &packet) {
  portENTER_CRITICAL(&rxQueueMux);
  if (rxQueueCount == 0) {
    portEXIT_CRITICAL(&rxQueueMux);
    return false;
  }

  PendingRxPacket &slot = rxQueue[rxQueueTail];
  packet = slot;
  slot.ready = false;
  rxQueueTail = (rxQueueTail + 1) % ESPNOW_RX_QUEUE_SIZE;
  rxQueueCount--;
  portEXIT_CRITICAL(&rxQueueMux);
  return true;
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
      Serial.println("⚠️  Transferencia BLE interrumpida");
      if (currentFile) currentFile.close();
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
        Serial.println("\n📩 Comando BLE: " + command);
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

  Serial.println("\n════════════════════════════════════════════════════════");
  Serial.println("  📡 File Transfer System v5.0 RX BROADCAST");
  Serial.println("  ESP32-WROOM-32D");
  Serial.println("  MODO: RECEPTOR BROADCAST ESP-NOW");
  Serial.println("  ✅ CON PERSISTENCIA DE CONFIGURACIÓN");
  Serial.println("════════════════════════════════════════════════════════\n");

  setupLittleFS();

  // Inicializar buffers en heap ANTES de BLE y ESP-NOW
  setupESPNowBuffers();

  setupBLE();
  delay(1000);
  setupESPNow();

  // Cargar configuración guardada
  loadESPNowConfig();

  Serial.println("\n✅ Sistema RX BROADCAST ESP-NOW listo");
  Serial.println("👂 Esperando conexión BLE y broadcast ESP-NOW...\n");
}

// ════════════════════════════════════════════════════════════════
// 🔁 LOOP
// ════════════════════════════════════════════════════════════════

void loop() {
  PendingRxPacket packet;
  while (dequeueESPNowPacket(packet)) {
    processESPNowPacket(packet.mac, packet.data, packet.len);
    packetsProcessed++;
    yield();
  }
  portENTER_CRITICAL(&rxQueueMux);
  packetReceived = (rxQueueCount > 0);
  portEXIT_CRITICAL(&rxQueueMux);

  // Reconexión BLE
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("🔄 Esperando reconexión BLE...");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  // Timeout de recepción
  if (receivingFile && (millis() - lastPacketTime > RX_TIMEOUT)) {
    Serial.println("\n⏱️  Timeout — ensamblando con datos actuales");
    assembleFile();
  }

  yield();
  delay(1);
}

// ════════════════════════════════════════════════════════════════
// 💾 LITTLEFS
// ════════════════════════════════════════════════════════════════

void setupLittleFS() {
  Serial.println("💾 Inicializando LittleFS...");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while (1) delay(1000);
  }

  uint32_t total = LittleFS.totalBytes();
  uint32_t used  = LittleFS.usedBytes();

  Serial.printf("✅ LittleFS | Total: %.2f MB | Libre: %.2f MB\n",
                total / 1048576.0, (total - used) / 1048576.0);

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
// 🗃️  INICIALIZAR BUFFERS ESP-NOW EN HEAP
// ════════════════════════════════════════════════════════════════

void setupESPNowBuffers() {
  Serial.println("\n🗃️  Inicializando buffers ESP-NOW en heap...");

  chunkBuffer   = (uint8_t**)calloc(MAX_CHUNKS,       sizeof(uint8_t*));
  chunkReceived = (bool*)    calloc(MAX_CHUNKS,        sizeof(bool));
  chunkLengths  = (uint16_t*)calloc(MAX_CHUNKS,        sizeof(uint16_t));

  parityBuffer   = (uint8_t**)calloc(MAX_PARITY_BLOCKS, sizeof(uint8_t*));
  parityReceived = (bool*)    calloc(MAX_PARITY_BLOCKS, sizeof(bool));
  parityLengths  = (uint16_t*)calloc(MAX_PARITY_BLOCKS, sizeof(uint16_t));

  if (!chunkBuffer || !chunkReceived || !chunkLengths ||
      !parityBuffer || !parityReceived || !parityLengths) {
    Serial.println("❌ CRÍTICO: Sin RAM para buffers ESP-NOW");
    while (1) delay(1000);
  }

  Serial.printf("✅ Buffers OK | MAX_CHUNKS=%u | MAX_PARITY=%u\n",
                MAX_CHUNKS, MAX_PARITY_BLOCKS);
  Serial.printf("   RAM usada por arrays: ~%u KB\n",
                (MAX_CHUNKS * (sizeof(uint8_t*) + sizeof(bool) + sizeof(uint16_t)) +
                 MAX_PARITY_BLOCKS * (sizeof(uint8_t*) + sizeof(bool) + sizeof(uint16_t))) / 1024);
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

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("✅ BLE iniciado: " + String(DEVICE_NAME));
  Serial.println("   SERVICE:    " + String(SERVICE_UUID));
  Serial.println("   CMD_WRITE:  " + String(CMD_WRITE_UUID));
  Serial.println("   DATA_READ:  " + String(DATA_READ_UUID));
  Serial.println("   PROGRESS:   " + String(PROGRESS_UUID));
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════

// ⚠️ REMOVIDO: enableVext() no es compatible con ESP32-WROOM-32D

void setupESPNow() {
  Serial.println("\n📡 Inicializando ESP-NOW...");

  // ⚠️ NO LLAMAR enableVext() en ESP32-WROOM-32D
  // enableVext(true);
  // delay(200);

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

  // Registrar callback de recepción
  esp_now_register_recv_cb(OnDataRecv);
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE); // canal inicial antes de loadESPNowConfig
}

// ════════════════════════════════════════════════════════════════
// ✅ CARGAR CONFIGURACIÓN ESP-NOW DESDE MEMORIA FLASH
// ════════════════════════════════════════════════════════════════

void loadESPNowConfig() {
  Serial.println("\n💾 Cargando configuración ESP-NOW desde memoria flash...");
  
  preferences.begin("espnow-config", true);  // Modo solo lectura
  
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
  Serial.println("\n📻 Aplicando configuración ESP-NOW...");

  // Validar parámetros
  if (currentChannel < 1 || currentChannel > 13) currentChannel = 1;
  if (currentPower < 2 || currentPower > 20) currentPower = 20;
  if (currentRate < 0 || currentRate > 3) currentRate = 1;

  // Cambiar canal
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  delay(100);

  // NOTA: En ESP32-WROOM, la potencia se controla a nivel de ESP-NOW
  // No usar esp_wifi_set_max_tx_power en modo RX broadcast

  Serial.println("📻 Configuración ESP-NOW RX (ESP32-WROOM-32D):");
  Serial.printf("   Potencia: %d dBm (máximo)\n", currentPower);
  Serial.printf("   Canal: %d\n", currentChannel);
  Serial.printf("   Velocidad: %s\n",
    currentRate == 0 ? "1 Mbps" :
    currentRate == 1 ? "2 Mbps" :
    currentRate == 2 ? "5.5 Mbps" : "11 Mbps");
  Serial.println("✅ Configuración aplicada (RX BROADCAST)\n");
}

// ════════════════════════════════════════════════════════════════
// 🎯 MANEJO DE COMANDOS BLE
// ════════════════════════════════════════════════════════════════

void handleCommand(String command) {
  command.trim();

  if      (command == "CMD:LIST")                    listFiles();
  else if (command == "CMD:GET_MODE") {
    sendResponse("MODE:RX");
    Serial.println("📥 Modo identificado: RX");
  }
  else if (command.startsWith("CMD:DELETE:"))        deleteFile(command.substring(11));
  else if (command.startsWith("CMD:DOWNLOAD:"))      startDownload(command.substring(13));
  else if (command.startsWith("CMD:SET_ESPNOW_CONFIG:")) setESPNowConfig(command.substring(22));
  else if (command == "CMD:GET_ESPNOW_CONFIG")       sendCurrentESPNowConfig();
  else if (command == "CMD:PING")                    sendResponse("PONG");
  else {
    Serial.println("⚠️  Comando desconocido: " + command);
    sendResponse("ERROR:UNKNOWN_COMMAND");
  }
}

// ════════════════════════════════════════════════════════════════
// 📤 ENVIAR RESPUESTA / PROGRESO BLE
// ════════════════════════════════════════════════════════════════

void sendResponse(String response) {
  if (!deviceConnected || pDataCharacteristic == NULL) return;
  response += "\n";
  pDataCharacteristic->setValue(response.c_str());
  pDataCharacteristic->notify();
  delay(10);
}

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

  if (!LittleFS.exists(filename))                              { sendResponse("ERROR:FILE_NOT_FOUND"); return; }
  if (receivingFile && receivingFileName == filename)          { sendResponse("ERROR:FILE_IN_USE");    return; }
  if (currentState != STATE_IDLE && currentFilename == filename) { sendResponse("ERROR:FILE_IN_USE");  return; }

  delay(100);

  if (LittleFS.remove(filename)) {
    Serial.println("✅ Eliminado: " + filename);
    sendResponse("OK:DELETED");
  } else {
    sendResponse("ERROR:CANT_DELETE");
  }
}

// ════════════════════════════════════════════════════════════════
// 📥 DOWNLOAD BLE
// ════════════════════════════════════════════════════════════════

void startDownload(String filename) {
  if (currentState != STATE_IDLE) { sendResponse("ERROR:TRANSFER_IN_PROGRESS"); return; }

  if (!filename.startsWith("/")) filename = "/" + filename;
  if (!LittleFS.exists(filename))  { sendResponse("ERROR:FILE_NOT_FOUND"); return; }

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

void sendFileInChunks(String filename) {
  File file = LittleFS.open(filename, "r");
  if (!file) { sendResponse("ERROR:FILE_OPEN_FAILED"); resetTransferState(); return; }

  uint32_t totalSize   = file.size();
  uint16_t totalChunksLocal = (totalSize + CHUNK_SIZE_BLE - 1) / CHUNK_SIZE_BLE;
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
    if (chunkNum % 5 == 0 || chunkNum >= totalChunksLocal) sendProgress(progress);

    delay(20);
  }

  file.close();
  sendResponse("DOWNLOAD_END:" + String(transferredBytes));
  sendProgress(100);
  Serial.printf("✅ Download BLE: %u bytes\n", transferredBytes);
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

  if (receivingFile) {
    sendResponse("ERROR:RECEIVING");
    return;
  }

  if (doc.containsKey("power")) {
    int pwr = doc["power"].as<int>();
    if (pwr >= 2 && pwr <= 20) currentPower = pwr;
    else Serial.printf("⚠️  POWER inválido (%d), ignorado\n", pwr);
  }
  
  if (doc.containsKey("channel")) {
    int ch = doc["channel"].as<int>();
    if (ch >= 1 && ch <= 13) currentChannel = ch;
    else Serial.printf("⚠️  CHANNEL inválido (%d), ignorado\n", ch);
  }
  
  if (doc.containsKey("rate")) {
    int rate = doc["rate"].as<int>();
    if (rate >= 0 && rate <= 3) currentRate = rate;
    else Serial.printf("⚠️  RATE inválido (%d), ignorado\n", rate);
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
// 📡 ESP-NOW RX - PROCESAR PAQUETE
// ════════════════════════════════════════════════════════════════

void processESPNowPacket(uint8_t *mac, uint8_t *incomingData, int len) {
  (void)mac;
  if (len < 8) {
    packetsDroppedInvalidLen++;
    Serial.printf("⚠️  Paquete descartado: longitud inválida (%d)\n", len);
    return;
  }

  // Verificar CRC
  uint16_t crcRecv, crcCalc;
  memcpy(&crcRecv, incomingData + len - 2, 2);
  crcCalc = crc16_ccitt(incomingData, len - 2);

  if (crcRecv != crcCalc) {
    packetsDroppedCRC++;
    Serial.printf("⚠️  CRC inválido: recv=0x%04X calc=0x%04X len=%d\n", crcRecv, crcCalc, len);
    return;
  }

  // Desempaquetar tipo de paquete
  if      (incomingData[0] == MANIFEST_MAGIC_1  && incomingData[1] == MANIFEST_MAGIC_2)  
    handleManifest(incomingData, len);
  else if (incomingData[0] == DATA_MAGIC_1      && incomingData[1] == DATA_MAGIC_2)      
    handleDataChunk(incomingData, len);
  else if (incomingData[0] == PARITY_MAGIC_1    && incomingData[1] == PARITY_MAGIC_2)    
    handleParityChunk(incomingData, len);
  else if (incomingData[0] == FILE_END_MAGIC_1  && incomingData[1] == FILE_END_MAGIC_2)  
    handleFileEnd(incomingData, len);
  else {
    packetsDroppedUnknownType++;
    Serial.printf("⚠️  Tipo de paquete desconocido: 0x%02X 0x%02X\n", incomingData[0], incomingData[1]);
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW RX - MANIFEST
// ════════════════════════════════════════════════════════════════

void handleManifest(uint8_t* data, size_t len) {
  if (len < 18) {
    packetsDroppedManifest++;
    Serial.printf("⚠️  Manifest descartado: len inválido (%u)\n", (unsigned)len);
    return;
  }

  uint32_t fileID, fileSize;
  uint16_t totalChunksRx, chunkSize;
  uint8_t  nameLen;

  size_t idx = 2;
  memcpy(&fileID,        data + idx, 4); idx += 4;
  memcpy(&fileSize,      data + idx, 4); idx += 4;
  memcpy(&totalChunksRx, data + idx, 2); idx += 2;
  memcpy(&chunkSize,     data + idx, 2); idx += 2;
  nameLen = data[idx++];

  if (nameLen == 0 || nameLen > 100 || len < idx + nameLen + 2) {
    packetsDroppedManifest++;
    Serial.println("⚠️  Manifest descartado: nombre/longitud inválidos");
    return;
  }

  char fileName[101];
  memcpy(fileName, data + idx, nameLen);
  fileName[nameLen] = '\0';

  manifestCount++;
  uint8_t detectedRound = (uint8_t)(((manifestCount - 1) / MANIFEST_REPEAT) + 1);
  if (detectedRound < 1) detectedRound = 1;
  if (detectedRound > REPEAT_COUNT) detectedRound = REPEAT_COUNT;

  if (fileID == lastProcessedFileID &&
      (millis() - lastFileCompletionTime) < FILE_ID_COOLDOWN) {
    packetsDroppedManifest++;
    Serial.printf("ℹ️  Manifest ignorado por cooldown (%lums restantes)\n",
                  FILE_ID_COOLDOWN - (millis() - lastFileCompletionTime));
    return;
  }

  if (receivingFile && currentFileID == fileID) {
    if (totalChunksRx != totalChunks) {
      packetsDroppedManifest++;
      Serial.printf("⚠️  Manifest inconsistente para FileID 0x%08X: chunks %u!=%u\n",
                    fileID, totalChunksRx, totalChunks);
      return;
    }
    if (chunkSize != receivingChunkSize) {
      packetsDroppedManifest++;
      Serial.printf("⚠️  Manifest inconsistente para FileID 0x%08X: chunkSize %u!=%u\n",
                    fileID, chunkSize, receivingChunkSize);
      return;
    }
    if (fileSize != receivingFileSize) {
      packetsDroppedManifest++;
      Serial.printf("⚠️  Manifest inconsistente para FileID 0x%08X: fileSize %u!=%u\n",
                    fileID, fileSize, receivingFileSize);
      return;
    }
    if (detectedRound > currentRound) {
      currentRound = detectedRound;
      uint16_t totalNow = 0;
      for (int r = 0; r < REPEAT_COUNT; r++) totalNow += chunksByRound[r];
      float completeness = (totalChunks > 0) ? (totalNow * 100.0f / totalChunks) : 0.0f;
      Serial.printf("🔁 Nueva vuelta detectada: %u/%u | Completitud acumulada: %.1f%%\n",
                    currentRound, REPEAT_COUNT, completeness);
    } else {
      Serial.printf("🔁 Manifest duplicado (vuelta %u)\n", currentRound);
    }
    return;
  }

  if (receivingFile && currentFileID != fileID) {
    Serial.println("\n⚠️  Nuevo FileID — ensamblando archivo anterior");
    assembleFile();
    delay(200);
  }

  if (!receivingFile) {
    if (totalChunksRx == 0 || totalChunksRx > MAX_CHUNKS) {
      packetsDroppedManifest++;
      Serial.printf("❌ totalChunks inválido: %u (max %u)\n", totalChunksRx, MAX_CHUNKS);
      return;
    }
    if (chunkSize == 0 || chunkSize > CHUNK_SIZE_ESPNOW) {
      packetsDroppedManifest++;
      Serial.printf("❌ chunkSize inválido: %u (max %u)\n", chunkSize, CHUNK_SIZE_ESPNOW);
      return;
    }

    uint32_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (fileSize > freeSpace) {
      Serial.printf("❌ Sin espacio: necesito %u KB, libre %u KB\n",
                    fileSize / 1024, freeSpace / 1024);
      sendResponse("RX_FAILED:NO_SPACE");
      return;
    }

    currentFileID        = fileID;
    receivingFileName    = "/" + String(fileName);
    receivingFileSize    = fileSize;
    receivingChunkSize   = chunkSize;
    totalChunks          = totalChunksRx;
    receivingFile        = true;
    receptionStartTime   = millis();

    resetReceptionBuffers();

    receivedDataChunks   = 0;
    receivedParityChunks = 0;
    duplicateChunks      = 0;
    currentRound         = detectedRound;
    for (int r = 0; r < REPEAT_COUNT; r++) {
      chunksByRound[r] = 0;
      duplicatesByRound[r] = 0;
    }
    rssiCount            = 0;

    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.printf("║  🆔 File ID:  0x%08X\n",             fileID);
    Serial.printf("║  📄 Archivo:  %s\n",                  fileName);
    Serial.printf("║  📊 Tamaño:   %u bytes (%.2f KB)\n",  fileSize, fileSize / 1024.0);
    Serial.printf("║  📦 Chunks:   %u\n",                  totalChunksRx);
    Serial.printf("║  📶 ESP-NOW RX (ESP32-WROOM-32D)\n");
    Serial.println("╚════════════════════════════════════════╝\n");

    String cleanName = String(fileName);
    sendResponse("RX_START:" + cleanName + ":" + String(fileSize));
    sendProgress(0);
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW RX - DATA CHUNK
// ════════════════════════════════════════════════════════════════

void handleDataChunk(uint8_t* data, size_t len) {
  if (len < 13 || !receivingFile) {
    packetsDroppedBounds++;
    return;
  }

  uint32_t fileID;
  uint16_t chunkIndex, totalChunksRx;

  size_t idx = 2;
  memcpy(&fileID,        data + idx, 4); idx += 4;
  memcpy(&chunkIndex,    data + idx, 2); idx += 2;
  memcpy(&totalChunksRx, data + idx, 2); idx += 2;

  if (fileID != currentFileID) {
    packetsDroppedFileMismatch++;
    return;
  }
  if (totalChunksRx == 0 || totalChunksRx > MAX_CHUNKS) {
    packetsDroppedBounds++;
    Serial.printf("⚠️  Chunk descartado: totalChunks del paquete inválido (%u)\n", totalChunksRx);
    return;
  }
  if (totalChunks == 0 || totalChunks > MAX_CHUNKS) {
    packetsDroppedBounds++;
    Serial.printf("⚠️  Chunk descartado: totalChunks interno inválido (%u)\n", totalChunks);
    return;
  }
  if (totalChunksRx != totalChunks) {
    packetsDroppedBounds++;
    Serial.printf("⚠️  Chunk descartado: totalChunks mismatch pkt=%u esperado=%u\n", totalChunksRx, totalChunks);
    return;
  }
  if (chunkIndex >= totalChunksRx || chunkIndex >= MAX_CHUNKS) {
    packetsDroppedBounds++;
    Serial.printf("⚠️  Chunk fuera de rango: idx=%u total=%u max=%u\n", chunkIndex, totalChunksRx, MAX_CHUNKS);
    return;
  }

  if (chunkReceived[chunkIndex]) {
    duplicateChunks++;
    uint8_t roundIdx = (currentRound > 0 && currentRound <= REPEAT_COUNT) ? (currentRound - 1) : 0;
    duplicatesByRound[roundIdx]++;
    return;
  }

  if (len <= idx + 2) {
    packetsDroppedBounds++;
    Serial.printf("⚠️  Chunk %u descartado: payload vacío\n", chunkIndex);
    return;
  }

  size_t dataLen = len - idx - 2;
  if (dataLen == 0 || dataLen > CHUNK_SIZE_ESPNOW) {
    packetsDroppedBounds++;
    Serial.printf("⚠️  Chunk %u descartado: dataLen inválido (%u)\n", chunkIndex, (unsigned)dataLen);
    return;
  }

  uint32_t fileOffset = (uint32_t)chunkIndex * receivingChunkSize;
  if (fileOffset >= receivingFileSize) {
    packetsDroppedBounds++;
    Serial.printf("⚠️  Chunk %u descartado: offset fuera de archivo (%u >= %u)\n",
                  chunkIndex, fileOffset, receivingFileSize);
    return;
  }
  if (fileOffset + dataLen > receivingFileSize) {
    dataLen = receivingFileSize - fileOffset;
    if (dataLen == 0) {
      packetsDroppedBounds++;
      Serial.printf("⚠️  Chunk %u descartado: recorte dejó 0 bytes\n", chunkIndex);
      return;
    }
  }

  chunkBuffer[chunkIndex] = (uint8_t*)malloc(dataLen);
  if (chunkBuffer[chunkIndex] != nullptr) {
    memcpy(chunkBuffer[chunkIndex], data + idx, dataLen);
    chunkLengths[chunkIndex]  = dataLen;
    chunkReceived[chunkIndex] = true;
    receivedDataChunks++;
    uint8_t roundIdx = (currentRound > 0 && currentRound <= REPEAT_COUNT) ? (currentRound - 1) : 0;
    chunksByRound[roundIdx]++;

    uint16_t pct = (receivedDataChunks * 100) / totalChunks;
    static unsigned long lastProgressTime = 0;
    if ((pct >= lastProgressPct + 5 || receivedDataChunks == totalChunks)
         && (millis() - lastProgressTime > 200)) {
      Serial.printf("📦 %u/%u (%.1f%%) | Dupes: %u\n",
                    receivedDataChunks, totalChunks,
                    (float)receivedDataChunks * 100.0 / totalChunks,
                    duplicateChunks);
      sendProgress((uint8_t)pct);
      lastProgressPct = pct;
      lastProgressTime = millis();
    }
  } else {
    Serial.printf("❌ malloc falló para chunk %u\n", chunkIndex);
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW RX - PARITY CHUNK (FEC)
// ════════════════════════════════════════════════════════════════

void handleParityChunk(uint8_t* data, size_t len) {
  if (len < 11 || !receivingFile) return;

  uint32_t fileID;
  uint16_t blockIndex;

  size_t idx = 2;
  memcpy(&fileID,     data + idx, 4); idx += 4;
  memcpy(&blockIndex, data + idx, 2); idx += 2;

  if (fileID != currentFileID)               return;
  if (blockIndex >= MAX_PARITY_BLOCKS)       return;
  if (parityReceived[blockIndex])            return;

  size_t dataLen = len - idx - 2;
  if (dataLen == 0 || dataLen > CHUNK_SIZE_ESPNOW) return;

  parityBuffer[blockIndex] = (uint8_t*)malloc(dataLen);
  if (parityBuffer[blockIndex] != nullptr) {
    memcpy(parityBuffer[blockIndex], data + idx, dataLen);
    parityLengths[blockIndex]  = dataLen;
    parityReceived[blockIndex] = true;
    receivedParityChunks++;
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW RX - FILE_END
// ════════════════════════════════════════════════════════════════

void handleFileEnd(uint8_t* data, size_t len) {
  if (len < 10 || !receivingFile) return;

  uint32_t fileID;
  memcpy(&fileID, data + 2, 4);
  if (fileID != currentFileID) return;

  Serial.println("\n🏁 FILE_END recibido");
  assembleFile();
}

// ════════════════════════════════════════════════════════════════
// 🔧 FEC RECOVERY
// ════════════════════════════════════════════════════════════════

uint16_t recoverMissingChunks() {
  Serial.println("\n🔧 FEC Recovery...");
  uint16_t recovered = 0;

  uint16_t numBlocks = (totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
  bool progress = true;
  uint8_t pass = 0;

  while (progress && pass < MAX_FEC_PASSES) {
    progress = false;
    pass++;
    for (uint16_t block = 0; block < numBlocks; block++) {
      if (!parityReceived[block]) continue;

      uint16_t baseIdx = block * FEC_BLOCK_SIZE;
      int      missing      = -1;
      int      missingCount = 0;

      for (int i = 0; i < FEC_BLOCK_SIZE && (baseIdx + i) < totalChunks; i++) {
        if (!chunkReceived[baseIdx + i]) {
          missing = i;
          missingCount++;
        }
      }

      if (missingCount != 1) continue;

      uint16_t missingIdx = baseIdx + missing;
      size_t   maxLen     = parityLengths[block];

      if (chunkBuffer[missingIdx] != nullptr) {
        free(chunkBuffer[missingIdx]);
        chunkBuffer[missingIdx] = nullptr;
      }
      chunkBuffer[missingIdx] = (uint8_t*)malloc(maxLen);
      if (!chunkBuffer[missingIdx]) continue;

      memcpy(chunkBuffer[missingIdx], parityBuffer[block], maxLen);

      for (int i = 0; i < FEC_BLOCK_SIZE && (baseIdx + i) < totalChunks; i++) {
        if (i == missing) continue;
        if (!chunkReceived[baseIdx + i]) continue;

        size_t xorLen = min(maxLen, (size_t)chunkLengths[baseIdx + i]);
        for (size_t k = 0; k < xorLen; k++)
          chunkBuffer[missingIdx][k] ^= chunkBuffer[baseIdx + i][k];
      }

      size_t recoveredLen = maxLen;
      if (missingIdx == (totalChunks - 1)) {
        size_t lastChunkOffset = (size_t)(totalChunks - 1) * CHUNK_SIZE_ESPNOW;
        if (receivingFileSize > lastChunkOffset) {
          size_t expectedLastLen = receivingFileSize - lastChunkOffset;
          recoveredLen = min(recoveredLen, expectedLastLen);
        }
      }

      chunkLengths[missingIdx]  = recoveredLen;
      chunkReceived[missingIdx] = true;
      receivedDataChunks++;
      recovered++;
      progress = true;

      Serial.printf("✅ Chunk %u recuperado (bloque FEC %u, pasada %u)\n", missingIdx, block, pass);
    }
  }

  if (recovered > 0) Serial.printf("🎉 %u chunk(s) recuperados con FEC\n", recovered);
  else               Serial.println("ℹ️  Sin chunks recuperables por FEC");
  return recovered;
}

void printPacketDropStats(const char *context) {
  uint32_t seen, queued, processed, dropLen, dropQueue, dropCrc, dropType, dropBounds, dropManifest, dropFile;
  portENTER_CRITICAL(&rxQueueMux);
  seen = packetsSeen;
  queued = packetsQueued;
  processed = packetsProcessed;
  dropLen = packetsDroppedInvalidLen;
  dropQueue = packetsDroppedQueueFull;
  dropCrc = packetsDroppedCRC;
  dropType = packetsDroppedUnknownType;
  dropBounds = packetsDroppedBounds;
  dropManifest = packetsDroppedManifest;
  dropFile = packetsDroppedFileMismatch;
  portEXIT_CRITICAL(&rxQueueMux);

  Serial.printf("📈 RX Stats (%s): seen=%lu queued=%lu processed=%lu | drops[len=%lu queue=%lu crc=%lu type=%lu bounds=%lu manifest=%lu file=%lu]\n",
                context,
                (unsigned long)seen, (unsigned long)queued, (unsigned long)processed,
                (unsigned long)dropLen, (unsigned long)dropQueue, (unsigned long)dropCrc,
                (unsigned long)dropType, (unsigned long)dropBounds, (unsigned long)dropManifest, (unsigned long)dropFile);
}

// ════════════════════════════════════════════════════════════════
// 📝 ENSAMBLAR ARCHIVO
// ════════════════════════════════════════════════════════════════

void assembleFile() {
  if (!receivingFile) return;

  Serial.println("\n═══════════════════════════════════════");
  Serial.println("📝 Ensamblando archivo...");

  uint16_t recoveredThisAssemble = recoverMissingChunks();

  uint16_t missingChunks = 0;
  for (uint16_t i = 0; i < totalChunks; i++)
    if (!chunkReceived[i]) missingChunks++;

  Serial.printf("📊 Recibidos: %u/%u | Perdidos: %u (%.1f%%) | Parity: %u | Dupes: %u\n",
                receivedDataChunks, totalChunks,
                missingChunks, (missingChunks * 100.0) / totalChunks,
                receivedParityChunks, duplicateChunks);

  if (missingChunks > 0 && missingChunks <= MAX_TOLERABLE_GAPS) {
    Serial.printf("⚠️  Tolerando %u gap(s) y rellenando con ceros...\n", missingChunks);
    uint16_t gapFillFailures = 0;
    for (uint16_t i = 0; i < totalChunks; i++) {
      if (chunkReceived[i]) continue;

      size_t expectedLen = receivingChunkSize;
      if (i == (totalChunks - 1)) {
        size_t lastChunkOffset = (size_t)(totalChunks - 1) * CHUNK_SIZE_ESPNOW;
        if (receivingFileSize > lastChunkOffset) {
          expectedLen = receivingFileSize - lastChunkOffset;
        }
      }

      chunkBuffer[i] = (uint8_t*)calloc(expectedLen, 1);
      if (chunkBuffer[i] != nullptr) {
        chunkLengths[i] = expectedLen;
        chunkReceived[i] = true;
      } else {
        gapFillFailures++;
      }
    }
    if (gapFillFailures > 0) {
      cancelReception("MEMORY_ALLOCATION_FAILED_DURING_GAP_FILL:" + String(gapFillFailures));
      return;
    }
    missingChunks = 0;
  } else if (missingChunks > 0) {
    cancelReception("MISSING_CHUNKS_AFTER_FEC:" + String(missingChunks));
    return;
  }

  if (LittleFS.exists(receivingFileName)) LittleFS.remove(receivingFileName);

  File outFile = LittleFS.open(receivingFileName, "w");
  if (!outFile) {
    cancelReception("CANT_CREATE");
    return;
  }

  uint32_t writtenBytes = 0;

  for (uint16_t i = 0; i < totalChunks; i++) {
    if (!chunkReceived[i] || chunkBuffer[i] == nullptr) continue;
    size_t written = outFile.write(chunkBuffer[i], chunkLengths[i]);
    writtenBytes += written;

    if (i % 32 == 0) yield();
  }

  outFile.flush();
  outFile.close();

  float receptionTime  = (millis() - receptionStartTime) / 1000.0;
  float completeness   = (receivedDataChunks * 100.0) / totalChunks;
  float speed          = (receptionTime > 0)
                         ? (writtenBytes * 8.0) / (receptionTime * 1000.0)
                         : 0;

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       🎉 ARCHIVO ENSAMBLADO           ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("📄 %s\n",               receivingFileName.c_str());
  Serial.printf("📊 %u / %u bytes\n",    writtenBytes, receivingFileSize);
  Serial.printf("📈 Completitud: %.1f%%\n", completeness);
  Serial.printf("🔧 Recuperados por FEC (ensamble actual): %u\n", recoveredThisAssemble);
  for (int r = 0; r < REPEAT_COUNT; r++) {
    Serial.printf("🔁 Vuelta %d: nuevos=%u, duplicados=%u\n",
                  r + 1, chunksByRound[r], duplicatesByRound[r]);
  }
  Serial.printf("⏱️  %.2f s | ⚡ %.2f kbps\n", receptionTime, speed);
  Serial.println("═══════════════════════════════════════\n");
  printPacketDropStats("RX_COMPLETE");

  String cleanName = receivingFileName;
  if (cleanName.startsWith("/")) cleanName = cleanName.substring(1);

  sendResponse("RX_COMPLETE:" + cleanName + ":" +
               String(writtenBytes) + ":" +
               String(receptionTime, 2) + ":" +
               String(completeness, 1));
  sendProgress(100);

  lastProcessedFileID    = currentFileID;
  lastFileCompletionTime = millis();

  resetReceptionBuffers();
  receivingFile = false;
  currentFileID = 0;

  Serial.println("👂 Esperando nueva transmisión...\n");
}

// ════════════════════════════════════════════════════════════════
// ❌ CANCELAR RECEPCIÓN
// ════════════════════════════════════════════════════════════════

void cancelReception(String reason) {
  Serial.println("❌ Cancelando recepción: " + reason);
  printPacketDropStats("RX_FAILED");
  sendResponse("RX_FAILED:" + reason);
  sendProgress(0);
  resetReceptionBuffers();
  receivingFile = false;
  currentFileID = 0;
  Serial.println("👂 Esperando nueva transmisión...\n");
}

// ════════════════════════════════════════════════════════════════
// 🔄 RESETEAR BUFFERS
// ════════════════════════════════════════════════════════════════

void resetReceptionBuffers() {
  for (uint16_t i = 0; i < MAX_CHUNKS; i++) {
    if (chunkBuffer[i] != nullptr) {
      free(chunkBuffer[i]);
      chunkBuffer[i] = nullptr;
    }
    chunkReceived[i] = false;
    chunkLengths[i]  = 0;
  }

  for (uint16_t i = 0; i < MAX_PARITY_BLOCKS; i++) {
    if (parityBuffer[i] != nullptr) {
      free(parityBuffer[i]);
      parityBuffer[i] = nullptr;
    }
    parityReceived[i] = false;
    parityLengths[i]  = 0;
  }

  receivedDataChunks   = 0;
  receivedParityChunks = 0;
  manifestCount        = 0;
  duplicateChunks      = 0;
  currentRound         = 1;
  for (int r = 0; r < REPEAT_COUNT; r++) {
    chunksByRound[r] = 0;
    duplicatesByRound[r] = 0;
  }
  lastProgressPct = 0;
}

// ════════════════════════════════════════════════════════════════
// 🔐 BASE64
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

size_t decodeBase64(String input, uint8_t* output, size_t maxLen) {
  size_t outputLen;
  int ret = mbedtls_base64_decode(
    output, maxLen, &outputLen,
    (const unsigned char*)input.c_str(), input.length()
  );
  return (ret == 0) ? outputLen : 0;
}

// ════════════════════════════════════════════════════════════════
// 🔄 RESETEAR ESTADO TRANSFERENCIA BLE
// ════════════════════════════════════════════════════════════════

void resetTransferState() {
  if (currentFile) currentFile.close();
  currentState     = STATE_IDLE;
  currentFilename  = "";
  expectedFileSize = 0;
  transferredBytes = 0;
}
