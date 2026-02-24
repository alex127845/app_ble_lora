#include <Arduino.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

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
// 🔧 PROTOCOLO
// ════════════════════════════════════════════════════════════════

#define MAX_PACKET_SIZE   300
#define RX_TIMEOUT        120000   // 120s sin paquetes → timeout
#define MAX_CHUNKS        4096     // Soporta hasta ~1 MB
#define FEC_BLOCK_SIZE    8
#define CHUNK_SIZE_LORA   240      // Debe coincidir con TX

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
// 🔧 WIFI AP
// ════════════════════════════════════════════════════════════════

const char* ssid     = "LoRa-RX-Broadcast";
const char* password = "12345678";

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES
// ════════════════════════════════════════════════════════════════

SX1262         radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
AsyncWebServer server(80);

volatile bool receivedFlag = false;

// Parámetros LoRa (deben coincidir con TX)
float currentBW = 125.0;
int   currentSF = 9;
int   currentCR = 7;

// Estadísticas globales
float    lastReceptionTime   = 0;
float    lastSpeed           = 0;
uint32_t lastFileSize        = 0;
uint32_t totalPacketsReceived = 0;
uint32_t totalCrcErrors      = 0;
uint32_t totalRecovered      = 0;
uint32_t totalDuplicates     = 0;

// ════════════════════════════════════════════════════════════════
// 📦 ESTRUCTURA DE SESIÓN
// ════════════════════════════════════════════════════════════════

struct FECBlock {
  bool     received = false;
  uint8_t* data     = nullptr;
  uint16_t length   = 0;
};

struct FileSession {
  uint32_t fileID          = 0;
  String   fileName        = "";
  String   tempFileName    = "";
  uint32_t totalSize       = 0;
  uint16_t totalChunks     = 0;
  uint16_t chunkSize       = 0;
  bool     active          = false;
  unsigned long lastPacketTime = 0;
  unsigned long startTime      = 0;

  bool*     chunkReceived       = nullptr;
  uint16_t  chunksReceivedCount = 0;

  FECBlock* parityBlocks    = nullptr;
  uint16_t  numParityBlocks = 0;
};

FileSession currentSession;
File        currentTempFile;

// ✅ FIX #1: Cooldown para ignorar reintentos del carrusel tras completar
//    Sin esto, si el TX envía N vueltas, el RX iniciará N sesiones del
//    mismo archivo sobrescribiendo el que ya guardó correctamente.
#define FILE_ID_COOLDOWN    60000   // 60s (aumentado de 30s)
uint32_t      lastProcessedFileID      = 0;
unsigned long lastFileCompletionTime   = 0;

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
// 📝 DECLARACIONES ADELANTADAS
// ════════════════════════════════════════════════════════════════

void enableVext(bool on);
void applyLoRaConfig();
void setupWebServer();
void freeFileSession();
bool initFileSession(uint32_t fileID, String fileName, uint32_t totalSize,
                     uint16_t totalChunks, uint16_t chunkSize);
void processManifest(uint8_t* data, size_t len);
void processDataChunk(uint8_t* data, size_t len);
void processParityChunk(uint8_t* data, size_t len);
void processFileEnd(uint8_t* data, size_t len);
void attemptFECRecovery();
void finalizeFile();

// ════════════════════════════════════════════════════════════════
// 🔌 ISR
// ════════════════════════════════════════════════════════════════

void IRAM_ATTR setFlag(void) {
  receivedFlag = true;
}

// ════════════════════════════════════════════════════════════════
// 💡 VEXT
// ════════════════════════════════════════════════════════════════

void enableVext(bool on) {
  pinMode(VEXT, OUTPUT);
  digitalWrite(VEXT, on ? VEXT_ON : !VEXT_ON);
}

// ════════════════════════════════════════════════════════════════
// 📡 LORA - CONFIGURAR
// ════════════════════════════════════════════════════════════════

void applyLoRaConfig() {
  Serial.println("📻 Configurando LoRa...");
  radio.standby();
  delay(100);

  radio.setSpreadingFactor(currentSF);
  radio.setBandwidth(currentBW);
  radio.setCodingRate(currentCR);
  radio.setSyncWord(0x12);
  radio.setOutputPower(17);

  delay(100);
  Serial.printf("   BW: %.0f kHz | SF: %d | CR: 4/%d\n", currentBW, currentSF, currentCR);
  Serial.println("✅ Radio configurado\n");
}

// ════════════════════════════════════════════════════════════════
// 🚀 SETUP
// ════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n=== RECEPTOR LoRa BROADCAST v8.2 FIXED ===");

  // LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("❌ Error montando LittleFS");
    while (1) delay(1000);
  }
  Serial.println("✅ LittleFS montado");
  Serial.printf("   Total: %.2f MB | Libre: %.2f MB\n",
                LittleFS.totalBytes() / 1048576.0,
                (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1048576.0);

  // Limpiar archivos .tmp de sesiones anteriores incompletas
  File root = LittleFS.open("/");
  File f    = root.openNextFile();
  while (f) {
    String name = String(f.name());
    if (name.endsWith(".tmp")) {
      Serial.println("🧹 Limpiando residuo: " + name);
      f.close();
      LittleFS.remove("/" + name);
    }
    f = root.openNextFile();
  }

  // WiFi AP
  Serial.println("\n📡 Configurando WiFi AP...");
  WiFi.softAP(ssid, password);
  Serial.printf("✅ WiFi AP: %s | IP: http://%s\n\n",
                ssid, WiFi.softAPIP().toString().c_str());

  setupWebServer();

  // LoRa
  Serial.println("📡 Iniciando radio SX1262...");
  enableVext(true);
  delay(200);

  int state = radio.begin(915.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error iniciando SX1262: %d\n", state);
    while (true) delay(1000);
  }

  applyLoRaConfig();
  radio.setDio1Action(setFlag);

  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("❌ Error startReceive: %d\n", state);
  }

  Serial.println("✅ Radio listo (RX ONLY - SIN ACK)");
  Serial.println("👂 Escuchando broadcast...\n");
}

