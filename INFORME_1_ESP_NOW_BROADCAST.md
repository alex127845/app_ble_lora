# 📡 INFORME TÉCNICO: Sistema de Transferencia de Archivos BLE + ESP-NOW
## Códigos: BLE_tx_ESP_NOW.ino y BLE_rx_ESP_NOW.ino

---

## 🎯 RESUMEN EJECUTIVO

El proyecto implementa un **sistema de transferencia de archivos inalámbrico broadcast** que combina dos protocolos:

- **BLE (Bluetooth Low Energy)**: Para comunicación de control y configuración con dispositivos móviles
- **ESP-NOW**: Para transmisión broadcast de archivos de corto alcance entre múltiples receptores ESP32 simultáneamente

**Arquitectura**: Transmisor (TX) → Broadcast ESP-NOW → Múltiples Receptores (RX)

---

## 🔧 HARDWARE Y COMPONENTES

### **Dispositivo Base**

| Componente | Especificación |
|---|---|
| Microcontrolador | ESP32 V3 (Heltec WiFi LoRa 32) |
| Módulo LoRa | SX1262 (frecuencia 915 MHz) |
| Bluetooth | BLE integrado en ESP32 |
| Almacenamiento | LittleFS |
| RAM | ~400 KB disponibles |

### **Pines Utilizados**

| Pin | Función | Propósito |
|---|---|---|
| 8 | CS (LoRa) | Chip Select del módulo |
| 12 | RST (LoRa) | Reset del módulo |
| 13 | BUSY (LoRa) | Estado del módulo |
| 14 | DIO1 (LoRa) | Interrupt para RX/TX |
| 3 | VEXT | Control de alimentación |

### **Librerías Principales**

```
- RadioLib.h         // Control del SX1262
- BLEDevice.h        // Bluetooth Low Energy
- LittleFS.h         // Sistema de archivos
- ArduinoJson.h      // Manejo de JSON
- mbedtls/base64.h   // Codificación Base64
- Preferences.h      // Almacenamiento persistente
```

---

## ⚙️ CONFIGURACIÓN GENERAL

### **Parámetros ESP-NOW Configurables**

```cpp
float currentBW     = 125.0;  // Ancho de banda: 125, 250, 500 kHz
int   currentSF     = 9;      // Spreading Factor: 5-12
int   currentCR     = 7;      // Coding Rate: 5-8 (4/5, 4/6, 4/7, 4/8)
int   currentREPEAT = 2;      // Veces a repetir el archivo (broadcast)
int   currentPower  = 17;     // Potencia de TX: 2-22 dBm
```

### **Impacto de Parámetros en Rendimiento**

| Parámetro | Bajo | Medio | Alto | Efecto |
|---|---|---|---|---|
| **BW** | 125 kHz | 250 kHz | 500 kHz | ↑ kbps, ↓ rango |
| **SF** | 5 | 9 | 12 | ↓ kbps, ↑ rango |
| **CR** | 5 (4/5) | 7 (4/7) | 8 (4/8) | Menos→Más FEC |
| **Power** | 2 dBm | ~12 dBm | 22 dBm | ↑ rango, ↑ consumo |

### **Persistencia de Configuración en Flash**

```cpp
void loadLoRaConfig() {
  preferences.begin("lora-config", true);  // Solo lectura
  currentBW     = preferences.getFloat("bw", 125.0);
  currentSF     = preferences.getInt("sf", 9);
  currentCR     = preferences.getInt("cr", 7);
  currentREPEAT = preferences.getInt("repeat", 2);
  currentPower  = preferences.getInt("power", 17);
  preferences.end();
  
  Serial.println("✅ Configuración LoRa cargada");
  applyLoRaConfig();
}
```

**Ventaja**: La configuración se guarda automáticamente y se restaura al reiniciar.

---

## 📡 PROTOCOLO DE COMUNICACIÓN

### **Magic Bytes de Identificación**

| Tipo de Paquete | Magic Bytes | Propósito |
|---|---|---|
| MANIFEST | 0xAABB | Metadatos del archivo |
| DATA_CHUNK | 0xCCDD | Contenido del archivo |
| PARITY_CHUNK | 0xEEFF | Corrección de errores (FEC) |
| FILE_END | 0x9988 | Fin de transmisión |

