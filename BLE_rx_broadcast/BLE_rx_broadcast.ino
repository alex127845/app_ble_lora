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
#include <Preferences.h>  // ✅ NUEVO: Para persistir configuración

// ════════════════════════════════════════════════════════════════
// 🔧 PINES HELTEC WIFI LORA 32 V3
// ════════════════════════════════════════════════════════════════

#define LORA_CS   8
#define LORA_RST  12
#define LORA_BUSY 13
#define LORA_DIO1 14
#define VEXT      36
#define VEXT_ON   LOW

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN BLE
// ════════════════════════════════════════════════════════════════

#define DEVICE_NAME    "Heltec-RX-Broadcast"
#define SERVICE_UUID   "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_WRITE_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DATA_READ_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define PROGRESS_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26aa"

// ✅ NUEVO: Modo RX (NO definir IS_TX_MODE)
// #define IS_TX_MODE  // Comentado porque este es RX

// ════════════════════════════════════════════════════════════════
// 🔧 PROTOCOLO BROADCAST
// ════════════════════════════════════════════════════════════════

#define CHUNK_SIZE_BLE  200
#define CHUNK_SIZE_LORA 240
#define RX_TIMEOUT      60000

#define MAX_CHUNKS      4096

#define FEC_BLOCK_SIZE  8

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
// 🌐 VARIABLES GLOBALES - RADIO LORA
// ════════════════════════════════════════════════════════════════

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Parámetros LoRa
float currentBW     = 125.0;
int   currentSF     = 9;
int   currentCR     = 7;
int   currentREPEAT = 2;
int   currentPower  = 17;

// Estado de recepción LoRa
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

// FEC — Parity buffers (también en heap)
#define MAX_PARITY_BLOCKS (MAX_CHUNKS / FEC_BLOCK_SIZE)
uint8_t**  parityBuffer   = nullptr;
bool*      parityReceived = nullptr;
uint16_t*  parityLengths  = nullptr;

// Estadísticas de recepción
uint16_t receivedDataChunks   = 0;
uint16_t receivedParityChunks = 0;
uint16_t manifestCount        = 0;
uint16_t duplicateChunks      = 0;
int16_t  avgRSSI              = 0;
float    avgSNR               = 0;
int      rssiCount            = 0;

#define FILE_ID_COOLDOWN       60000
uint32_t      lastProcessedFileID    = 0;
unsigned long lastFileCompletionTime = 0;

// ✅ NUEVO: Objeto Preferences para persistencia
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
void setupLoRa();
void setupLoRaBuffers();
void applyLoRaConfig();
void enableVext(bool on);

// ✅ NUEVO: Funciones para persistencia
void loadLoRaConfig();
void saveLoRaConfig();

void handleCommand(String command);
void sendResponse(String response);
void sendProgress(uint8_t percentage);

void listFiles();
void deleteFile(String filename);
void startDownload(String filename);
void sendFileInChunks(String filename);

void setLoRaConfig(String jsonStr);
void sendCurrentLoRaConfig();

void processLoRaPacket();
void handleManifest(uint8_t* data, size_t len);
void handleDataChunk(uint8_t* data, size_t len);
void handleParityChunk(uint8_t* data, size_t len);
void handleFileEnd(uint8_t* data, size_t len);
void assembleFile();
void recoverMissingChunks();
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
// 📡 ISR LORA
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

  Serial.println("\n════════════════════════════════════════════════");
  Serial.println("  📡 File Transfer System v4.2 RX BROADCAST");
  Serial.println("  Heltec WiFi LoRa 32 V3");
  Serial.println("  MODO: RECEPTOR BROADCAST (SIN ACK)");
  Serial.println("  ✅ CON PERSISTENCIA DE CONFIGURACIÓN");
  Serial.println("════════════════════════════════════════════════\n");

  setupLittleFS();

  // Inicializar buffers en heap ANTES de BLE y LoRa
  setupLoRaBuffers();

  setupBLE();
  delay(1000);
  setupLoRa();

  // ✅ NUEVO: Cargar configuración guardada
  loadLoRaConfig();

  Serial.println("\n✅ Sistema RX BROADCAST listo");
  Serial.println("👂 Esperando conexión BLE y broadcast LoRa...\n");
}