// ════════════════════════════════════════════════════════════════
// 🔁 LOOP
// ════════════════════════════════════════════════════════════════

void loop() {
  static unsigned long lastDebugPrint = 0;

  if (millis() - lastDebugPrint > 10000) {
    if (currentSession.active) {
      Serial.printf("👂 RX activo | %u/%u chunks | FileID: 0x%08X\n",
                    currentSession.chunksReceivedCount,
                    currentSession.totalChunks,
                    currentSession.fileID);
    } else {
      Serial.println("👂 Esperando broadcast...");
    }
    lastDebugPrint = millis();
  }

  // Timeout de sesión
  if (currentSession.active &&
      (millis() - currentSession.lastPacketTime) > RX_TIMEOUT) {
    Serial.println("\n⚠️ TIMEOUT — Finalizando con lo recibido");
    attemptFECRecovery();
    finalizeFile();
  }

  if (receivedFlag) {
    receivedFlag = false;

    uint8_t buffer[MAX_PACKET_SIZE];
    int state = radio.readData(buffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      size_t packetLen = radio.getPacketLength();

      // Mínimo viable: 2 magic + 4 fileID + 2 CRC = 8 bytes
      if (packetLen < 8) {
        radio.startReceive();
        return;
      }

      // ✅ FIX #2: Verificar CRC ANTES de procesar cualquier campo.
      //    En el código original, algunos procesadores accedían a campos
      //    del paquete antes de verificar el CRC, lo que podía causar
      //    corrupción de estado con paquetes basura.
      uint16_t crcRecv, crcCalc;
      memcpy(&crcRecv, buffer + packetLen - 2, 2);
      crcCalc = crc16_ccitt(buffer, packetLen - 2);

      if (crcRecv != crcCalc) {
        totalCrcErrors++;
        radio.startReceive();
        return;
      }

      totalPacketsReceived++;

      // Dispatch por magic bytes
      if      (buffer[0] == MANIFEST_MAGIC_1  && buffer[1] == MANIFEST_MAGIC_2)  processManifest(buffer, packetLen);
      else if (buffer[0] == DATA_MAGIC_1      && buffer[1] == DATA_MAGIC_2)      processDataChunk(buffer, packetLen);
      else if (buffer[0] == PARITY_MAGIC_1    && buffer[1] == PARITY_MAGIC_2)    processParityChunk(buffer, packetLen);
      else if (buffer[0] == FILE_END_MAGIC_1  && buffer[1] == FILE_END_MAGIC_2)  processFileEnd(buffer, packetLen);

    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      totalCrcErrors++;
    }

    delay(5);
    radio.startReceive();
  }

  yield();
  delay(10);
}

// ════════════════════════════════════════════════════════════════
// 💾 INICIALIZAR SESIÓN
// ════════════════════════════════════════════════════════════════