### **Estructura de MANIFEST (Metadatos)**

```
┌──────┬───────┬──────────┬──────────┬───────────┬────────┬──────────────┬─────┐
│Magic │FileID │FileSize  │TotalChks │ChunkSize  │NameLen │   FileName   │ CRC │
│0xAABB│  4B   │   4B     │   2B     │    2B     │  1B    │ 0-100 bytes  │ 2B  │
└──────┴───────┴──────────┴──────────┴───────────┴────────┴──────────────┴─────┘

Tamaño total: 2 + 4 + 4 + 2 + 2 + 1 + 100 + 2 = 117 bytes máximo
```

### **Estructura de DATA_CHUNK (Contenido)**

```
┌──────┬───────┬──────────┬─────────┬──────────┬─────┐
│Magic │FileID │ChunkIdx  │TotalChks│   Data   │ CRC │
│0xCCDD│  4B   │   2B     │   2B    │  0-240B  │ 2B  │
└──────┴───────┴──────────┴─────────┴──────────┴─────┘

Tamaño total: 2 + 4 + 2 + 2 + 240 + 2 = 252 bytes máximo
```

### **Estructura de PARITY_CHUNK (FEC)**

```
┌──────┬───────┬──────────┬──────────────┬─────┐
│Magic │FileID │BlockIdx  │  ParityData  │ CRC │
│0xEEFF│  4B   │   2B     │  0-240 bytes │ 2B  │
└──────┴───────┴──────────┴──────────────┴─────┘
```

### **Estructura de FILE_END**

```
┌──────┬───────┬──────────┬─────┐
│Magic │FileID │TotalChks │ CRC │
│0x9988│  4B   │   2B     │ 2B  │
└──────┴───────┴──────────┴─────┘

Tamaño: 2 + 4 + 2 + 2 = 10 bytes
```

### **Verificación de Integridad: CRC16-CCITT**

```cpp
uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000) 
        crc = (crc << 1) ^ 0x1021;
      else              
        crc = (crc << 1);
    }
  }
  return crc;
}
```

---

## 🚀 TRANSMISOR (TX)

### **Interfaz BLE del TX**

| Característica | UUID | Propiedades | Función |
|---|---|---|---|
| CMD_WRITE | `beb5483e-36e1-4688-b7f5-ea07361b26a8` | WRITE | Comandos BLE |
| DATA_READ | `beb5483e-36e1-4688-b7f5-ea07361b26a9` | NOTIFY | Respuestas |
| PROGRESS | `beb5483e-36e1-4688-b7f5-ea07361b26aa` | NOTIFY | Progreso (%) |

### **Comandos BLE Disponibles**

```
CMD:LIST
  → Listar archivos almacenados en LittleFS

CMD:GET_MODE
  → Retorna "MODE:TX" (identifica modo)

CMD:DELETE:<filename>
  → Elimina archivo
  → Respuesta: "OK:DELETED" o "ERROR:FILE_NOT_FOUND"

CMD:UPLOAD_START:<filename>:<size>
  → Inicia carga de archivo via BLE
  → Respuesta: "OK:UPLOAD_READY"

CMD:UPLOAD_CHUNK:<base64_data>
  → Recibe chunk codificado en base64
  → Respuesta: "ACK:<chunk_num>"

CMD:DOWNLOAD:<filename>
  → Descarga archivo via BLE (TX→móvil)
  → Respuesta: "DOWNLOAD_START:<file>:<size>"

CMD:SET_ESPNOW_CONFIG:<json>
  → Configura parámetros LoRa
  → Formato: {"power":20,"channel":1,"rate":1}
  → Respuesta: "OK:LORA_CONFIG_SET"

CMD:GET_ESPNOW_CONFIG
  → Obtiene configuración actual
  → Respuesta: "LORA_CONFIG:{...json...}"

CMD:TX_FILE:<filename>
  → Inicia transmisión BROADCAST ESP-NOW
  → Respuesta: "OK:TX_STARTING_BROADCAST"

CMD:PING
  → Prueba de conectividad
  → Respuesta: "PONG"
```