// ════════════════════════════════════════════════════════════════
// 🔁 LOOP
// ════════════════════════════════════════════════════════════════

void loop() {
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

  // Procesar paquete LoRa
  if (packetReceived) {
    packetReceived = false;
    processLoRaPacket();
  }

  // Timeout de recepción
  if (receivingFile && (millis() - lastPacketTime > RX_TIMEOUT)) {
    Serial.println("\n⏱️  Timeout — ensamblando con datos actuales");
    assembleFile();
  }

  yield();
  delay(10);
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
// 🗃️  INICIALIZAR BUFFERS LORA EN HEAP
// ══════════════════════════════════════════════���═════════════════

void setupLoRaBuffers() {
  Serial.println("\n🗃️  Inicializando buffers LoRa en heap...");

  chunkBuffer   = (uint8_t**)calloc(MAX_CHUNKS,       sizeof(uint8_t*));
  chunkReceived = (bool*)    calloc(MAX_CHUNKS,        sizeof(bool));
  chunkLengths  = (uint16_t*)calloc(MAX_CHUNKS,        sizeof(uint16_t));

  parityBuffer   = (uint8_t**)calloc(MAX_PARITY_BLOCKS, sizeof(uint8_t*));
  parityReceived = (bool*)    calloc(MAX_PARITY_BLOCKS, sizeof(bool));
  parityLengths  = (uint16_t*)calloc(MAX_PARITY_BLOCKS, sizeof(uint16_t));

  if (!chunkBuffer || !chunkReceived || !chunkLengths ||
      !parityBuffer || !parityReceived || !parityLengths) {
    Serial.println("❌ CRÍTICO: Sin RAM para buffers LoRa");
    while (1) delay(1000);
  }

  Serial.printf("✅ Buffers OK | MAX_CHUNKS=%u | MAX_PARITY=%u\n",
                MAX_CHUNKS, MAX_PARITY_BLOCKS);
  Serial.printf("   RAM usada por arrays: ~%u KB\n",
                (MAX_CHUNKS * (sizeof(uint8_t*) + sizeof(bool) + sizeof(uint16_t)) +
                 MAX_PARITY_BLOCKS * (sizeof(uint8_t*) + sizeof(bool) + sizeof(uint16_t))) / 1024);
}

// ���═══════════════════════════════════════════════════════════════
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
    Serial.printf("❌ Error SX1262: %d\n", state);
    Serial.println("⚠️  Continuando sin LoRa...");
    return;
  }

  Serial.println("✅ SX1262 inicializado");

  radio.setDio1Action(setFlag);
}

// ═════��══════════════════════════════════════════════════════════
// ✅ NUEVO: CARGAR CONFIGURACIÓN LORA DESDE MEMORIA FLASH
// ════════════════════════════════════════════════════════════════

void loadLoRaConfig() {
  Serial.println("\n💾 Cargando configuración LoRa desde memoria flash...");
  
  preferences.begin("lora-config", true);  // Modo solo lectura
  
  currentBW     = preferences.getFloat("bw", 125.0);
  currentSF     = preferences.getInt("sf", 9);
  currentCR     = preferences.getInt("cr", 7);
  currentREPEAT = preferences.getInt("repeat", 2);
  currentPower  = preferences.getInt("power", 17);
  
  preferences.end();
  
  Serial.println("✅ Configuración LoRa cargada:");
  Serial.printf("   BW: %.0f kHz\n", currentBW);
  Serial.printf("   SF: %d\n", currentSF);
  Serial.printf("   CR: 4/%d\n", currentCR);
  Serial.printf("   REPEAT: %d\n", currentREPEAT);
  Serial.printf("   POWER: %d dBm\n", currentPower);
  
  applyLoRaConfig();
  
  // Iniciar recepción
  int state = radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("📻 Radio en RX continuo BROADCAST");
  } else {
    Serial.printf("❌ Error startReceive: %d\n", state);
  }
}

