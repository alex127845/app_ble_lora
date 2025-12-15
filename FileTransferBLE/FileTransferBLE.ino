/**
 * ════════════════════════════════════════════════════════════════
 * 📡 File Transfer System - Heltec WiFi LoRa 32 V3 (ESP32-S3)
 * ════════════════════════════════════════════════════════════════
 * 
 * Sistema completo de gestión y transferencia de archivos vía BLE
 * 
 * Características:
 * - LittleFS para almacenamiento persistente
 * - BLE con protocolo personalizado de comandos
 * - Transferencia por chunks con validación
 * - Progress tracking en tiempo real
 * - Manejo robusto de errores
 * 
 * @author alex127845
 * @date 2025-01-21
 * @version 2.0
 */

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <mbedtls/base64.h>

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN
// ════════════════════════════════════════════════════════════════

#define DEVICE_NAME "Heltec-FileManager"
#define CHUNK_SIZE 200  // Tamaño de chunk en bytes
#define MAX_FILENAME_LENGTH 64
#define ACK_TIMEOUT 5000 // Timeout para ACK en ms

// UUIDs del servicio BLE
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_WRITE_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DATA_READ_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define PROGRESS_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26aa"

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES
// ════════════════════════════════════════════════════════════════

// BLE
BLEServer* pServer = NULL;
BLECharacteristic* pCmdCharacteristic = NULL;
BLECharacteristic* pDataCharacteristic = NULL;
BLECharacteristic* pProgressCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Estado de transferencia
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