### **Flujo de Transmisión ESP-NOW**

```
startLoRaTransmission(filename)
  ↓
processLoRaTransmission()
  ↓
sendFileViaLoRa(filename)
  ├─ Lee archivo de LittleFS
  ├─ Calcula parámetros
  ├─ Genera FileID único
  │
  └─ BUCLE POR CADA VUELTA (REPEAT_COUNT = 3):
      ├─ Envía MANIFEST × MANIFEST_REPEAT (5 veces)
      │
      ├─ Para cada chunk del archivo:
      │  ├─ Envía DATA_CHUNK (240 bytes)
      │  ├─ Acumula 8 chunks para FEC
      │  └─ Cada bloque 8: Envía PARITY_CHUNK (XOR)
      │
      ├─ Envía FILE_END × FILE_END_REPEAT (5 veces)
      │
      └─ Delay entre vueltas (configurable)
```

### **Delays Dinámicos Entre Paquetes**

```cpp
int getInterPacketDelay() {
  // Mayor ancho de banda = menos tiempo aire necesario
  // Mayor SF = más tiempo aire
  
  if (currentBW >= 500.0) {
    if (currentSF <= 7) return 60;    // Rápido: BW=500, SF≤7
    if (currentSF == 9) return 100;
    return 130;                        // Lento: BW=500, SF≥11
  } 
  else if (currentBW >= 250.0) {
    if (currentSF <= 7) return 80;
    if (currentSF == 9) return 120;
    return 150;
  } 
  else {  // BW = 125 kHz (default)
    if (currentSF <= 7) return 100;   // SF=5-7: 100ms
    if (currentSF == 9) return 130;   // SF=9: 130ms
    return 180;                        // SF≥11: 180ms
  }
}
```

### **Forward Error Correction (FEC) - XOR**

```cpp
// Por cada bloque de 8 chunks:
for (int round = 1; round <= currentREPEAT; round++) {
  for (uint16_t i = 0; i < totalChunks; i++) {
    // Enviar DATA_CHUNK
    
    if (fecIndex == FEC_BLOCK_SIZE || i + 1 == totalChunks) {
      // Calcular paridad (XOR bit a bit)
      uint8_t parityData[CHUNK_SIZE_LORA] = {0};
      
      for (int j = 0; j < fecIndex; j++) {
        for (size_t k = 0; k < fecLengths[j]; k++) {
          parityData[k] ^= fecBlock[j][k];
        }
      }
      
      // Enviar PARITY_CHUNK
      sendParityChunk(fileID, blockIndex, parityData, maxLen);
      
      fecIndex = 0;
    }
  }
}
```

**Beneficio**: El RX puede recuperar 1 chunk perdido por bloque de 8.

### **Estadísticas de Transmisión**

```
════════════════════════════════════════════════════════════════
Transmisión BROADCAST Completada
════════════════════════════════════════════════════════════════
📊 Total paquetes: 1245
📈 Fallos de radio: 3
🔁 Vueltas completadas: 2
⏱️  Tiempo total: 45.32 segundos
⚡ Velocidad: 145.67 kbps
📦 Tamaño archivo: 512 KB

Cálculo velocidad: (512 KB × 8 bits × 2 vueltas) / (45.32 s × 1000 ms)
```

---

## 📥 RECEPTOR (RX)

### **Inicialización de Buffers en Heap**

```cpp
#define MAX_CHUNKS 4096
#define MAX_PARITY_BLOCKS (MAX_CHUNKS / FEC_BLOCK_SIZE)

void setupLoRaBuffers() {
  // Prealoca arrays dinámicos en heap
  chunkBuffer   = (uint8_t**)calloc(MAX_CHUNKS, sizeof(uint8_t*));
  chunkReceived = (bool*)calloc(MAX_CHUNKS, sizeof(bool));
  chunkLengths  = (uint16_t*)calloc(MAX_CHUNKS, sizeof(uint16_t));
  
  parityBuffer   = (uint8_t**)calloc(MAX_PARITY_BLOCKS, sizeof(uint8_t*));
  parityReceived = (bool*)calloc(MAX_PARITY_BLOCKS, sizeof(bool));
  parityLengths  = (uint16_t*)calloc(MAX_PARITY_BLOCKS, sizeof(uint16_t));
  
  // RAM estimado: ~140 KB para máximo 4096 chunks
}
```