// ════════════════════════════════════════════════════════════════
// ✅ NUEVO: GUARDAR CONFIGURACIÓN LORA EN MEMORIA FLASH
// ════════════════════════════════════════════════════════════════

void saveLoRaConfig() {
  preferences.begin("lora-config", false);  // Modo escritura
  
  preferences.putFloat("bw", currentBW);
  preferences.putInt("sf", currentSF);
  preferences.putInt("cr", currentCR);
  preferences.putInt("repeat", currentREPEAT);
  preferences.putInt("power", currentPower);
  
  preferences.end();
  
  Serial.println("💾 ✅ Configuración LoRa guardada en memoria flash");
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA - APLICAR CONFIGURACIÓN
// ════════════════════════════════════════════════════════════════

void applyLoRaConfig() {
  Serial.println("\n📻 Aplicando configuración LoRa...");

  radio.standby();
  delay(100);

  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(currentPower);

  delay(100);

  Serial.printf("   BW: %.0f kHz | SF: %d | CR: 4/%d | Power: %d dBm\n",
                currentBW, currentSF, currentCR, currentPower);
  Serial.println("✅ Configuración aplicada (RX ONLY - SIN ACK)\n");
}

// ════════════════════════════════════════════════════════════════
// 🎯 MANEJO DE COMANDOS BLE
// ════════════════════════════════════════════════════════════════

void handleCommand(String command) {
  command.trim();

  if      (command == "CMD:LIST")                    listFiles();
  // ✅ NUEVO: Comando para identificar modo TX/RX
  else if (command == "CMD:GET_MODE") {
    #ifdef IS_TX_MODE
      sendResponse("MODE:TX");
      Serial.println("📤 Modo identificado: TX");
    #else
      sendResponse("MODE:RX");
      Serial.println("📥 Modo identificado: RX");
    #endif
  }
  else if (command.startsWith("CMD:DELETE:"))        deleteFile(command.substring(11));
  else if (command.startsWith("CMD:DOWNLOAD:"))      startDownload(command.substring(13));
  else if (command.startsWith("CMD:SET_LORA_CONFIG:")) setLoRaConfig(command.substring(20));
  else if (command == "CMD:GET_LORA_CONFIG")         sendCurrentLoRaConfig();
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
// ⚙️  CONFIGURACIÓN LORA - SET
// ✅ MEJORADO: Ahora guarda en memoria flash
// ════════════════════════════════════════════════════════════════

void setLoRaConfig(String jsonStr) {
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

  if (doc.containsKey("bw")) {
    float bw = doc["bw"].as<float>();
    if (bw == 125.0 || bw == 250.0 || bw == 500.0) currentBW = bw;
    else Serial.printf("⚠️  BW inválido (%.1f), ignorado\n", bw);
  }
  if (doc.containsKey("sf")) {
    int sf = doc["sf"].as<int>();
    if (sf >= 5 && sf <= 12) currentSF = sf;
    else Serial.printf("⚠️  SF inválido (%d), ignorado\n", sf);
  }
  if (doc.containsKey("cr")) {
    int cr = doc["cr"].as<int>();
    if (cr >= 5 && cr <= 8) currentCR = cr;
    else Serial.printf("⚠️  CR inválido (%d), ignorado\n", cr);
  }
  if (doc.containsKey("repeat")) {
    int rep = doc["repeat"].as<int>();
    if (rep >= 1 && rep <= 20) currentREPEAT = rep;
  }
  if (doc.containsKey("power")) {
    int pwr = doc["power"].as<int>();
    if (pwr >= 2 && pwr <= 22) currentPower = pwr;
    else Serial.printf("⚠️  POWER inválido (%d), ignorado\n", pwr);
  }

  // ✅ NUEVO: Guardar en memoria flash
  saveLoRaConfig();

  applyLoRaConfig();

  // Reiniciar recepción con nueva configuración
  radio.setDio1Action(setFlag);
  radio.startReceive();

  sendResponse("OK:LORA_CONFIG_SET");
  Serial.println("✅ Configuración LoRa actualizada y guardada");
}

// ════════════════════════════════════════════════════════════════
// ⚙️  CONFIGURACIÓN LORA - GET
// ════════════════════════════════════════════════════════════════

void sendCurrentLoRaConfig() {
  StaticJsonDocument<200> doc;
  doc["bw"]     = (int)currentBW;
  doc["sf"]     = currentSF;
  doc["cr"]     = currentCR;
  doc["repeat"] = currentREPEAT;
  doc["power"]  = currentPower;

  String json;
  serializeJson(doc, json);
  sendResponse("LORA_CONFIG:" + json);
  Serial.println("✅ Config LoRa enviada: " + json);
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - PROCESAR PAQUETE
// ════════════════════════════════════════════════════════════════

void processLoRaPacket() {
  uint8_t buffer[300];
  int     state = radio.readData(buffer, sizeof(buffer));

  if (state != RADIOLIB_ERR_NONE) {
    radio.startReceive();
    return;
  }

  size_t len = radio.getPacketLength();

  if (len < 8) { radio.startReceive(); return; }

  uint16_t crcRecv, crcCalc;
  memcpy(&crcRecv, buffer + len - 2, 2);
  crcCalc = crc16_ccitt(buffer, len - 2);

  if (crcRecv != crcCalc) {
    radio.startReceive();
    return;
  }

  int16_t rssi = (int16_t)radio.getRSSI();
  float   snr  = radio.getSNR();
  avgRSSI = (int16_t)((avgRSSI * rssiCount + rssi) / (rssiCount + 1));
  avgSNR  = (avgSNR  * rssiCount + snr)  / (rssiCount + 1);
  rssiCount++;

  lastPacketTime = millis();

  if      (buffer[0] == MANIFEST_MAGIC_1  && buffer[1] == MANIFEST_MAGIC_2)  handleManifest(buffer, len);
  else if (buffer[0] == DATA_MAGIC_1      && buffer[1] == DATA_MAGIC_2)      handleDataChunk(buffer, len);
  else if (buffer[0] == PARITY_MAGIC_1    && buffer[1] == PARITY_MAGIC_2)    handleParityChunk(buffer, len);
  else if (buffer[0] == FILE_END_MAGIC_1  && buffer[1] == FILE_END_MAGIC_2)  handleFileEnd(buffer, len);

  radio.startReceive();
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - MANIFEST
// ════════════════════════════════════════════════════════════════

void handleManifest(uint8_t* data, size_t len) {
  if (len < 18) return;

  uint32_t fileID, fileSize;
  uint16_t totalChunksRx, chunkSize;
  uint8_t  nameLen;

  size_t idx = 2;
  memcpy(&fileID,        data + idx, 4); idx += 4;
  memcpy(&fileSize,      data + idx, 4); idx += 4;
  memcpy(&totalChunksRx, data + idx, 2); idx += 2;
  memcpy(&chunkSize,     data + idx, 2); idx += 2;
  nameLen = data[idx++];

  if (nameLen == 0 || nameLen > 100 || len < idx + nameLen + 2) return;

  char fileName[101];
  memcpy(fileName, data + idx, nameLen);
  fileName[nameLen] = '\0';

  manifestCount++;

  if (fileID == lastProcessedFileID &&
      (millis() - lastFileCompletionTime) < FILE_ID_COOLDOWN) {
    return;
  }

  if (receivingFile && currentFileID == fileID) {
    Serial.printf("🔁 Manifest duplicado (vuelta %u)\n", manifestCount);
    return;
  }

  if (receivingFile && currentFileID != fileID) {
    Serial.println("\n⚠️  Nuevo FileID — ensamblando archivo anterior");
    assembleFile();
    delay(200);
  }

  if (!receivingFile) {
    if (totalChunksRx == 0 || totalChunksRx > MAX_CHUNKS) {
      Serial.printf("❌ totalChunks inválido: %u (max %u)\n", totalChunksRx, MAX_CHUNKS);
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

    receivedDataChunks   = 0;
    receivedParityChunks = 0;
    duplicateChunks      = 0;
    rssiCount            = 0;

    resetReceptionBuffers();

    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.printf("║  🆔 File ID:  0x%08X\n",             fileID);
    Serial.printf("║  📄 Archivo:  %s\n",                  fileName);
    Serial.printf("║  📊 Tamaño:   %u bytes (%.2f KB)\n",  fileSize, fileSize / 1024.0);
    Serial.printf("║  📦 Chunks:   %u\n",                  totalChunksRx);
    Serial.printf("║  📶 RSSI: %d dBm | SNR: %.2f dB\n",  (int)radio.getRSSI(), radio.getSNR());
    Serial.println("╚════════════════════════════════════════╝\n");

    String cleanName = String(fileName);
    sendResponse("RX_START:" + cleanName + ":" + String(fileSize));
    sendProgress(0);
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - DATA CHUNK
// ════════════════════════════════════════════════════════════════

void handleDataChunk(uint8_t* data, size_t len) {
  if (len < 13 || !receivingFile) return;

  uint32_t fileID;
  uint16_t chunkIndex, totalChunksRx;

  size_t idx = 2;
  memcpy(&fileID,        data + idx, 4); idx += 4;
  memcpy(&chunkIndex,    data + idx, 2); idx += 2;
  memcpy(&totalChunksRx, data + idx, 2); idx += 2;

  if (fileID != currentFileID)          return;
  if (chunkIndex >= totalChunks)        return;
  if (chunkIndex >= MAX_CHUNKS)         return;

  if (chunkReceived[chunkIndex]) {
    duplicateChunks++;
    return;
  }

  size_t dataLen = len - idx - 2;

  uint32_t fileOffset = (uint32_t)chunkIndex * receivingChunkSize;
  if (fileOffset + dataLen > receivingFileSize) {
    dataLen = receivingFileSize - fileOffset;
  }

  chunkBuffer[chunkIndex] = (uint8_t*)malloc(dataLen);
  if (chunkBuffer[chunkIndex] != nullptr) {
    memcpy(chunkBuffer[chunkIndex], data + idx, dataLen);
    chunkLengths[chunkIndex]  = dataLen;
    chunkReceived[chunkIndex] = true;
    receivedDataChunks++;

    static uint16_t lastPct = 0;
    uint16_t pct = (receivedDataChunks * 100) / totalChunks;
    if (pct >= lastPct + 5 || receivedDataChunks == totalChunks) {
      Serial.printf("📦 %u/%u (%.1f%%) | RSSI: %d | Dupes: %u\n",
                    receivedDataChunks, totalChunks,
                    (float)receivedDataChunks * 100.0 / totalChunks,
                    (int)radio.getRSSI(), duplicateChunks);
      sendProgress((uint8_t)pct);
      lastPct = pct;
    }
  } else {
    Serial.printf("❌ malloc falló para chunk %u\n", chunkIndex);
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - PARITY CHUNK (FEC)
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
  if (dataLen == 0 || dataLen > CHUNK_SIZE_LORA) return;

  parityBuffer[blockIndex] = (uint8_t*)malloc(dataLen);
  if (parityBuffer[blockIndex] != nullptr) {
    memcpy(parityBuffer[blockIndex], data + idx, dataLen);
    parityLengths[blockIndex]  = dataLen;
    parityReceived[blockIndex] = true;
    receivedParityChunks++;
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA RX - FILE_END
// ════════════════════════════════════════════════════════════════

void handleFileEnd(uint8_t* data, size_t len) {
  if (len < 10 || !receivingFile) return;

  uint32_t fileID;
  memcpy(&fileID, data + 2, 4);
  if (fileID != currentFileID) return;

  Serial.println("\n🏁 FILE_END recibido");
  Serial.println("⏳ Ventana 1s para chunks retrasados...");

  unsigned long waitStart = millis();
  while (millis() - waitStart < 1000) {
    if (packetReceived) {
      packetReceived = false;
      uint8_t buf[300];
      int st = radio.readData(buf, sizeof(buf));
      if (st == RADIOLIB_ERR_NONE) {
        size_t pLen = radio.getPacketLength();
        if (pLen >= 8) {
          uint16_t cR, cC;
          memcpy(&cR, buf + pLen - 2, 2);
          cC = crc16_ccitt(buf, pLen - 2);
          if (cR == cC && buf[0] == DATA_MAGIC_1 && buf[1] == DATA_MAGIC_2)
            handleDataChunk(buf, pLen);
        }
      }
      radio.startReceive();
    }
    yield();
    delay(5);
  }

  assembleFile();
}

// ════════════════════════════════════════════════════════════════
// 🔧 FEC RECOVERY
// ════════════════════════════════════════════════════════════════

void recoverMissingChunks() {
  Serial.println("\n🔧 FEC Recovery...");
  uint16_t recovered = 0;

  uint16_t numBlocks = (totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;

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

    chunkLengths[missingIdx]  = maxLen;
    chunkReceived[missingIdx] = true;
    receivedDataChunks++;
    recovered++;

    Serial.printf("✅ Chunk %u recuperado (bloque FEC %u)\n", missingIdx, block);
  }

  if (recovered > 0) Serial.printf("🎉 %u chunk(s) recuperados con FEC\n", recovered);
  else               Serial.println("ℹ️  Sin chunks recuperables por FEC");
}

// ════════════════════════════════════════════════════════════════
// 📝 ENSAMBLAR ARCHIVO
// ════════════════════════════════════════════════════════════════

void assembleFile() {
  if (!receivingFile) return;

  Serial.println("\n═══════════════════════════════════════");
  Serial.println("📝 Ensamblando archivo...");

  recoverMissingChunks();

  uint16_t missingChunks = 0;
  for (uint16_t i = 0; i < totalChunks; i++)
    if (!chunkReceived[i]) missingChunks++;

  Serial.printf("📊 Recibidos: %u/%u | Perdidos: %u (%.1f%%) | Parity: %u | Dupes: %u\n",
                receivedDataChunks, totalChunks,
                missingChunks, (missingChunks * 100.0) / totalChunks,
                receivedParityChunks, duplicateChunks);

  if (LittleFS.exists(receivingFileName)) LittleFS.remove(receivingFileName);

  File outFile = LittleFS.open(receivingFileName, "w");
  if (!outFile) {
    cancelReception("CANT_CREATE");
    return;
  }

  uint32_t writtenBytes = 0;

  for (uint16_t i = 0; i < totalChunks; i++) {
    if (chunkReceived[i] && chunkBuffer[i] != nullptr) {
      size_t written = outFile.write(chunkBuffer[i], chunkLengths[i]);
      writtenBytes += written;
    } else {
      uint32_t remaining = (receivingFileSize > writtenBytes)
                           ? receivingFileSize - writtenBytes
                           : 0;
      size_t fillSize = min((uint32_t)CHUNK_SIZE_LORA, remaining);

      if (fillSize > 0) {
        uint8_t zeros[CHUNK_SIZE_LORA] = {0};
        outFile.write(zeros, fillSize);
        writtenBytes += fillSize;
        Serial.printf("⚠️  Chunk %u perdido (relleno %u bytes con zeros)\n", i, fillSize);
      }
    }

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
  Serial.printf("⏱️  %.2f s | ⚡ %.2f kbps\n", receptionTime, speed);
  Serial.printf("📶 RSSI prom: %d dBm | SNR prom: %.2f dB\n", avgRSSI, avgSNR);
  Serial.println("═══════════════════════════════════════\n");

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
  sendResponse("RX_FAILED:" + reason);
  sendProgress(0);
  resetReceptionBuffers();
  receivingFile = false;
  currentFileID = 0;
  Serial.println("👂 Esperando nueva transmisión...\n");
}

// ════════════════════════════════════════════════════════════════
// 🔄 RESETEAR BUFFERS
// ════════════════════════════��═══════════════════════════════════

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