bool initFileSession(uint32_t fileID, String fileName,
                     uint32_t totalSize, uint16_t totalChunks, uint16_t chunkSize) {
  freeFileSession();

  if (totalChunks == 0 || totalChunks > MAX_CHUNKS) {
    Serial.printf("❌ totalChunks inválido: %u\n", totalChunks);
    return false;
  }

  // ✅ FIX #3: Verificar espacio libre ANTES de crear el archivo.
  //    El código original podía crear el archivo y luego fallar a mitad
  //    del pre-llenado si no había espacio.
  uint32_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (totalSize > freeSpace) {
    Serial.printf("❌ Sin espacio: necesito %u KB, libre %u KB\n",
                  totalSize / 1024, freeSpace / 1024);
    return false;
  }

  currentSession.fileID          = fileID;
  currentSession.fileName        = fileName;
  currentSession.tempFileName    = fileName + ".tmp";
  currentSession.totalSize       = totalSize;
  currentSession.totalChunks     = totalChunks;
  currentSession.chunkSize       = chunkSize;
  currentSession.active          = true;
  currentSession.lastPacketTime  = millis();
  currentSession.startTime       = millis();
  currentSession.chunksReceivedCount = 0;

  // Flags de chunks recibidos
  currentSession.chunkReceived = (bool*)calloc(totalChunks, sizeof(bool));
  if (!currentSession.chunkReceived) {
    Serial.println("❌ Sin RAM para chunk flags");
    freeFileSession();
    return false;
  }

  // Bloques FEC (parity)
  currentSession.numParityBlocks = (totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
  currentSession.parityBlocks    = new FECBlock[currentSession.numParityBlocks];
  if (!currentSession.parityBlocks) {
    Serial.println("❌ Sin RAM para parity blocks");
    freeFileSession();
    return false;
  }

  // Limpiar posible residuo
  if (LittleFS.exists(currentSession.tempFileName))
    LittleFS.remove(currentSession.tempFileName);

  // Pre-llenar archivo con ceros para permitir seeks aleatorios
  {
    File tmpInit = LittleFS.open(currentSession.tempFileName, "w");
    if (!tmpInit) {
      Serial.println("❌ Error creando archivo temporal");
      freeFileSession();
      return false;
    }

    uint8_t zeros[256];
    memset(zeros, 0, sizeof(zeros));
    uint32_t written = 0;

    while (written < totalSize) {
      size_t toWrite = min((uint32_t)sizeof(zeros), totalSize - written);
      size_t w = tmpInit.write(zeros, toWrite);
      if (w == 0) {
        Serial.println("❌ Error en pre-llenado (sin espacio)");
        tmpInit.close();
        LittleFS.remove(currentSession.tempFileName);
        freeFileSession();
        return false;
      }
      written += w;
      yield();
    }

    tmpInit.flush();
    tmpInit.close();
  }

  // Abrir en modo r+ para seek+write durante la recepción
  currentTempFile = LittleFS.open(currentSession.tempFileName, "r+");
  if (!currentTempFile) {
    Serial.println("❌ Error abriendo temp file (r+)");
    LittleFS.remove(currentSession.tempFileName);
    freeFileSession();
    return false;
  }

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.printf("║  🆔 File ID:   0x%08X\n",       fileID);
  Serial.printf("║  📁 Archivo:   %s\n",            fileName.c_str());
  Serial.printf("║  📊 Tamaño:    %u bytes (%.2f KB)\n", totalSize, totalSize / 1024.0);
  Serial.printf("║  📦 Chunks:    %u\n",            totalChunks);
  Serial.printf("║  🛡️ Bloques FEC: %u\n",          currentSession.numParityBlocks);
  Serial.println("╚════════════════════════════════════════╝");
  Serial.println("✅ Sesión iniciada — listo para recibir chunks\n");

  return true;
}

// ════════════════════════════════════════════════════════════════
// 🧹 LIBERAR SESIÓN
// ════════════════════════════════════════════════════════════════

void freeFileSession() {
  // Cerrar archivo si está abierto
  if (currentTempFile) {
    currentTempFile.flush();
    currentTempFile.close();
    currentTempFile = File();
  }

  // Liberar parity blocks
  if (currentSession.parityBlocks) {
    for (uint16_t i = 0; i < currentSession.numParityBlocks; i++) {
      if (currentSession.parityBlocks[i].data) {
        free(currentSession.parityBlocks[i].data);
        currentSession.parityBlocks[i].data = nullptr;
      }
    }
    delete[] currentSession.parityBlocks;
    currentSession.parityBlocks = nullptr;
  }

  // Liberar chunk flags
  if (currentSession.chunkReceived) {
    free(currentSession.chunkReceived);
    currentSession.chunkReceived = nullptr;
  }

  // Reset estado
  currentSession.active              = false;
  currentSession.fileID              = 0;
  currentSession.fileName            = "";
  currentSession.tempFileName        = "";
  currentSession.totalSize           = 0;
  currentSession.totalChunks         = 0;
  currentSession.numParityBlocks     = 0;
  currentSession.chunksReceivedCount = 0;
}

// ════════════════════════════════════════════════════════════════
// 📋 PROCESAR MANIFEST
// ════════════════════════════════════════════════════════════════

void processManifest(uint8_t* data, size_t len) {
  // Header(2) + fileID(4) + totalSize(4) + totalChunks(2) + chunkSize(2) + nameLen(1) + min1char + CRC(2) = 18
  if (len < 18) return;

  uint32_t fileID;
  uint32_t totalSize;
  uint16_t totalChunks;
  uint16_t chunkSize;
  uint8_t  nameLen;

  size_t idx = 2;
  memcpy(&fileID,      data + idx, 4); idx += 4;
  memcpy(&totalSize,   data + idx, 4); idx += 4;
  memcpy(&totalChunks, data + idx, 2); idx += 2;
  memcpy(&chunkSize,   data + idx, 2); idx += 2;
  nameLen = data[idx++];

  if (nameLen == 0 || nameLen > 100) return;
  if (len < idx + nameLen + 2)       return;  // +2 = CRC al final

  char nameBuf[101];
  memcpy(nameBuf, data + idx, nameLen);
  nameBuf[nameLen] = '\0';

  String fileName = String(nameBuf);
  if (!fileName.startsWith("/")) fileName = "/" + fileName;

  // ✅ FIX #1a: Ignorar si este FileID ya fue completado recientemente.
  //    Previene que las vueltas extra del carrusel sobreescriban un archivo
  //    ya recibido y guardado correctamente.
  if (fileID == lastProcessedFileID) {
    if ((millis() - lastFileCompletionTime) < FILE_ID_COOLDOWN) {
      // Sin print para no saturar el serial con cada manifest repetido
      return;
    }
  }

  // Si ya tenemos una sesión activa con el mismo FileID → manifest duplicado
  if (currentSession.active && currentSession.fileID == fileID) {
    return;
  }

  // Si hay sesión activa con FileID DIFERENTE → nuevo archivo, finalizar anterior
  if (currentSession.active && currentSession.fileID != fileID) {
    Serial.println("\n⚠️ Nuevo FileID detectado — finalizando sesión anterior");
    attemptFECRecovery();
    finalizeFile();
    delay(200);
  }

  // Iniciar nueva sesión
  if (!currentSession.active) {
    Serial.printf("\n📋 MANIFEST — Nuevo archivo: %s (%u chunks)\n",
                  fileName.c_str(), totalChunks);
    if (!initFileSession(fileID, fileName, totalSize, totalChunks, chunkSize)) {
      Serial.println("❌ Error iniciando sesión");
    }
  }
}

// ════════════════════════════════════════════════════════════════
// 📦 PROCESAR DATA CHUNK
// ════════════════════════════════════════════════════════════════

void processDataChunk(uint8_t* data, size_t len) {
  // Header(2) + fileID(4) + chunkIndex(2) + totalChunks(2) + min1byte data + CRC(2) = 13
  if (len < 13) return;

  if (!currentSession.active) return;

  uint32_t fileID;
  uint16_t chunkIndex;
  uint16_t totalChunks;

  size_t idx = 2;
  memcpy(&fileID,      data + idx, 4); idx += 4;
  memcpy(&chunkIndex,  data + idx, 2); idx += 2;
  memcpy(&totalChunks, data + idx, 2); idx += 2;

  // Verificar FileID
  if (fileID != currentSession.fileID) return;

  // Verificar rango
  if (chunkIndex >= currentSession.totalChunks) {
    Serial.printf("⚠️ chunkIndex %u fuera de rango (max %u)\n",
                  chunkIndex, currentSession.totalChunks - 1);
    return;
  }

  currentSession.lastPacketTime = millis();

  // Duplicado
  if (currentSession.chunkReceived[chunkIndex]) {
    totalDuplicates++;
    return;
  }

  // ✅ FIX #4: Verificar que el archivo está abierto ANTES del seek.
  //    En el original se verificaba después del seek, perdiendo el mensaje
  //    de error útil y pudiendo crashear con puntero nulo.
  if (!currentTempFile) {
    Serial.println("❌ CRÍTICO: currentTempFile no está abierto — ignorando chunk");
    return;
  }

  size_t   dataLen    = len - idx - 2;   // -2 por CRC al final
  uint32_t fileOffset = (uint32_t)chunkIndex * currentSession.chunkSize;

  // ✅ FIX #5: Validar que el offset + dataLen no sobrepasa el tamaño del archivo.
  //    Sin esto, un paquete mal formado podía escribir más allá del EOF
  //    corrompiendo otros chunks o incluso LittleFS.
  if (fileOffset + dataLen > currentSession.totalSize) {
    Serial.printf("⚠️ Chunk %u: offset+len (%u) supera totalSize (%u), truncando\n",
                  chunkIndex, fileOffset + dataLen, currentSession.totalSize);
    dataLen = currentSession.totalSize - fileOffset;
  }

  if (!currentTempFile.seek(fileOffset)) {
    Serial.printf("❌ Seek falló en offset %u (chunk %u)\n", fileOffset, chunkIndex);
    return;
  }

  size_t written = currentTempFile.write(data + idx, dataLen);
  if (written != dataLen) {
    Serial.printf("❌ Escritura parcial chunk %u: %u/%u bytes\n",
                  chunkIndex, written, dataLen);
    return;
  }

  // Flush periódico para no perder datos si hay corte de alimentación
  if (chunkIndex % 16 == 0) currentTempFile.flush();

  currentSession.chunkReceived[chunkIndex] = true;
  currentSession.chunksReceivedCount++;

  // Progreso cada 5%
  static uint16_t lastProgressPercent = 0;
  uint16_t currentPercent = (currentSession.chunksReceivedCount * 100) / currentSession.totalChunks;
  if (currentPercent >= lastProgressPercent + 5 ||
      currentSession.chunksReceivedCount == currentSession.totalChunks) {
    Serial.printf("📦 %u/%u (%.1f%%) | RSSI: %.1f dBm | Dupes: %u\n",
                  currentSession.chunksReceivedCount,
                  currentSession.totalChunks,
                  (float)currentSession.chunksReceivedCount * 100.0 / currentSession.totalChunks,
                  radio.getRSSI(),
                  totalDuplicates);
    lastProgressPercent = currentPercent;
  }

  if (chunkIndex % 5 == 0) yield();
}

// ════════════════════════════════════════════════════════════════
// 🛡️ PROCESAR PARITY CHUNK (FEC)
// ════════════════════════════════════════════════════════════════

void processParityChunk(uint8_t* data, size_t len) {
  // Header(2) + fileID(4) + blockIndex(2) + min1byte + CRC(2) = 11
  if (len < 11) return;
  if (!currentSession.active) return;

  uint32_t fileID;
  uint16_t blockIndex;

  size_t idx = 2;
  memcpy(&fileID,     data + idx, 4); idx += 4;
  memcpy(&blockIndex, data + idx, 2); idx += 2;

  if (fileID != currentSession.fileID)          return;
  if (blockIndex >= currentSession.numParityBlocks) return;

  currentSession.lastPacketTime = millis();

  // Ya tenemos este bloque
  if (currentSession.parityBlocks[blockIndex].received) return;

  size_t dataLen = len - idx - 2;
  if (dataLen == 0 || dataLen > CHUNK_SIZE_LORA) return;

  currentSession.parityBlocks[blockIndex].data = (uint8_t*)malloc(dataLen);
  if (!currentSession.parityBlocks[blockIndex].data) {
    Serial.printf("❌ Sin RAM para parity block %u\n", blockIndex);
    return;
  }

  memcpy(currentSession.parityBlocks[blockIndex].data, data + idx, dataLen);
  currentSession.parityBlocks[blockIndex].length   = dataLen;
  currentSession.parityBlocks[blockIndex].received = true;

  yield();
}

// ════════════════════════════════════════════════════════════════
// 🏁 PROCESAR FILE_END
// ════════════════════════════════════════════════════════════════

void processFileEnd(uint8_t* data, size_t len) {
  // Header(2) + fileID(4) + totalChunks(2) + CRC(2) = 10
  if (len < 10) return;
  if (!currentSession.active) return;

  uint32_t fileID;
  memcpy(&fileID, data + 2, 4);

  if (fileID != currentSession.fileID) return;

  Serial.println("\n🏁 FILE_END recibido");

  // ✅ FIX #6: La espera bloqueante de 2s en el loop principal hace que
  //    el radio no llame a startReceive durante ese tiempo, perdiendo
  //    paquetes retrasados. Usar millis() para espera no bloqueante.
  Serial.println("⏳ Ventana de 2s para chunks retrasados...");
  unsigned long waitStart = millis();
  while (millis() - waitStart < 2000) {
    // Procesar cualquier paquete que llegue durante la espera
    if (receivedFlag) {
      receivedFlag = false;
      uint8_t buf[MAX_PACKET_SIZE];
      int st = radio.readData(buf, MAX_PACKET_SIZE);
      if (st == RADIOLIB_ERR_NONE) {
        size_t pLen = radio.getPacketLength();
        if (pLen >= 8) {
          uint16_t crcR, crcC;
          memcpy(&crcR, buf + pLen - 2, 2);
          crcC = crc16_ccitt(buf, pLen - 2);
          if (crcR == crcC && buf[0] == DATA_MAGIC_1 && buf[1] == DATA_MAGIC_2) {
            processDataChunk(buf, pLen);
          }
        }
      }
      radio.startReceive();
    }
    yield();
    delay(5);
  }

  attemptFECRecovery();
  finalizeFile();
}

// ════════════════════════════════════════════════════════════════
// 🛡️ FEC RECOVERY
// ════════════════════════════════════════════════════════════════

void attemptFECRecovery() {
  if (!currentSession.active) return;
  if (!currentTempFile)       return;

  uint16_t recovered = 0;
  Serial.println("\n🔧 FEC Recovery...");

  for (uint16_t block = 0; block < currentSession.numParityBlocks; block++) {
    if (!currentSession.parityBlocks[block].received) continue;

    uint16_t blockStart = block * FEC_BLOCK_SIZE;
    uint16_t blockEnd   = min((uint16_t)(blockStart + FEC_BLOCK_SIZE),
                               currentSession.totalChunks);

    // Contar chunks faltantes en este bloque
    int     missingCount = 0;
    int16_t missingIndex = -1;

    for (uint16_t i = blockStart; i < blockEnd; i++) {
      if (!currentSession.chunkReceived[i]) {
        missingCount++;
        missingIndex = (int16_t)i;
      }
    }

    // FEC solo puede recuperar exactamente 1 chunk por bloque
    if (missingCount != 1) continue;

    size_t   maxLen    = currentSession.parityBlocks[block].length;
    uint8_t* recov     = (uint8_t*)malloc(maxLen);
    if (!recov) continue;

    memcpy(recov, currentSession.parityBlocks[block].data, maxLen);

    // XOR con todos los chunks presentes del bloque
    uint8_t chunkBuf[CHUNK_SIZE_LORA + 4];
    bool    readError = false;

    for (uint16_t i = blockStart; i < blockEnd; i++) {
      if (!currentSession.chunkReceived[i]) continue;  // es el que falta

      uint32_t offset = (uint32_t)i * currentSession.chunkSize;
      if (!currentTempFile.seek(offset)) {
        readError = true;
        break;
      }

      // ✅ FIX #7: Leer exactamente el tamaño correcto del chunk.
      //    El último chunk puede ser más corto que chunkSize; usar
      //    min() para no leer basura más allá del archivo.
      size_t expectedLen = min((uint32_t)currentSession.chunkSize,
                               currentSession.totalSize - offset);
      size_t readLen = currentTempFile.read(chunkBuf, expectedLen);

      for (size_t j = 0; j < readLen && j < maxLen; j++) {
        recov[j] ^= chunkBuf[j];
      }
      yield();
    }

    if (readError) {
      Serial.printf("❌ Error leyendo bloque %u para FEC\n", block);
      free(recov);
      continue;
    }

    // Escribir chunk recuperado
    uint32_t offset = (uint32_t)missingIndex * currentSession.chunkSize;
    if (!currentTempFile.seek(offset)) {
      free(recov);
      continue;
    }

    size_t written = currentTempFile.write(recov, maxLen);
    if (written == maxLen) {
      currentSession.chunkReceived[missingIndex] = true;
      currentSession.chunksReceivedCount++;
      totalRecovered++;
      recovered++;
      Serial.printf("✅ Chunk %u recuperado (bloque FEC %u)\n", missingIndex, block);
    } else {
      Serial.printf("❌ Error escribiendo chunk recuperado %u\n", missingIndex);
    }

    free(recov);
    yield();
  }

  currentTempFile.flush();

  if (recovered > 0)
    Serial.printf("🛡️ %u chunk(s) recuperados con FEC\n", recovered);
  else
    Serial.println("ℹ️ Sin chunks recuperables por FEC");
}

// ════════════════════════════════════════════════════════════════
// 📝 FINALIZAR ARCHIVO
// ════════════════════════════════════════════════════════════════

void finalizeFile() {
  if (!currentSession.active) return;

  uint16_t missing = currentSession.totalChunks - currentSession.chunksReceivedCount;

  Serial.println("\n📝 Finalizando archivo...");
  Serial.printf("   Chunks recibidos: %u / %u\n",
                currentSession.chunksReceivedCount, currentSession.totalChunks);

  // ✅ FIX #8: Cerrar el archivo SIEMPRE antes de renombrar.
  //    LittleFS no puede renombrar un archivo que tiene un File handle abierto.
  //    En el original, si currentTempFile estaba en un estado inconsistente,
  //    el rename podía silenciosamente fallar y el archivo quedaba como .tmp.
  if (currentTempFile) {
    currentTempFile.flush();
    currentTempFile.close();
    currentTempFile = File();
    Serial.println("   Archivo temporal cerrado");
  }

  // Guardar info antes de liberar la sesión
  String finalName = currentSession.fileName;
  String tempName  = currentSession.tempFileName;
  uint32_t savedFileID = currentSession.fileID;
  unsigned long savedStartTime = currentSession.startTime;

  // Eliminar archivo final anterior si existía
  if (LittleFS.exists(finalName)) {
    LittleFS.remove(finalName);
  }

  // Renombrar .tmp → nombre final
  if (!LittleFS.rename(tempName, finalName)) {
    Serial.println("❌ Error en rename — intentando copia manual...");

    // ✅ FIX #9: Fallback manual si rename falla (puede ocurrir con LittleFS
    //    si el archivo está en un directorio distinto o el FS está fragmentado).
    File src = LittleFS.open(tempName, "r");
    File dst = LittleFS.open(finalName, "w");
    if (src && dst) {
      uint8_t copyBuf[256];
      while (src.available()) {
        size_t n = src.read(copyBuf, sizeof(copyBuf));
        dst.write(copyBuf, n);
        yield();
      }
      src.close();
      dst.close();
      LittleFS.remove(tempName);
      Serial.println("✅ Copia manual completada");
    } else {
      Serial.println("❌ Copia manual también falló");
      if (src) src.close();
      if (dst) dst.close();
    }
  }

  // Verificar resultado final
  if (LittleFS.exists(finalName)) {
    File result = LittleFS.open(finalName, "r");
    lastFileSize = result ? result.size() : 0;
    if (result) result.close();
  } else {
    lastFileSize = 0;
  }

  float duration = (millis() - savedStartTime) / 1000.0;
  lastReceptionTime = duration;
  lastSpeed = (duration > 0) ? (lastFileSize * 8.0) / (duration * 1000.0) : 0;

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       🎉 ARCHIVO GUARDADO             ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("📁 %s\n", finalName.c_str());
  Serial.printf("📊 %u bytes (%.2f KB)\n", lastFileSize, lastFileSize / 1024.0);

  if (currentSession.totalSize > 0) {
    Serial.printf("   Completitud: %.1f%%\n",
                  (lastFileSize * 100.0) / currentSession.totalSize);
  }

  Serial.printf("⏱️  %.2f s\n", lastReceptionTime);
  Serial.println("╔════════════════════════════════════════╗");
  Serial.printf("║  ⚡ VELOCIDAD: %.2f kbps              ║\n", lastSpeed);
  Serial.println("╚════════════════════════════════════════╝");

  if (missing > 0)
    Serial.printf("⚠️  %u chunks faltantes (%.1f%%) — archivo parcial\n",
                  missing, (missing * 100.0) / currentSession.totalChunks);
  else
    Serial.println("✅ Archivo 100% completo");

  if (totalRecovered > 0)  Serial.printf("🛡️  %u chunks recuperados con FEC\n", totalRecovered);
  if (totalDuplicates > 0) Serial.printf("🔁 %u duplicados ignorados\n",         totalDuplicates);
  Serial.printf("📊 Paquetes totales: %u | Errores CRC: %u\n\n",
                totalPacketsReceived, totalCrcErrors);

  // Registrar FileID como completado (para ignorar vueltas extra del carrusel)
  lastProcessedFileID    = savedFileID;
  lastFileCompletionTime = millis();

  freeFileSession();
}

// ════════════════════════════════════════════════════════════════
// 🌐 WEB SERVER
// ════════════════════════════════════════════════════════════════

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += "<title>LoRa RX v8.2</title>";
    html += "<style>";
    html += "body{font-family:Arial;background:linear-gradient(135deg,#667eea,#764ba2);padding:20px;margin:0}";
    html += ".box{max-width:900px;margin:0 auto;background:#fff;padding:30px;border-radius:15px;box-shadow:0 10px 40px rgba(0,0,0,.2)}";
    html += "h1{color:#333;border-bottom:3px solid #667eea;padding-bottom:15px}";
    html += ".sec{background:#f8f9fa;padding:20px;border-radius:10px;margin:20px 0}";
    html += ".sec h2{color:#667eea;margin-bottom:15px}";
    html += ".cfg{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:15px}";
    html += ".pg{background:#fff;padding:15px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,.05)}";
    html += ".pg label{display:block;font-weight:bold;color:#333;margin-bottom:8px;font-size:.9em}";
    html += ".pg select{width:100%;padding:10px;border:2px solid #667eea;border-radius:5px;font-size:14px}";
    html += ".btn{padding:12px 20px;border:none;border-radius:5px;cursor:pointer;font-weight:bold;width:100%;margin-top:10px}";
    html += ".btn-blue{background:#667eea;color:#fff}.btn-blue:hover{background:#5568d3}";
    html += ".btn-green{background:#28a745;color:#fff}.btn-green:hover{background:#218838}";
    html += ".btn-red{background:#dc3545;color:#fff}.btn-red:hover{background:#c82333}";
    html += ".badge{display:inline-block;background:#667eea;color:#fff;padding:4px 10px;border-radius:12px;margin:3px;font-size:.82em}";
    html += ".badge-rx{background:#ff6b6b}";
    html += ".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:10px;margin:15px 0}";
    html += ".sv{background:#fff;padding:12px;border-radius:8px;text-align:center;box-shadow:0 2px 5px rgba(0,0,0,.1)}";
    html += ".sv-val{font-size:1.5em;font-weight:bold;color:#667eea}";
    html += ".sv-lbl{color:#666;font-size:.78em;margin-top:4px}";
    html += ".sv-spd{background:linear-gradient(135deg,#f093fb,#f5576c);color:#fff}";
    html += ".sv-spd .sv-val{color:#fff}";
    html += ".fl{list-style:none;padding:0}";
    html += ".fi{background:#fff;padding:12px;margin:8px 0;border-radius:8px;display:flex;justify-content:space-between;align-items:center;box-shadow:0 2px 5px rgba(0,0,0,.08)}";
    html += ".fi-info{flex-grow:1}.fi-name{font-weight:bold}.fi-size{color:#666;font-size:.88em}";
    html += ".fi-btns{display:flex;gap:8px}";
    html += ".pbar-wrap{background:#eee;border-radius:10px;overflow:hidden;margin:10px 0}";
    html += ".pbar{background:linear-gradient(90deg,#667eea,#764ba2);height:28px;display:flex;align-items:center;justify-content:center;color:#fff;font-weight:bold;transition:width .3s}";
    html += ".warn{background:#fff3cd;border-left:4px solid #ffc107;padding:12px;border-radius:5px;color:#856404;margin:10px 0}";
    html += ".info{background:#d1ecf1;border-left:4px solid #0c5460;padding:12px;border-radius:5px;color:#0c5460;margin:10px 0}";
    html += "</style></head><body><div class='box'>";

    html += "<h1>🛰️ LoRa RX Broadcast v8.2 FIXED</h1>";

    html += "<div class='warn'>📻 <b>MODO BROADCAST:</b> Recepción sin ACK. Compatible con carrusel multi-vuelta.</div>";

    html += "<div class='info'>✅ CRC antes de procesar paquetes | ✅ FEC offset corregido | "
            "✅ Cooldown 60s anti-carrusel | ✅ Espera no bloqueante en FILE_END | "
            "✅ Fallback copia si rename falla | ✅ Validación offset antes de write</div>";

    // Configuración LoRa
    html += "<div class='sec'><h2>⚙️ Configuración LoRa</h2>";
    html += "<div style='margin-bottom:12px'>";
    html += "<span class='badge'>BW: " + String((int)currentBW) + " kHz</span>";
    html += "<span class='badge'>SF: " + String(currentSF) + "</span>";
    html += "<span class='badge'>CR: 4/" + String(currentCR) + "</span>";
    html += "<span class='badge badge-rx'>BROADCAST RX</span>";
    html += "</div>";

    if (!currentSession.active) {
      html += "<div class='cfg'>";
      html += "<div class='pg'><label>📶 Bandwidth</label><select name='bw' id='bw'>";
      html += "<option value='125'" + String(currentBW == 125.0 ? " selected" : "") + ">125 kHz</option>";
      html += "<option value='250'" + String(currentBW == 250.0 ? " selected" : "") + ">250 kHz</option>";
      html += "<option value='500'" + String(currentBW == 500.0 ? " selected" : "") + ">500 kHz</option>";
      html += "</select></div>";
      html += "<div class='pg'><label>📡 SF</label><select name='sf' id='sf'>";
      html += "<option value='7'"  + String(currentSF == 7  ? " selected" : "") + ">SF7</option>";
      html += "<option value='9'"  + String(currentSF == 9  ? " selected" : "") + ">SF9</option>";
      html += "<option value='12'" + String(currentSF == 12 ? " selected" : "") + ">SF12</option>";
      html += "</select></div>";
      html += "<div class='pg'><label>🔧 CR</label><select name='cr' id='cr'>";
      html += "<option value='5'" + String(currentCR == 5 ? " selected" : "") + ">4/5</option>";
      html += "<option value='7'" + String(currentCR == 7 ? " selected" : "") + ">4/7</option>";
      html += "<option value='8'" + String(currentCR == 8 ? " selected" : "") + ">4/8</option>";
      html += "</select></div>";
      html += "</div>";
      html += "<button class='btn btn-blue' onclick='applyConfig()'>✅ Aplicar configuración</button>";
    } else {
      html += "<p style='text-align:center;color:#856404'>🔒 Bloqueado durante recepción</p>";
    }
    html += "</div>";

    // Progreso de recepción activa
    if (currentSession.active && currentSession.totalChunks > 0) {
      float pct = (currentSession.chunksReceivedCount * 100.0) / currentSession.totalChunks;
      html += "<div class='warn'>📡 Recibiendo: <b>" + currentSession.fileName + "</b></div>";
      html += "<div class='pbar-wrap'><div class='pbar' style='width:" + String(pct, 1) + "%'>";
      html += String(currentSession.chunksReceivedCount) + "/" +
              String(currentSession.totalChunks) + " (" + String(pct, 1) + "%)";
      html += "</div></div>";
    }

    // Estadísticas última recepción
    if (lastReceptionTime > 0) {
      html += "<div class='sec'><h2>📊 Última recepción</h2><div class='stats'>";
      html += "<div class='sv'><div class='sv-val'>" + String(lastReceptionTime, 1) + "s</div><div class='sv-lbl'>Tiempo</div></div>";
      html += "<div class='sv'><div class='sv-val'>" + String(lastFileSize / 1024.0, 1) + " KB</div><div class='sv-lbl'>Tamaño</div></div>";
      html += "<div class='sv sv-spd'><div class='sv-val'>" + String(lastSpeed, 2) + "</div><div class='sv-lbl'>kbps ⚡</div></div>";
      html += "<div class='sv'><div class='sv-val'>" + String(totalPacketsReceived) + "</div><div class='sv-lbl'>Paquetes</div></div>";
      html += "<div class='sv'><div class='sv-val'>" + String(totalCrcErrors) + "</div><div class='sv-lbl'>CRC Err</div></div>";
      html += "<div class='sv'><div class='sv-val'>" + String(totalRecovered) + "</div><div class='sv-lbl'>FEC Fix</div></div>";
      html += "<div class='sv'><div class='sv-val'>" + String(totalDuplicates) + "</div><div class='sv-lbl'>Dupes</div></div>";
      html += "</div></div>";
    }

    // Lista de archivos
    html += "<div class='sec'><h2>📁 Archivos recibidos</h2><ul class='fl'>";

    File root = LittleFS.open("/");
    File file = root.openNextFile();
    bool hasFiles = false;

    while (file) {
      if (!file.isDirectory() && !String(file.name()).endsWith(".tmp")) {
        hasFiles = true;
        String name = String(file.name());
        if (name.startsWith("/")) name = name.substring(1);
        float sz = file.size() / 1024.0;
        String szStr = sz < 1.0
          ? String(file.size()) + " bytes"
          : String(sz, 2) + " KB";

        html += "<li class='fi'>";
        html += "<div class='fi-info'><div class='fi-name'>📄 " + name + "</div>";
        html += "<div class='fi-size'>" + szStr + "</div></div>";
        html += "<div class='fi-btns'>";
        html += "<button class='btn btn-green' style='width:auto' onclick='location.href=\"/download?file=" + name + "\"'>📥</button>";
        html += "<button class='btn btn-red'   style='width:auto' onclick='if(confirm(\"¿Eliminar " + name + "?\"))location.href=\"/delete?file=" + name + "\"'>🗑️</button>";
        html += "</div></li>";
      }
      file = root.openNextFile();
    }

    if (!hasFiles)
      html += "<li style='text-align:center;color:#999;padding:20px'>Sin archivos. Esperando broadcast...</li>";

    html += "</ul></div>";

    html += "<button class='btn btn-blue' onclick='location.reload()'>🔄 Actualizar</button>";

    html += "<script>"
            "function applyConfig(){"
            "const b=document.getElementById('bw').value;"
            "const s=document.getElementById('sf').value;"
            "const c=document.getElementById('cr').value;"
            "fetch('/config?bw='+b+'&sf='+s+'&cr='+c)"
            ".then(r=>r.text()).then(t=>{alert('✅ '+t);location.reload();});"
            "}";

    if (currentSession.active) html += "setTimeout(()=>location.reload(),3000);";

    html += "</script></div></body></html>";

    request->send(200, "text/html", html);
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (currentSession.active) {
      request->send(400, "text/plain", "No cambiar configuración durante recepción activa");
      return;
    }

    if (!request->hasParam("bw") || !request->hasParam("sf") || !request->hasParam("cr")) {
      request->send(400, "text/plain", "Faltan parámetros: bw, sf, cr");
      return;
    }

    // ✅ FIX #10: Validar parámetros del web server igual que el TX.
    //    En el original se asignaban sin validación, igual que el bug del TX.
    float bw = request->getParam("bw")->value().toFloat();
    int   sf = request->getParam("sf")->value().toInt();
    int   cr = request->getParam("cr")->value().toInt();

    if (bw != 125.0 && bw != 250.0 && bw != 500.0) {
      request->send(400, "text/plain", "BW inválido (acepta: 125, 250, 500)");
      return;
    }
    if (sf < 5 || sf > 12) {
      request->send(400, "text/plain", "SF inválido (acepta: 5-12)");
      return;
    }
    if (cr < 5 || cr > 8) {
      request->send(400, "text/plain", "CR inválido (acepta: 5-8)");
      return;
    }

    currentBW = bw;
    currentSF = sf;
    currentCR = cr;

    applyLoRaConfig();
    radio.setDio1Action(setFlag);

    int state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
      Serial.printf("❌ Error startReceive tras config: %d\n", state);
      request->send(500, "text/plain", "Radio error al reiniciar RX");
      return;
    }

    request->send(200, "text/plain",
                  "Configuración aplicada: BW=" + String((int)currentBW) +
                  " SF=" + String(currentSF) +
                  " CR=4/" + String(currentCR));
  });

  server.on("/download", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Falta parámetro: file");
      return;
    }

    String filename = request->getParam("file")->value();
    if (!filename.startsWith("/")) filename = "/" + filename;

    if (!LittleFS.exists(filename)) {
      request->send(404, "text/plain", "Archivo no encontrado");
      return;
    }

    String ct = "application/octet-stream";
    if      (filename.endsWith(".pdf"))  ct = "application/pdf";
    else if (filename.endsWith(".txt"))  ct = "text/plain";
    else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) ct = "image/jpeg";
    else if (filename.endsWith(".png"))  ct = "image/png";

    request->send(LittleFS, filename, ct, true);
  });

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Falta parámetro: file");
      return;
    }

    String filename = request->getParam("file")->value();
    if (!filename.startsWith("/")) filename = "/" + filename;

    if (LittleFS.remove(filename)) {
      request->redirect("/");
    } else {
      request->send(500, "text/plain", "No se pudo eliminar");
    }
  });

  server.begin();
  Serial.println("✅ Web server iniciado");
}