void setupLittleFS();
void setupBLE();
void handleCommand(String command);
void sendResponse(String response);
void sendProgress(uint8_t percentage);
void listFiles();
void deleteFile(String filename);
void startUpload(String filename, uint32_t fileSize);
void receiveChunk(String base64Data);
void startDownload(String filename);
void sendFileInChunks(String filename);
String encodeBase64(uint8_t* data, size_t length);
size_t decodeBase64(String input, uint8_t* output, size_t maxLen);
void resetTransferState();

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
    
    // Limpiar estado de transferencia
    if (currentState != STATE_IDLE) {
      Serial.println("⚠️  Transferencia interrumpida, limpiando...");
      if (currentFile) currentFile.close();
      
      // Eliminar archivo incompleto en upload
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
    // Obtener valor directamente como String de Arduino
    String command = pCharacteristic->getValue().c_str();
    command.trim();
    
    if (command.length() > 0) {
      Serial.println("\n📩 Comando recibido: " + command);
      handleCommand(command);
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
  Serial.println("════════════════════════════════════════");
  Serial.println("  📡 File Transfer System v2.0");
  Serial.println("  Heltec WiFi LoRa 32 V3");
  Serial.println("════════════════════════════════════════");
  Serial.println();
  
  setupLittleFS();
  setupBLE();
  
  Serial.println("\n✅ Sistema listo");
  Serial.println("👂 Esperando conexión BLE...\n");
}

// ════════════════════════════════════════════════════════════════
// 🔁 LOOP
// ════════════════════════════════════════════════════════════════

void loop() {
  // Manejar reconexión BLE
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("🔄 Esperando reconexión...");
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
  
  delay(10);
}

// ════════════════════════════════════════════════════════════════
// 💾 LITTLEFS - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════

void setupLittleFS() {
  Serial.println("💾 Inicializando LittleFS...");
  
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    Serial.println("⚠️  Sistema no puede continuar");
    while(1) {
      delay(1000);
    }
  }
  
  Serial.println("✅ LittleFS montado correctamente");
  
  // Mostrar información del sistema de archivos
  uint32_t totalBytes = LittleFS.totalBytes();
  uint32_t usedBytes = LittleFS.usedBytes();
  uint32_t freeBytes = totalBytes - usedBytes;
  
  Serial.printf("   Total: %.2f MB\n", totalBytes / 1048576.0);
  Serial.printf("   Usado: %.2f MB (%.1f%%)\n", 
                usedBytes / 1048576.0, 
                (usedBytes * 100.0) / totalBytes);
  Serial.printf("   Libre: %.2f MB\n", freeBytes / 1048576.0);
  
  // Listar archivos existentes
  Serial.println("\n📁 Archivos existentes:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int count = 0;
  
  while (file) {
    if (!file.isDirectory()) {
      Serial.printf("   - %s (%.2f KB)\n", 
                   file.name(), 
                   file.size() / 1024.0);
      count++;
    }
    file = root.openNextFile();
  }
  
  if (count == 0) {
    Serial.println("   (vacío)");
  } else {
    Serial.printf("   Total: %d archivo(s)\n", count);
  }
}

// ════════════════════════════════════════════════════════════════
// 📡 BLE - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════

void setupBLE() {
  Serial.println("\n📡 Inicializando BLE...");
  
  // Inicializar BLE
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(517); // MTU máximo para mejor rendimiento
  
  // Crear servidor
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  // Crear servicio
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // Característica de comandos (WRITE)
  pCmdCharacteristic = pService->createCharacteristic(
    CMD_WRITE_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCmdCharacteristic->setCallbacks(new CmdCallbacks());
  
  // Característica de datos (READ/NOTIFY)
  pDataCharacteristic = pService->createCharacteristic(
    DATA_READ_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pDataCharacteristic->addDescriptor(new BLE2902());
  
  // Característica de progreso (NOTIFY)
  pProgressCharacteristic = pService->createCharacteristic(
    PROGRESS_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pProgressCharacteristic->addDescriptor(new BLE2902());
  
  // Iniciar servicio
  pService->start();
  
  // Iniciar advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.println("✅ BLE iniciado");
  Serial.printf("   Nombre: %s\n", DEVICE_NAME);
  Serial.printf("   UUID Servicio: %s\n", SERVICE_UUID);
  Serial.println("   Visible para emparejamiento");
}

// ════════════════════════════════════════════════════════════════
// 🎯 MANEJO DE COMANDOS
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
      
      Serial.printf("📤 Procesando: UPLOAD_START - %s (%u bytes)\n", 
                   filename.c_str(), fileSize);
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
  
  // Comando: PING (para verificar conexión)
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
// 📤 ENVIAR RESPUESTA
// ════════════════════════════════════════════════════════════════

void sendResponse(String response) {
  if (!deviceConnected || pDataCharacteristic == NULL) {
    Serial.println("⚠️  No conectado, respuesta no enviada");
    return;
  }
  
  response += "\n";
  pDataCharacteristic->setValue(response.c_str());
  pDataCharacteristic->notify();
  
  Serial.println("📨 Respuesta enviada: " + response);
  delay(10);
}

// ════════════════════════════════════════════════════════════════
// 📊 ENVIAR PROGRESO
// ════════════════════════════════════════════════════════════════

void sendProgress(uint8_t percentage) {
  if (!deviceConnected || pProgressCharacteristic == NULL) return;
  
  uint8_t data[1] = { percentage };
  pProgressCharacteristic->setValue(data, 1);
  pProgressCharacteristic->notify();
  
  Serial.printf("📊 Progreso: %d%%\n", percentage);
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
      if (filename.startsWith("/")) {
        filename = filename.substring(1);
      }
      
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
  if (!filename.startsWith("/")) {
    filename = "/" + filename;
  }
  
  // Verificar si existe
  if (!LittleFS.exists(filename)) {
    Serial.println("❌ Archivo no existe");
    sendResponse("ERROR:FILE_NOT_FOUND");
    return;
  }
  
  // Verificar que no está en uso
  if (currentState != STATE_IDLE && currentFilename == filename) {
    Serial.println("❌ Archivo en uso");
    sendResponse("ERROR:FILE_IN_USE");
    return;
  }
  
  // Eliminar
  if (LittleFS.remove(filename)) {
    Serial.println("✅ Archivo eliminado: " + filename);
    sendResponse("OK:DELETED");
  } else {
    Serial.println("❌ Error eliminando archivo");
    sendResponse("ERROR:DELETE_FAILED");
  }
}

// ════════════════════════════════════════════════════════════════
// 📤 INICIAR UPLOAD
// ════════════════════════════════════════════════════════════════

void startUpload(String filename, uint32_t fileSize) {
  if (currentState != STATE_IDLE) {
    Serial.println("❌ Transferencia ya en progreso");
    sendResponse("ERROR:TRANSFER_IN_PROGRESS");
    return;
  }
  
  if (!filename.startsWith("/")) {
    filename = "/" + filename;
  }
  
  // Verificar espacio disponible
  uint32_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (fileSize > freeSpace) {
    Serial.printf("❌ Espacio insuficiente (libre: %u, necesario: %u)\n", 
                 freeSpace, fileSize);
    sendResponse("ERROR:NO_SPACE");
    return;
  }
  
  // Eliminar archivo si existe
  if (LittleFS.exists(filename)) {
    LittleFS.remove(filename);
    Serial.println("🗑️  Archivo existente eliminado");
  }
  
  // Abrir archivo para escritura
  currentFile = LittleFS.open(filename, "w");
  if (!currentFile) {
    Serial.println("❌ Error creando archivo");
    sendResponse("ERROR:CREATE_FAILED");
    return;
  }
  
  // Configurar estado
  currentState = STATE_UPLOADING;
  currentFilename = filename;
  expectedFileSize = fileSize;
  transferredBytes = 0;
  expectedChunks = (fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  receivedChunks = 0;
  
  Serial.printf("✅ Upload iniciado\n");
  Serial.printf("   Archivo: %s\n", filename.c_str());
  Serial.printf("   Tamaño: %u bytes\n", fileSize);
  Serial.printf("   Chunks esperados: %u\n", expectedChunks);
  
  sendResponse("OK:UPLOAD_READY");
  sendProgress(0);
}

// ════════════════════════════════════════════════════════════════
// 📦 RECIBIR CHUNK
// ════════════════════════════════════════════════════════════════

void receiveChunk(String base64Data) {
  if (currentState != STATE_UPLOADING) {
    Serial.println("❌ No hay upload en progreso");
    sendResponse("ERROR:NOT_UPLOADING");
    return;
  }
  
  // Decodificar base64
  uint8_t buffer[CHUNK_SIZE + 10];
  size_t decodedLen = decodeBase64(base64Data, buffer, sizeof(buffer));
  
  if (decodedLen == 0) {
    Serial.println("❌ Error decodificando chunk");
    sendResponse("ERROR:DECODE_FAILED");
    return;
  }
  
  // Escribir al archivo
  size_t written = currentFile.write(buffer, decodedLen);
  if (written != decodedLen) {
    Serial.println("❌ Error escribiendo chunk");
    currentFile.close();
    LittleFS.remove(currentFilename);
    resetTransferState();
    sendResponse("ERROR:WRITE_FAILED");
    return;
  }
  
  transferredBytes += written;
  receivedChunks++;
  
  // Calcular progreso
  uint8_t progress = (transferredBytes * 100) / expectedFileSize;
  
  // Log cada 10 chunks o en el último
  if (receivedChunks % 10 == 0 || receivedChunks >= expectedChunks) {
    Serial.printf("📦 Chunk %u/%u (%.1f%%) - %u bytes\n", 
                 receivedChunks, expectedChunks, progress, transferredBytes);
    sendProgress(progress);
  }
  
  // Enviar ACK
  sendResponse("ACK:" + String(receivedChunks));
  
  // ¿Upload completo?
  if (receivedChunks >= expectedChunks || transferredBytes >= expectedFileSize) {
    currentFile.flush();
    currentFile.close();
    
    // Verificar tamaño final
    File checkFile = LittleFS.open(currentFilename, "r");
    size_t actualSize = checkFile.size();
    checkFile.close();
    
    Serial.printf("\n✅ Upload completado\n");
    Serial.printf("   Esperado: %u bytes\n", expectedFileSize);
    Serial.printf("   Recibido: %u bytes\n", actualSize);
    
    if (actualSize == expectedFileSize) {
      sendResponse("OK:UPLOAD_COMPLETE:" + String(actualSize));
      sendProgress(100);
    } else {
      Serial.println("⚠️  Advertencia: Tamaño no coincide");
      sendResponse("WARNING:SIZE_MISMATCH:" + String(actualSize));
    }
    
    resetTransferState();
  }
}

// ════════════════════════════════════════════════════════════════
// 📥 INICIAR DOWNLOAD
// ════════════════════════════════════════════════════════════════

void startDownload(String filename) {
  if (currentState != STATE_IDLE) {
    Serial.println("❌ Transferencia ya en progreso");
    sendResponse("ERROR:TRANSFER_IN_PROGRESS");
    return;
  }
  
  if (!filename.startsWith("/")) {
    filename = "/" + filename;
  }
  
  // Verificar si existe
  if (!LittleFS.exists(filename)) {
    Serial.println("❌ Archivo no existe");
    sendResponse("ERROR:FILE_NOT_FOUND");
    return;
  }
  
  // Abrir archivo
  File file = LittleFS.open(filename, "r");
  if (!file) {
    Serial.println("❌ Error abriendo archivo");
    sendResponse("ERROR:OPEN_FAILED");
    return;
  }
  
  uint32_t fileSize = file.size();
  file.close();
  
  Serial.printf("✅ Download iniciado\n");
  Serial.printf("   Archivo: %s\n", filename.c_str());
  Serial.printf("   Tamaño: %u bytes\n", fileSize);
  
  // Configurar estado
  currentState = STATE_DOWNLOADING;
  currentFilename = filename;
  expectedFileSize = fileSize;
  transferredBytes = 0;
  
  // Enviar metadata
  String cleanName = filename;
  if (cleanName.startsWith("/")) cleanName = cleanName.substring(1);
  
  sendResponse("DOWNLOAD_START:" + cleanName + ":" + String(fileSize));
  delay(100);
  
  // Enviar archivo en chunks
  sendFileInChunks(filename);
}

// ════════════════════════════════════════════════════════════════
// 📤 ENVIAR ARCHIVO EN CHUNKS
// ════════════════════════════════════════════════════════════════

void sendFileInChunks(String filename) {
  File file = LittleFS.open(filename, "r");
  if (!file) {
    sendResponse("ERROR:FILE_OPEN_FAILED");
    resetTransferState();
    return;
  }
  
  uint32_t totalSize = file.size();
  uint16_t totalChunks = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  uint16_t chunkNum = 0;
  
  Serial.printf("📦 Enviando %u chunks...\n", totalChunks);
  sendProgress(0);
  
  uint8_t buffer[CHUNK_SIZE];
  
  while (file.available()) {
    size_t bytesRead = file.read(buffer, CHUNK_SIZE);
    if (bytesRead == 0) break;
    
    // Codificar a base64
    String encoded = encodeBase64(buffer, bytesRead);
    
    // Enviar chunk
    String chunkMsg = "CHUNK:" + String(chunkNum) + ":" + encoded;
    sendResponse(chunkMsg);
    
    transferredBytes += bytesRead;
    chunkNum++;
    
    // Actualizar progreso
    uint8_t progress = (transferredBytes * 100) / totalSize;
    if (chunkNum % 5 == 0 || chunkNum >= totalChunks) {
      Serial.printf("📤 Enviado chunk %u/%u (%.1f%%)\n", 
                   chunkNum, totalChunks, progress);
      sendProgress(progress);
    }
    
    delay(20); // Pequeña pausa entre chunks
  }
  
  file.close();
  
  // Enviar fin
  sendResponse("DOWNLOAD_END:" + String(transferredBytes));
  sendProgress(100);
  
  Serial.printf("\n✅ Download completado: %u bytes enviados\n", transferredBytes);
  
  resetTransferState();
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
  
  if (ret != 0) {
    Serial.printf("❌ Error decodificando base64: %d\n", ret);
    return 0;
  }
  
  return outputLen;
}

// ════════════════════════════════════════════════════════════════
// 🔄 RESETEAR ESTADO
// ════════════════════════════════════════════════════════════════

void resetTransferState() {
  if (currentFile) {
    currentFile.close();
  }
  
  currentState = STATE_IDLE;
  currentFilename = "";
  expectedFileSize = 0;
  transferredBytes = 0;
  expectedChunks = 0;
  receivedChunks = 0;
  
  Serial.println("🔄 Estado reseteado");
}