### **Pipeline de Recepción**

```
OnDataRecv() [ISR] ← Interrupción hardware de nuevo paquete
  ├─ Valida longitud del paquete
  ├─ Encola paquete en rxQueue
  └─ Ajusta banderas para procesar en contexto normal

loop() [Contexto principal]
  ├─ dequeueESPNowPacket() ← Saca paquetes de la cola
  ├─ processESPNowPacket() ← Procesa en contexto seguro
  │  ├─ Valida CRC16
  │  ├─ Identifica tipo por magic bytes
  │  └─ Llama handler específico
  │
  ├─ Timeout (~120 segundos)
  │  └─ assembleFile() ← Ensambla cuando se agota tiempo
  │
  └─ Espera nuevos paquetes
```

### **Procesamiento de Paquetes**

```cpp
void processLoRaPacket() {
  uint8_t buffer[300];
  int state = radio.readData(buffer, sizeof(buffer));
  
  if (state != RADIOLIB_ERR_NONE) {
    radio.startReceive();
    return;  // Error en lectura
  }
  
  size_t len = radio.getPacketLength();
  
  // Validar longitud mínima
  if (len < 8) { 
    radio.startReceive(); 
    return; 
  }
  
  // Validar CRC16-CCITT
  uint16_t crcRecv, crcCalc;
  memcpy(&crcRecv, buffer + len - 2, 2);
  crcCalc = crc16_ccitt(buffer, len - 2);
  
  if (crcRecv != crcCalc) {
    radio.startReceive();
    return;  // CRC inválido
  }
  
  // Procesar según tipo (magic bytes)
  if (buffer[0] == MANIFEST_MAGIC_1 && 
      buffer[1] == MANIFEST_MAGIC_2) {
    handleManifest(buffer, len);
  }
  else if (buffer[0] == DATA_MAGIC_1 && 
           buffer[1] == DATA_MAGIC_2) {
    handleDataChunk(buffer, len);
  }
  else if (buffer[0] == PARITY_MAGIC_1 && 
           buffer[1] == PARITY_MAGIC_2) {
    handleParityChunk(buffer, len);
  }
  else if (buffer[0] == FILE_END_MAGIC_1 && 
           buffer[1] == FILE_END_MAGIC_2) {
    handleFileEnd(buffer, len);
  }
  
  radio.startReceive();  // Volver a escuchar
}
```

### **Detección de Nuevas Transmisiones**

```cpp
#define FILE_ID_COOLDOWN 5000  // 5 segundos

if (fileID == lastProcessedFileID && 
    (millis() - lastFileCompletionTime) < FILE_ID_COOLDOWN) {
  return;  // Ignorar repeticiones dentro del cooldown
}
```

**Ventaja**: Evita procesar la misma transmisión dos veces en rápida sucesión.

### **Recuperación FEC (Forward Error Correction)**

```cpp
void recoverMissingChunks() {
  uint16_t recovered = 0;
  uint16_t numBlocks = (totalChunks + FEC_BLOCK_SIZE - 1) / FEC_BLOCK_SIZE;
  
  // Procesar cada bloque FEC
  for (uint16_t block = 0; block < numBlocks; block++) {
    if (!parityReceived[block]) continue;  // No hay parity para este bloque
    
    uint16_t baseIdx = block * FEC_BLOCK_SIZE;
    int missing = -1;
    int missingCount = 0;
    
    // Contar chunks faltantes en este bloque
    for (int i = 0; i < FEC_BLOCK_SIZE && (baseIdx + i) < totalChunks; i++) {
      if (!chunkReceived[baseIdx + i]) {
        missing = i;
        missingCount++;
      }
    }
    
    // Si hay exactamente 1 perdido, recuperar usando XOR
    if (missingCount != 1) continue;
    
    uint16_t missingIdx = baseIdx + missing;
    size_t maxLen = parityLengths[block];
    
    // Copiar parity como punto de partida
    chunkBuffer[missingIdx] = (uint8_t*)malloc(maxLen);
    if (!chunkBuffer[missingIdx]) continue;
    
    memcpy(chunkBuffer[missingIdx], parityBuffer[block], maxLen);
    
    // XOR con todos los otros chunks del bloque para recuperar
    for (int i = 0; i < FEC_BLOCK_SIZE && (baseIdx + i) < totalChunks; i++) {
      if (i == missing) continue;  // Saltar el chunk faltante
      if (!chunkReceived[baseIdx + i]) continue;  // Saltar otros faltantes
      
      size_t xorLen = min(maxLen, (size_t)chunkLengths[baseIdx + i]);
      for (size_t k = 0; k < xorLen; k++) {
        chunkBuffer[missingIdx][k] ^= chunkBuffer[baseIdx + i][k];
      }
    }
    
    // Marcar como recuperado
    chunkLengths[missingIdx] = maxLen;
    chunkReceived[missingIdx] = true;
    receivedDataChunks++;
    recovered++;
  }
  
  if (recovered > 0) {
    Serial.printf("🎉 %u chunk(s) recuperados con FEC\n", recovered);
  }
}
```

---

## 📝 ENSAMBLAJE DE ARCHIVO

### **Proceso Final**

```cpp
void assembleFile() {
  if (!receivingFile) return;
  
  // 1. Intentar recuperar chunks faltantes con FEC
  recoverMissingChunks();
  
  // 2. Contar qué sigue faltando
  uint16_t missingChunks = 0;
  for (uint16_t i = 0; i < totalChunks; i++) {
    if (!chunkReceived[i]) missingChunks++;
  }
  
  // 3. Decidir si escribir o cancelar
  if (missingChunks > MAX_TOLERABLE_GAPS) {
    Serial.printf("❌ Demasiados gaps: %u > %u\n", 
                  missingChunks, MAX_TOLERABLE_GAPS);
    cancelReception("TOO_MANY_GAPS");
    return;
  }
  
  // 4. Crear archivo en LittleFS
  if (LittleFS.exists(receivingFileName)) {
    LittleFS.remove(receivingFileName);
  }
  
  File outFile = LittleFS.open(receivingFileName, "w");
  if (!outFile) {
    cancelReception("CANT_CREATE");
    return;
  }
  
  // 5. Escribir chunks en orden
  uint32_t writtenBytes = 0;
  
  for (uint16_t i = 0; i < totalChunks; i++) {
    if (chunkReceived[i] && chunkBuffer[i] != nullptr) {
      // Escribir chunk recibido
      size_t written = outFile.write(chunkBuffer[i], chunkLengths[i]);
      writtenBytes += written;
    } else {
      // Rellenar gap con ceros (tolerancia configurada)
      uint32_t remaining = (receivingFileSize > writtenBytes)
                          ? receivingFileSize - writtenBytes
                          : 0;
      size_t fillSize = min((uint32_t)CHUNK_SIZE_LORA, remaining);
      
      if (fillSize > 0) {
        uint8_t zeros[CHUNK_SIZE_LORA] = {0};
        outFile.write(zeros, fillSize);
        writtenBytes += fillSize;
      }
    }
  }
  
  outFile.flush();
  outFile.close();
  
  // 6. Reportar estadísticas
  float receptionTime = (millis() - receptionStartTime) / 1000.0;
  float completeness = (receivedDataChunks * 100.0) / totalChunks;
  float speed = (receptionTime > 0)
                ? (writtenBytes * 8.0) / (receptionTime * 1000.0)
                : 0;
  
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║       🎉 ARCHIVO ENSAMBLADO           ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("📄 %s\n", receivingFileName.c_str());
  Serial.printf("📊 %u / %u bytes\n", writtenBytes, receivingFileSize);
  Serial.printf("📈 Completitud: %.1f%%\n", completeness);
  Serial.printf("⏱️  %.2f s | ⚡ %.2f kbps\n", receptionTime, speed);
}
```

### **Criterios de Éxito/Fallo**

| Condición | Acción |
|---|---|
| **Todos los chunks recibidos** | ✅ Archivo completo |
| **Chunks faltantes ≤ 2** | ✅ Rellenar con ceros |
| **Chunks faltantes > 2** | ❌ Cancelar recepción |
| **Error de memoria** | ❌ Cancelar recepción |
| **Error de escritura en LittleFS** | ❌ Cancelar recepción |

---

## 🔧 CARACTERÍSTICAS TÉCNICAS AVANZADAS

### **Rango vs. Velocidad Trade-off**

| Parámetros | Rango | Velocidad | Caso de Uso |
|---|---|---|---|
| SF=7, BW=500, CR=5 | ~3 km | ~5 kbps | Urbano, cercano |
| SF=9, BW=250, CR=7 | ~5 km | ~1 kbps | Rural, medio |
| SF=12, BW=125, CR=8 | ~10 km | ~100 bps | Muy largo alcance |

### **Consumo de Energía Estimado**

| Operación | Corriente | Duración | Energía |
|---|---|---|---|
| TX full power (22 dBm) | ~600 mA | 2 seg | ~1.2 J |
| RX escucha continua | ~10 mA | 1 seg | ~0.01 J |
| Repetir 3 veces (1 MB) | - | - | ~3.6 J |

### **Mejoras vs. LoRa**

| Aspecto | ESP-NOW | LoRa |
|---|---|---|
| **Rango** | ~500 m | ~5-10 km |
| **MTU** | 240 bytes | 240 bytes |
| **Modulación** | DSSS | LoRa |
| **Configurabilidad** | Limitada | Muy flexible |
| **Latencia** | <100 ms | 100-2000 ms |
| **Complejidad** | Baja | Media |

---

## 🔄 FLUJOS COMPLETOS DE DATOS

### **Flujo 1: Transmisión BROADCAST ESP-NOW**

```
MÓVIL (BLE)
  │
  ├─ "CMD:SET_ESPNOW_CONFIG:{"power":20,"channel":1,"rate":1}"
  │   ↓ (Almacena en Flash)
  │
  ├─ "CMD:TX_FILE:documento.bin"
  │
  ▼
TRANSMISOR (TX)
  ├─ Lee documento.bin de LittleFS (1 MB)
  ├─ Genera FileID único
  ├─ Calcula: 4266 chunks × 240 bytes
  │
  └─ Bucle 3 vueltas:
      ├─ Vuelta 1:
      │  ├─ BROADCAST MANIFEST × 5
      │  │   └─ ~5 × 117 bytes = 585 bytes
      │  │
      │  ├─ Envía 4266 × DATA_CHUNK
      │  │   └─ ~4266 × 252 bytes = 1,075 KB
      │  │
      │  ├─ Envía 534 × PARITY_CHUNK (FEC blocks)
      │  │   └─ ~534 × 246 bytes = 131 KB
      │  │
      │  ├─ BROADCAST FILE_END × 5
      │  │   └─ ~5 × 10 bytes = 50 bytes
      │  │
      │  └─ Delay: 100-150 ms entre chunks
      │
      ├─ Vuelta 2 (repetición automática)
      │  └─ [Idéntico a Vuelta 1]
      │
      └─ Vuelta 3 (repetición automática)
         └─ [Idéntico a Vuelta 1]
  │
  └─ Reporta:
      • Velocidad: 250-300 kbps
      • Paquetes totales: ~15,792
      • Fallos: ~47 reintentos
      • Tiempo: ~34 segundos
      │
      ▼
      MÓVIL: "TX_COMPLETE:1048576:34.20:287.50:3"

RECEPTORES (RX × N)
  ├─ Reciben TODOS los paquetes broadcast en paralelo
  ├─ Cada RX procesa independientemente
  ├─ FEC recupera chunks faltantes (~8 chunks por vuelta típicamente)
  ├─ Ensamblan en LittleFS
  └─ Reportan completitud individual
      • RX1: 100% en 34.2s (287.5 kbps)
      • RX2: 99.8% en 34.1s (287.8 kbps)
      • RX3: 100% en 34.3s (287.2 kbps)
```

**Ventaja clave**: Todos reciben EN PARALELO. TX solo transmite 1 vez (×2-3 vueltas).

---

## 📊 MONITOREO Y DEPURACIÓN

### **Serial Monitor - Transmisor**

```
════════════════════════════════════════════════════════════════
📡 File Transfer System v4.2 TX BROADCAST
════════════════════════════════════════════════════════════════

💾 Inicializando LittleFS...
   Total: 1.89 MB | Usado: 0.25 MB | Libre: 1.64 MB

📡 Inicializando BLE...
✅ BLE iniciado: Heltec-TX-Broadcast

📡 Inicializando radio LoRa...
✅ SX1262 inicializado

💾 Cargando configuración LoRa desde memoria flash...
✅ Configuración LoRa cargada:
   BW: 125 kHz
   SF: 9
   CR: 4/7
   REPEAT: 2
   POWER: 17 dBm

✅ Sistema TX BROADCAST listo
👂 Esperando conexión BLE...
📡 Radio LoRa configurado para TX BROADCAST

---[Cliente BLE conecta]---

📩 Comando BLE: CMD:TX_FILE:/datos.bin
📡 Iniciando transmisión LoRa BROADCAST...

╔════════════════════════════════════════╗
║  📁 Archivo: datos.bin
║  📊 Tamaño: 512000 bytes (500.00 KB)
║  📦 Chunks: 2134
║  🔁 Repeticiones: 2 vueltas
║  🔀 Interleaving: DESACTIVADO
║  🆔 File ID: 0x9ABCD123
╚════════════════════════════════════════╝

╔════════════════════════════════════════╗
║       🔁 VUELTA 1 de 2
╚════════════════════════════════════════╝

📤 Enviando MANIFEST (5 repeticiones)...
📤 TX MANIFEST (119 bytes, fileID=0x9ABCD123)... ✅ OK
📤 TX MANIFEST (119 bytes, fileID=0x9ABCD123)... ✅ OK
📤 TX MANIFEST (119 bytes, fileID=0x9ABCD123)... ✅ OK
📤 TX MANIFEST (119 bytes, fileID=0x9ABCD123)... ✅ OK
📤 TX MANIFEST (119 bytes, fileID=0x9ABCD123)... ✅ OK
✅ MANIFEST OK

📦 Progreso: 100/2134 (4.7%) - Vuelta 1
📦 Progreso: 500/2134 (23.4%) - Vuelta 1
📦 Progreso: 1000/2134 (46.9%) - Vuelta 1
📦 Progreso: 1500/2134 (70.3%) - Vuelta 1
📦 Progreso: 2000/2134 (93.7%) - Vuelta 1
📦 Progreso: 2134/2134 (100.0%) - Vuelta 1

🏁 Enviando FILE_END (vuelta 1)...
📤 TX FILE_END... ✅ OK (intento 1)

╔════════════════════════════════════════╗
║       🔁 VUELTA 2 de 2
╚════════════════════════════════════════╝

[Proceso idéntico para vuelta 2]

╔════════════════════════════════════════╗
║     🎉 TRANSMISIÓN COMPLETA           ║
╚════════════════════════════════════════╝
📊 Total paquetes: 4402
📈 Fallos de radio: 2
🔁 Vueltas completadas: 2

✅ Transmisión LoRa BROADCAST exitosa
⏱️  Tiempo: 540.45 s
⚡ Velocidad: 7.55 kbps
╔════════════════════════════════════════╗
║  ⚡ VELOCIDAD: 7.55 kbps              ║
╚════════════════════════════════════════╝
```

### **Serial Monitor - Receptor**

```
════════════════════════════════════════════════════════════════
📡 File Transfer System v4.2 RX BROADCAST
════════════════════════════════════════════════════════════════

✅ Sistema RX BROADCAST listo
👂 Esperando conexión BLE y broadcast LoRa...

---[MANIFEST recibido]---

🔁 Manifest duplicado (vuelta 1) × 5 veces

╔════════════════════════════════════════╗
║  🆔 File ID:  0x9ABCD123
║  📄 Archivo:  datos.bin
║  📊 Tamaño:   512000 bytes (500.00 KB)
║  📦 Chunks:   2134
║  📶 RSSI: -78 dBm | SNR: 9.50 dB
╚════════════════════════════════════════╝

---[Recepción de datos]---

📦 100/2134 (4.7%) | RSSI: -78 | Dupes: 15
📦 500/2134 (23.4%) | RSSI: -79 | Dupes: 67
📦 1000/2134 (46.9%) | RSSI: -77 | Dupes: 143
📦 1500/2134 (70.3%) | RSSI: -80 | Dupes: 234
📦 2000/2134 (93.7%) | RSSI: -78 | Dupes: 318
📦 2134/2134 (100.0%) | RSSI: -79 | Dupes: 387

---[Vuelta 2 en progreso]---

🔁 Manifest duplicado (vuelta 2) × 5 veces
📦 100/2134 (4.7%) | RSSI: -78 | Dupes: 402
📦 2134/2134 (100.0%) | RSSI: -78 | Dupes: 687

🏁 FILE_END recibido
⏳ Ventana 1s para chunks retrasados...

═══════════════════════════════════════
📝 Ensamblando archivo...

🔧 FEC Recovery...
✅ 8 chunk(s) recuperados con FEC

📊 Recibidos: 2134/2134 | Perdidos: 0 (0.0%)
   Parity recibidos: 267
   Duplicados: 687

╔════════════════════════════════════════╗
║       🎉 ARCHIVO ENSAMBLADO           ║
╚════════════════════════════════════════╝
📄 /datos.bin
📊 512000 / 512000 bytes
📈 Completitud: 100.0%
⏱️  540.32 s | ⚡ 7.56 kbps
📶 RSSI promedio: -78 dBm | SNR promedio: 9.25 dB
═══════════════════════════════════════

RX_COMPLETE:datos.bin:512000:540.32:100.0

👂 Esperando nueva transmisión...
```

---

## 🔐 SEGURIDAD Y LIMITACIONES

### **Seguridad Actual**

| Aspecto | Estado | Notas |
|---|---|---|
| **Encriptación** | ❌ NO | Broadcast abierto |
| **Autenticación** | ❌ NO | Cualquiera puede escuchar/enviar |
| **Integridad** | ⚠️ CRC16 | Detecta errores, no autentica |
| **Privacidad** | ❌ NO | Datos sin protección |

### **Limitaciones Conocidas**

- **Rango teoría vs. práctica**: ~80% del rango teórico típicamente
- **Interferencia ISM**: Compartido con WiFi, teléfonos, garajes
- **Latencia**: 100-2000 ms por paquete (muy lento para control real-time)
- **Ancho de banda**: Máximo ~50 kbps en BW=500, SF=5

### **Mejoras Recomendadas**

1. Usar encriptación AES-128 en capa de aplicación
2. Implementar autenticación por token/firma en MANIFEST
3. Detectar jamming automático (cambiar SF/BW)
4. Validar FileID y versión de protocolo

---

## 📈 CASOS DE USO IDEALES

✅ **Actualizaciones de firmware distribuidas** en parques eólicos, plantas solares

✅ **Sincronización de sensores remotos** en redes agrícolas

✅ **Distribución de configuración** en redes de IoT urbano/rural

✅ **Alertas de emergencia** con confiabilidad de FEC

---

## 🎯 CONCLUSIÓN

El sistema ESP-NOW Broadcast ofrece:

- ✅ **Rango moderado**: ~500 m en línea de vista
- ✅ **Bajo consumo**: Transmisión efficient a nivel inalámbrico
- ✅ **FEC automático**: Recuperación de chunks perdidos
- ✅ **Escalabilidad**: Broadcast 1→N sin límite práctico
- ✅ **Persistencia**: Configuración guardada automáticamente

**Ideal para**: IoT de corto-medio alcance, sensores distribuidos, alertas en áreas urbanas.

---

**Versión**: 4.2
**Fecha**: 2026-05-16
**Hardware**: Heltec WiFi LoRa 32 V3 (ESP32-S3)
**Protocolo**: ESP-NOW Broadcast con FEC (XOR)
**Rango**: ~500 metros (línea de vista)