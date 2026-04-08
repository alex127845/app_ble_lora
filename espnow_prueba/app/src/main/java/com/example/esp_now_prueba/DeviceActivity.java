package com.example.esp_now_prueba;
import android.os.Bundle;

import androidx.activity.EdgeToEdge;
import androidx.appcompat.app.AppCompatActivity;
import androidx.cardview.widget.CardView;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import android.widget.Spinner;
import android.widget.ArrayAdapter;
import androidx.cardview.widget.CardView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.text.DecimalFormat;
import java.util.ArrayList;
import java.util.List;

public class DeviceActivity extends AppCompatActivity implements BLEManager.BLECallback {

    private static final String TAG = "DeviceActivity";

    // ════════════════════════════════════════════════════════════════════
    // 🔧 CONSTANTES
    // ════════════════════════════════════════════════════════════════════

    private static final int REQUEST_FILE_UPLOAD = 100;
    private static final int REQUEST_FILE_DOWNLOAD = 101;

    // ════════════════════════════════════════════════════════════════════
    // 🎨 COMPONENTES UI
    // ════════════════════════════════════════════════════════════════════

    private TextView tvDeviceName;
    private TextView tvConnectionStatus;
    private Button btnListFiles;
    private Button btnUploadFile;
    private Button btnDisconnect;
    private RecyclerView recyclerViewFiles;
    private FileAdapter fileAdapter;
    private ProgressBar progressBar;
    private TextView tvProgressText;
    private View layoutProgress;

    // LoRa
    //private CardView cardLoRaConfig;
    //private TextView tvLoRaStatus;
    //private Button btnConfigLoRa;
    //private View layoutLoRaTransmitting;
    //private TextView tvLoRaProgress;
    //private ProgressBar progressBarLoRa;

    private CardView cardESPNowConfig;
    private TextView tvESPNowStatus;
    private Button btnConfigESPNow;
    private View layoutESPNowTransmitting;
    private TextView tvESPNowProgress;
    private ProgressBar progressBarESPNow;

    // ════════════════════════════════════════════════════════════════════
    // 📡 BLE Y GESTIÓN DE ARCHIVOS
    // ════════════════════════════════════════════════════════════════════

    private BLEManager bleManager;
    private FileManager fileManager;
    private String deviceAddress;
    private String deviceName;
    private boolean isConnected = false;

    // Lista de archivos en el Heltec
    private List<FileInfo> fileList = new ArrayList<>();

    // Estado LoRa
    private boolean isTxMode = false;  // true si es TX, false si es RX
    private boolean isTransmitting = false;

    //private LoRaConfig currentLoRaConfig;
    private ESPNowConfig currentESPNowConfig;

    // ════════════════════════════════════════════════════════════════════
    // 🚀 CICLO DE VIDA - onCreate
    // ════════════════════════════════════════════════════════════════════

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_device);

        Log.d(TAG, "📱 DeviceActivity iniciada");

        // Obtener datos del dispositivo
        Intent intent = getIntent();
        deviceAddress = intent.getStringExtra("DEVICE_ADDRESS");
        deviceName = intent.getStringExtra("DEVICE_NAME");

        Log.d(TAG, "📍 Dispositivo: " + deviceName + " (" + deviceAddress + ")");

        // Configurar ActionBar
        if (getSupportActionBar() != null) {
            getSupportActionBar().setTitle("📂 " + deviceName);
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
        }

        // Inicializar vistas
        initViews();

        // Inicializar managers
        initManagers();

        // Conectar automáticamente
        connectToDevice();
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎨 INICIALIZAR VISTAS
    // ════════════════════════════════════════════════════════════════════

    private void initViews() {
        tvDeviceName = findViewById(R.id.tvDeviceName);
        tvConnectionStatus = findViewById(R.id.tvConnectionStatus);
        btnListFiles = findViewById(R.id.btnListFiles);
        btnUploadFile = findViewById(R.id.btnUploadFile);
        btnDisconnect = findViewById(R.id.btnDisconnect);
        recyclerViewFiles = findViewById(R.id.recyclerViewFiles);
        progressBar = findViewById(R.id.progressBar);
        tvProgressText = findViewById(R.id.tvProgressText);
        layoutProgress = findViewById(R.id.layoutProgress);

        // Configurar información del dispositivo
        tvDeviceName.setText("📡 " + deviceName);
        tvConnectionStatus.setText("🔄 Conectando...");

        // Configurar RecyclerView de archivos
        recyclerViewFiles.setLayoutManager(new LinearLayoutManager(this));
        fileAdapter = new FileAdapter();
        recyclerViewFiles.setAdapter(fileAdapter);

        // Configurar botones
        btnListFiles.setOnClickListener(v -> listFiles());
        btnUploadFile.setOnClickListener(v -> selectFileToUpload());
        btnDisconnect.setOnClickListener(v -> disconnect());

        // Deshabilitar botones hasta conectar
        setButtonsEnabled(false);

        // Ocultar barra de progreso
        layoutProgress.setVisibility(View.GONE);

        // ════════════════════════════════════════════════════════════════
        // NUEVOS - Vistas LoRa
        // ════════════════════════════════════════════════════════════════

        //cardLoRaConfig = findViewById(R.id.cardLoRaConfig);
        //tvLoRaStatus = findViewById(R.id.tvLoRaStatus);
        //btnConfigLoRa = findViewById(R.id.btnConfigLoRa);
        //layoutLoRaTransmitting = findViewById(R.id.layoutLoRaTransmitting);
        //tvLoRaProgress = findViewById(R.id.tvLoRaProgress);
        //progressBarLoRa = findViewById(R.id.progressBarLoRa);

        cardESPNowConfig = findViewById(R.id.cardESPNowConfig);
        tvESPNowStatus = findViewById(R.id.tvESPNowStatus);
        btnConfigESPNow = findViewById(R.id.btnConfigESPNow);
        layoutESPNowTransmitting = findViewById(R.id.layoutESPNowTransmitting);
        tvESPNowProgress = findViewById(R.id.tvESPNowProgress);
        progressBarESPNow = findViewById(R.id.progressBarESPNow);

        // Inicializar configuración LoRa
        //currentLoRaConfig = new LoRaConfig();
        currentESPNowConfig = new ESPNowConfig();
        updateLoRaStatusUI();

        // Configurar botón de config LoRa
        //btnConfigLoRa.setOnClickListener(v -> showLoRaConfigDialog());
        btnConfigLoRa.setOnClickListener(v -> showESPNowConfigDialog());

        // Ocultar progreso LoRa
        layoutLoRaTransmitting.setVisibility(View.GONE);

        // ✅ NUEVO: Mostrar modo como "detectando..." hasta confirmar
        tvLoRaStatus.setText("🔍 Detectando modo...");

        // Detectar modo TX/RX del nombre del dispositivo
        detectDeviceMode();
    }

    // ════════════════════════════════════════════════════════════════════
    // 🔧 INICIALIZAR MANAGERS
    // ════════════════════════════════════════════════════════════════════

    private void initManagers() {
        Log.d(TAG, "🔧 Inicializando BLEManager y FileManager...");

        // Inicializar BLEManager
        bleManager = new BLEManager(this, this);

        // Inicializar FileManager
        fileManager = new FileManager(this);

        Log.d(TAG, "✅ Managers inicializados");
    }

    // ════════════════════════════════════════════════════════════════════
    // 🔌 CONECTAR AL DISPOSITIVO
    // ════════════════════════════════════════════════════════════════════

    private void connectToDevice() {
        Log.d(TAG, "🔌 Conectando a " + deviceName + "...");

        tvConnectionStatus.setText("🔄 Conectando...");

        // Conectar vía BLE
        bleManager.connect(deviceAddress);
    }

    // ════════════════════════════════════════════════════════════════════
    // 🔌 DESCONECTAR DEL DISPOSITIVO
    // ════════════════════════════════════════════════════════════════════

    private void disconnect() {
        Log.d(TAG, "🔌 Desconectando...");

        new AlertDialog.Builder(this)
                .setTitle("🔌 Desconectar")
                .setMessage("¿Deseas desconectarte de " + deviceName + "?")
                .setPositiveButton("Sí", (dialog, which) -> {
                    bleManager.disconnect();
                    finish();
                })
                .setNegativeButton("No", null)
                .show();
    }

    // ════════════════════════════════════════════════════════════════════
    // 📋 LISTAR ARCHIVOS
    // ════════════════════════════════════════════════════════════════════

    private void listFiles() {
        if (!isConnected) {
            Toast.makeText(this, "⚠️ No conectado", Toast.LENGTH_SHORT).show();
            return;
        }

        Log.d(TAG, "📋 Solicitando lista de archivos...");

        // Limpiar lista actual
        fileList.clear();
        fileAdapter.notifyDataSetChanged();

        // Mostrar progreso
        showProgress(true, "Listando archivos...", 0);

        // Enviar comando LIST
        bleManager.sendCommand("CMD:LIST");
    }

    // ════════════════════════════════════════════════════════════════════
    // 📤 SELECCIONAR ARCHIVO PARA SUBIR
    // ════════════════════════════════════════════════════════════════════

    private void selectFileToUpload() {
        if (!isConnected) {
            Toast.makeText(this, "⚠️ No conectado", Toast.LENGTH_SHORT).show();
            return;
        }

        Log.d(TAG, "📤 Abriendo selector de archivos...");

        // Intent para seleccionar archivo
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType("*/*");
        intent.addCategory(Intent.CATEGORY_OPENABLE);

        try {
            startActivityForResult(
                    Intent.createChooser(intent, "Seleccionar archivo para subir"),
                    REQUEST_FILE_UPLOAD
            );
        } catch (Exception e) {
            Log.e(TAG, "❌ Error abriendo selector: " + e.getMessage());
            Toast.makeText(this, "❌ Error abriendo selector de archivos",
                    Toast.LENGTH_SHORT).show();
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📥 DESCARGAR ARCHIVO
    // ════════════════════════════════════════════════════════════════════

    private void downloadFile(FileInfo fileInfo) {
        Log.d(TAG, "📥 Descargando: " + fileInfo.name);

        // Mostrar confirmación
        new AlertDialog.Builder(this)
                .setTitle("📥 Descargar Archivo")
                .setMessage("¿Descargar '" + fileInfo.name + "'?\n\n" +
                        "Tamaño: " + formatFileSize(fileInfo.size) + "\n" +
                        "Se guardará en Descargas/")
                .setPositiveButton("📥 Descargar", (dialog, which) -> {
                    showProgress(true, "Descargando " + fileInfo.name + "...", 0);
                    bleManager.sendCommand("CMD:DOWNLOAD:" + fileInfo.name);
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    // ════════════════════════════════════════════════════════════════════
    // 🗑️ ELIMINAR ARCHIVO
    // ════════════════════════════════════════════════════════════════════

    private void deleteFile(FileInfo fileInfo) {
        Log.d(TAG, "🗑️ Eliminando: " + fileInfo.name);

        // Mostrar confirmación
        new AlertDialog.Builder(this)
                .setTitle("🗑️ Eliminar Archivo")
                .setMessage("¿Eliminar '" + fileInfo.name + "'?\n\n" +
                        "Esta acción no se puede deshacer.")
                .setPositiveButton("🗑️ Eliminar", (dialog, which) -> {
                    showProgress(true, "Eliminando...", 0);
                    bleManager.sendCommand("CMD:DELETE:" + fileInfo.name);
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    // ════════════════════════════════════════════════════════════════════
    // 📊 RESULTADO DE SELECCIÓN DE ARCHIVO
    // ════════════════════════════════════════════════════════════════════

    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == REQUEST_FILE_UPLOAD && resultCode == Activity.RESULT_OK) {
            if (data != null && data.getData() != null) {
                Uri fileUri = data.getData();
                Log.d(TAG, "📄 Archivo seleccionado: " + fileUri.toString());

                // Procesar archivo seleccionado
                processFileUpload(fileUri);
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📤 PROCESAR SUBIDA DE ARCHIVO
    // ════════════════════════════════════════════════════════════════════

    private void processFileUpload(Uri fileUri) {
        try {
            // Obtener información del archivo
            String fileName = fileManager.getFileName(fileUri);
            long fileSize = fileManager.getFileSize(fileUri);

            Log.d(TAG, "📄 Nombre: " + fileName);
            Log.d(TAG, "📊 Tamaño: " + fileSize + " bytes");

            // Verificar tamaño máximo (1.5 MB para LittleFS típico)
            if (fileSize > 1500000) {
                Toast.makeText(this,
                        "⚠️ Archivo muy grande (máx 1.5 MB)",
                        Toast.LENGTH_LONG).show();
                return;
            }

            // Mostrar confirmación
            new AlertDialog.Builder(this)
                    .setTitle("📤 Subir Archivo")
                    .setMessage("¿Subir '" + fileName + "'?\n\n" +
                            "Tamaño: " + formatFileSize(fileSize))
                    .setPositiveButton("📤 Subir", (dialog, which) -> {
                        startFileUpload(fileUri, fileName, fileSize);
                    })
                    .setNegativeButton("Cancelar", null)
                    .show();

        } catch (Exception e) {
            Log.e(TAG, "❌ Error procesando archivo: " + e.getMessage());
            Toast.makeText(this, "❌ Error: " + e.getMessage(),
                    Toast.LENGTH_SHORT).show();
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📤 INICIAR SUBIDA DE ARCHIVO
    // ════════════════════════════════════════════════════════════════════

    private void startFileUpload(Uri fileUri, String fileName, long fileSize) {
        Log.d(TAG, "📤 Iniciando subida: " + fileName);

        showProgress(true, "Subiendo " + fileName + "...", 0);

        // Enviar comando UPLOAD_START
        String command = "CMD:UPLOAD_START:" + fileName + ":" + fileSize;
        bleManager.sendCommand(command);

        // Preparar archivo para envío en chunks
        new Thread(() -> {
            try {
                // Esperar confirmación del Heltec
                Thread.sleep(500);

                // Leer archivo
                InputStream inputStream = getContentResolver().openInputStream(fileUri);
                if (inputStream == null) {
                    runOnUiThread(() -> {
                        showProgress(false, "", 0);
                        Toast.makeText(this, "❌ Error leyendo archivo",
                                Toast.LENGTH_SHORT).show();
                    });
                    return;
                }

                // Dividir en chunks y enviar
                fileManager.uploadFileInChunks(
                        inputStream,
                        fileSize,
                        bleManager,
                        new FileManager.UploadCallback() {
                            @Override
                            public void onProgress(int percentage) {
                                runOnUiThread(() -> {
                                    updateProgress(percentage,
                                            "Subiendo... " + percentage + "%");
                                });
                            }

                            @Override
                            public void onComplete() {
                                runOnUiThread(() -> {
                                    showProgress(false, "", 0);
                                    Toast.makeText(DeviceActivity.this,
                                            "✅ Archivo subido correctamente",
                                            Toast.LENGTH_SHORT).show();

                                    // Actualizar lista
                                    new Handler().postDelayed(() -> listFiles(), 1000);
                                });
                            }

                            @Override
                            public void onError(String error) {
                                runOnUiThread(() -> {
                                    showProgress(false, "", 0);
                                    Toast.makeText(DeviceActivity.this,
                                            "❌ Error: " + error,
                                            Toast.LENGTH_LONG).show();
                                });
                            }
                        }
                );

                inputStream.close();

            } catch (Exception e) {
                Log.e(TAG, "❌ Error en upload: " + e.getMessage());
                runOnUiThread(() -> {
                    showProgress(false, "", 0);
                    Toast.makeText(this, "❌ Error: " + e.getMessage(),
                            Toast.LENGTH_LONG).show();
                });
            }
        }).start();
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎨 MOSTRAR/OCULTAR PROGRESO
    // ════════════════════════════════════════════════════════════════════

    private void showProgress(boolean show, String text, int progress) {
        runOnUiThread(() -> {
            if (show) {
                layoutProgress.setVisibility(View.VISIBLE);
                progressBar.setProgress(progress);
                tvProgressText.setText(text);
            } else {
                layoutProgress.setVisibility(View.GONE);
            }
        });
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎨 ACTUALIZAR PROGRESO
    // ════════════════════════════════════════════════════════════════════

    private void updateProgress(int percentage, String text) {
        runOnUiThread(() -> {
            progressBar.setProgress(percentage);
            tvProgressText.setText(text);
        });
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎨 HABILITAR/DESHABILITAR BOTONES
    // ════════════════════════════════════════════════════════════════════

    private void setButtonsEnabled(boolean enabled) {
        btnListFiles.setEnabled(enabled);
        btnUploadFile.setEnabled(enabled);
    }

    // ════════════════════════════════════════════════════════════════════
    // 📏 FORMATEAR TAMAÑO DE ARCHIVO
    // ════════════════════════════════════════════════════════════════════

    private String formatFileSize(long size) {
        if (size < 1024) {
            return size + " B";
        } else if (size < 1024 * 1024) {
            return new DecimalFormat("#.##").format(size / 1024.0) + " KB";
        } else {
            return new DecimalFormat("#.##").format(size / (1024.0 * 1024.0)) + " MB";
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📡 CALLBACKS DE BLEManager
    // ════════════════════════════════════════════════════════════════════

    @Override
    public void onConnected() {
        Log.d(TAG, "✅ Conectado al dispositivo");

        runOnUiThread(() -> {
            isConnected = true;
            tvConnectionStatus.setText("🟢 Conectado");
            setButtonsEnabled(true);
            Toast.makeText(this, "✅ Conectado a " + deviceName,
                    Toast.LENGTH_SHORT).show();

            // Listar archivos automáticamente
            new Handler().postDelayed(() -> {
                listFiles();

                // ✅ IMPORTANTE: Enviar comando para detectar modo
                bleManager.sendCommand("CMD:GET_MODE");

                // Obtener configuración LoRa
                //bleManager.sendCommand("CMD:GET_LORA_CONFIG");
                bleManager.sendCommand("CMD:GET_ESPNOW_CONFIG");
            }, 500);
        });
    }

    @Override
    public void onDisconnected() {
        Log.d(TAG, "❌ Desconectado del dispositivo");

        runOnUiThread(() -> {
            isConnected = false;
            tvConnectionStatus.setText("🔴 Desconectado");
            setButtonsEnabled(false);
            Toast.makeText(this, "🔴 Desconectado", Toast.LENGTH_SHORT).show();
        });
    }

    @Override
    public void onDataReceived(String data) {
        Log.d(TAG, "📥 Datos recibidos: " + data);

        runOnUiThread(() -> {
            processReceivedData(data);
        });
    }

    @Override
    public void onProgress(int percentage) {
        Log.d(TAG, "📊 Progreso: " + percentage + "%");

        runOnUiThread(() -> {
            updateProgress(percentage, "Progreso: " + percentage + "%");
        });
    }

    @Override
    public void onError(String error) {
        Log.e(TAG, "❌ Error BLE: " + error);

        runOnUiThread(() -> {
            showProgress(false, "", 0);
            Toast.makeText(this, "❌ Error: " + error, Toast.LENGTH_LONG).show();
        });
    }

    // ════════════════════════════════════════════════════════════════════
    // 📨 PROCESAR DATOS RECIBIDOS
    // ════════════════════════════════════════════════════════════════════

    private void processReceivedData(String data) {
        data = data.trim();

        // Inicio de lista de archivos
        if (data.equals("FILES_START")) {
            Log.d(TAG, "📋 Inicio de lista de archivos");
            fileList.clear();
            return;
        }

        // Fin de lista de archivos
        if (data.startsWith("FILES_END")) {
            Log.d(TAG, "📋 Fin de lista (" + fileList.size() + " archivos)");
            fileAdapter.notifyDataSetChanged();
            showProgress(false, "", 0);

            if (fileList.isEmpty()) {
                Toast.makeText(this, "📂 No hay archivos en el dispositivo",
                        Toast.LENGTH_SHORT).show();
            }
            return;
        }

        // Archivo individual: FILE:nombre:tamaño
        if (data.startsWith("FILE:")) {
            String[] parts = data.substring(5).split(":");
            if (parts.length >= 2) {
                String name = parts[0];
                long size = Long.parseLong(parts[1]);

                FileInfo fileInfo = new FileInfo(name, size);
                fileList.add(fileInfo);

                Log.d(TAG, "📄 Archivo agregado: " + name + " (" + size + " bytes)");
            }
            return;
        }

        // Confirmación de eliminación
        if (data.equals("OK:DELETED")) {
            Log.d(TAG, "✅ Archivo eliminado");
            showProgress(false, "", 0);
            Toast.makeText(this, "✅ Archivo eliminado", Toast.LENGTH_SHORT).show();

            // Actualizar lista
            new Handler().postDelayed(() -> listFiles(), 500);
            return;
        }

        // Upload completo
        if (data.startsWith("OK:UPLOAD_COMPLETE")) {
            Log.d(TAG, "✅ Upload completado");
            // El callback de FileManager ya maneja esto
            return;
        }

        // Download inicio
        if (data.startsWith("DOWNLOAD_START:")) {
            String[] parts = data.substring(15).split(":");
            if (parts.length >= 2) {
                String fileName = parts[0];
                long fileSize = Long.parseLong(parts[1]);

                Log.d(TAG, "📥 Iniciando descarga: " + fileName + " (" + fileSize + " bytes)");
                fileManager.startDownload(fileName, fileSize);
            }
            return;
        }

        // Download chunk
        if (data.startsWith("CHUNK:")) {
            String[] parts = data.substring(6).split(":", 2);
            if (parts.length >= 2) {
                int chunkNum = Integer.parseInt(parts[0]);
                String base64Data = parts[1];

                fileManager.receiveChunk(chunkNum, base64Data,
                        new FileManager.DownloadCallback() {
                            @Override
                            public void onProgress(int percentage) {
                                updateProgress(percentage, "Descargando... " + percentage + "%");
                            }

                            @Override
                            public void onComplete(File file) {
                                showProgress(false, "", 0);
                                Toast.makeText(DeviceActivity.this,
                                        "✅ Descargado: " + file.getName(),
                                        Toast.LENGTH_SHORT).show();
                            }

                            @Override
                            public void onError(String error) {
                                showProgress(false, "", 0);
                                Toast.makeText(DeviceActivity.this,
                                        "❌ Error: " + error,
                                        Toast.LENGTH_LONG).show();
                            }
                        });
            }
            return;
        }

        // Download fin
        if (data.startsWith("DOWNLOAD_END:")) {
            Log.d(TAG, "✅ Download completado");
            fileManager.completeDownload();
            return;
        }

        // ACK de chunk
        if (data.startsWith("ACK:")) {
            // FileManager maneja esto internamente
            return;
        }

        // Errores
        if (data.startsWith("ERROR:")) {
            String error = data.substring(6);
            Log.e(TAG, "❌ Error del Heltec: " + error);
            showProgress(false, "", 0);

            String mensaje = "";
            switch (error) {
                case "FILE_NOT_FOUND":
                    mensaje = "Archivo no encontrado";
                    break;
                case "NO_SPACE":
                    mensaje = "Sin espacio en el dispositivo";
                    break;
                case "FILE_IN_USE":
                    mensaje = "Archivo en uso";
                    break;
                case "DELETE_FAILED":
                    mensaje = "Error eliminando archivo";
                    break;
                default:
                    mensaje = error;
            }

            Toast.makeText(this, "❌ " + mensaje, Toast.LENGTH_LONG).show();
        }

        // Config LoRa recibida
        /*
        if (data.startsWith("LORA_CONFIG:")) {
            String json = data.substring(12);
            Log.d(TAG, "⚙️ Configuración LoRa recibida: " + json);
            currentLoRaConfig.fromJson(json);
            updateLoRaStatusUI();
            // ✅ CAMBIO: Informar que es config persistida
            Toast.makeText(this, "✅ Config LoRa cargada (guardada en flash)",
                    Toast.LENGTH_SHORT).show();
            return;
        }*/

        // Confirmación de config LoRa
        /*
        if (data.equals("OK:LORA_CONFIG_SET")) {
            Log.d(TAG, "✅ Configuración LoRa aplicada y guardada");

            // ✅ CAMBIO: Informar persistencia
            Toast.makeText(this, "✅ Configuración guardada en memoria flash",
                    Toast.LENGTH_SHORT).show();
            return;
        }*/
        // Config ESP-NOW recibida
        if (data.startsWith("ESPNOW_CONFIG:")) {
            String json = data.substring(14);
            Log.d(TAG, "⚙️ Configuración ESP-NOW recibida: " + json);
            currentESPNowConfig.fromJson(json);
            updateESPNowStatusUI();
            Toast.makeText(this, "✅ Config ESP-NOW cargada (guardada en flash)",
                    Toast.LENGTH_SHORT).show();
            return;
        }

        // Confirmación de config ESP-NOW
        if (data.equals("OK:ESPNOW_CONFIG_SET")) {
            Log.d(TAG, "✅ Configuración ESP-NOW aplicada y guardada");
            Toast.makeText(this, "✅ Configuración guardada en memoria flash",
                    Toast.LENGTH_SHORT).show();
            return;
        }



        // Inicio de transmisión LoRa BROADCAST
        if (data.equals("OK:TX_STARTING_BROADCAST")) {  // ✅ CAMBIO
            Log.d(TAG, "📡 Transmisión LoRa BROADCAST iniciada");
            showLoRaProgress(true, "Transmitiendo (Broadcast)...", 0);
            return;
        }

        // Transmisión LoRa BROADCAST completada
        if (data.startsWith("TX_COMPLETE_BROADCAST:")) {  // ✅ CAMBIO
            isTransmitting = false;
            showLoRaProgress(false, "", 0);

            String[] parts = data.substring(23).split(":");  // ✅ CAMBIO: offset 23
            if (parts.length >= 4) {  // ✅ CAMBIO: ahora son 4 partes
                String size = parts[0];
                String time = parts[1];
                String speed = parts[2];
                String rounds = parts[3];  // ✅ NUEVO: número de vueltas

                String message = "✅ Transmisión BROADCAST completada\n\n" +
                        "Modo: Sin ACK (Carrusel)\n" +  // ✅ NUEVO
                        "Tamaño: " + formatFileSize(Long.parseLong(size)) + "\n" +
                        "Tiempo: " + time + " s\n" +
                        "Velocidad: " + speed + " kbps\n" +
                        "Vueltas: " + rounds;  // ✅ NUEVO

                new AlertDialog.Builder(this)
                        .setTitle("📡 Transmisión BROADCAST Exitosa")
                        .setMessage(message)
                        .setPositiveButton("OK", null)
                        .show();
            }
            return;
        }

        // Transmisión LoRa fallida
        if (data.startsWith("TX_FAILED:")) {
            isTransmitting = false;
            showLoRaProgress(false, "", 0);

            String reason = data.substring(10);
            Toast.makeText(this, "❌ TX fallida: " + reason, Toast.LENGTH_LONG).show();
            return;
        }

        // Inicio de recepción LoRa (solo RX)
        if (data.startsWith("RX_START:")) {
            String[] parts = data.substring(9).split(":");
            if (parts.length >= 2) {
                String filename = parts[0];
                String size = parts[1];

                showLoRaProgress(true, "Recibiendo " + filename + "...", 0);
                Toast.makeText(this, "📥 Recibiendo: " + filename,
                        Toast.LENGTH_SHORT).show();
            }
            return;
        }

        // Status de recepción LoRa
        if (data.startsWith("RX_STATUS:")) {
            String[] parts = data.substring(10).split(":");
            if (parts.length >= 2) {
                String progress = parts[0]; // ej: "25/100"

                String[] progressParts = progress.split("/");
                if (progressParts.length == 2) {
                    int current = Integer.parseInt(progressParts[0]);
                    int total = Integer.parseInt(progressParts[1]);
                    int percentage = (current * 100) / total;

                    updateLoRaProgress(percentage, "Fragmento " + progress);
                }
            }
            return;
        }

        // Recepción LoRa completada
        if (data.startsWith("RX_COMPLETE:")) {
            showLoRaProgress(false, "", 0);

            String[] parts = data.substring(12).split(":");
            if (parts.length >= 3) {
                String filename = parts[0];
                String size = parts[1];
                String time = parts[2];

                String message = "✅ Recepción completada\n\n" +
                        "Archivo: " + filename + "\n" +
                        "Tamaño: " + formatFileSize(Long.parseLong(size)) + "\n" +
                        "Tiempo: " + time + " s";

                new AlertDialog.Builder(this)
                        .setTitle("📥 Recepción Exitosa")
                        .setMessage(message)
                        .setPositiveButton("OK", null)
                        .show();

                // Actualizar lista
                new Handler().postDelayed(() -> listFiles(), 1000);
            }
            return;
        }

        // Recepción LoRa fallida
        if (data.startsWith("RX_FAILED:")) {
            showLoRaProgress(false, "", 0);

            String reason = data.substring(10);
            Toast.makeText(this, "❌ RX fallida: " + reason, Toast.LENGTH_LONG).show();
            return;
        }

        // ✅ Respuesta de modo del dispositivo
        if (data.startsWith("MODE:")) {
            String mode = data.substring(5).trim();

            if (mode.equals("TX")) {
                isTxMode = true;
                Log.d(TAG, "✅ Modo confirmado: TRANSMISOR");
                runOnUiThread(() -> {
                    tvLoRaStatus.setText("📡 Modo: TRANSMISOR");
                    Toast.makeText(this, "📡 Conectado a TX", Toast.LENGTH_SHORT).show();
                    // Actualizar lista para mostrar/ocultar botones LoRa
                    fileAdapter.notifyDataSetChanged();
                });
            } else if (mode.equals("RX")) {
                isTxMode = false;
                Log.d(TAG, "✅ Modo confirmado: RECEPTOR");
                runOnUiThread(() -> {
                    tvLoRaStatus.setText("📥 Modo: RECEPTOR");
                    Toast.makeText(this, "📥 Conectado a RX", Toast.LENGTH_SHORT).show();
                    // Actualizar lista para mostrar/ocultar botones LoRa
                    fileAdapter.notifyDataSetChanged();
                });
            }
            return;
        }



    }

    // ════════════════════════════════════════════════════════════════════
    // 🔄 CICLO DE VIDA - onDestroy
    // ════════════════════════════════════════════════════════════════════

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "💥 DeviceActivity destruida");

        // Desconectar BLE
        if (bleManager != null) {
            bleManager.disconnect();
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📂 CLASE INTERNA - FileInfo
    // ════════════════════════════════════════════════════════════════════

    /**
     * Información de un archivo en el Heltec
     */
    private static class FileInfo {
        String name;
        long size;

        FileInfo(String name, long size) {
            this.name = name;
            this.size = size;
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📋 ADAPTADOR DE LISTA DE ARCHIVOS
    // ════════════════════════════════════════════════════════════════════

    /**
     * Adaptador para mostrar la lista de archivos del Heltec
     */

    private class FileAdapter extends RecyclerView.Adapter<FileAdapter.ViewHolder> {

        @NonNull
        @Override
        public ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            View view = LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.item_file, parent, false);
            return new ViewHolder(view);
        }

        @Override
        public void onBindViewHolder(@NonNull ViewHolder holder, int position) {
            FileInfo fileInfo = fileList.get(position);

            holder.tvFileName.setText("📄 " + fileInfo.name);
            holder.tvFileSize.setText(formatFileSize(fileInfo.size));

            // Botón descargar
            holder.btnDownload.setOnClickListener(v -> {
                downloadFile(fileInfo);
            });

            // Botón eliminar
            holder.btnDelete.setOnClickListener(v -> {
                deleteFile(fileInfo);
            });

            // ⬇️ NUEVO - Botón transmitir por LoRa
            holder.btnLoRaTx.setOnClickListener(v -> {
                transmitFileViaLoRa(fileInfo);
            });

            // Mostrar botón LoRa solo si es modo TX
            if (isTxMode) {
                holder.btnLoRaTx.setVisibility(View.VISIBLE);
            } else {
                holder.btnLoRaTx.setVisibility(View.GONE);
            }
        }

        @Override
        public int getItemCount() {
            return fileList.size();
        }

        class ViewHolder extends RecyclerView.ViewHolder {
            TextView tvFileName;
            TextView tvFileSize;
            Button btnDownload;
            Button btnDelete;
            Button btnLoRaTx;  // ⬇️ NUEVO

            ViewHolder(View itemView) {
                super(itemView);
                tvFileName = itemView.findViewById(R.id.tvFileName);
                tvFileSize = itemView.findViewById(R.id.tvFileSize);
                btnDownload = itemView.findViewById(R.id.btnDownload);
                btnDelete = itemView.findViewById(R.id.btnDelete);
                btnLoRaTx = itemView.findViewById(R.id.btnLoRaTx);  // ⬇️ NUEVO
            }
        }
    }
    /**
     * ══════════════════════════════════════════════════════════════
     * ⚙️ CLASE - LoRaConfig
     * ══════════════════════════════════════════════════════════════
     */
    /*
    private static class LoRaConfig {
        int bandwidth;       // 125, 250, 500
        int spreadingFactor; // 7, 9, 12
        int codingRate;      // 5, 7, 8
        int repeat;          // ✅ CAMBIO: 1, 2, 3, 5 (vueltas carrusel)
        int power;           // 10, 14, 17, 20

        LoRaConfig() {
            // Valores por defecto
            bandwidth = 125;
            spreadingFactor = 9;
            codingRate = 7;
            repeat = 2;  // ✅ CAMBIO: 2 vueltas por defecto
            power = 17;
        }

        String toJson() {
            return "{\"bw\":" + bandwidth +
                    ",\"sf\":" + spreadingFactor +
                    ",\"cr\":" + codingRate +
                    ",\"repeat\":" + repeat +  // ✅ CAMBIO: "repeat" en vez de "ack"
                    ",\"power\":" + power + "}";
        }

        void fromJson(String json) {
            try {
                json = json.replace("{", "").replace("}", "").replace("\"", "");
                String[] pairs = json.split(",");

                for (String pair : pairs) {
                    String[] keyValue = pair.split(":");
                    if (keyValue.length == 2) {
                        String key = keyValue[0].trim();
                        int value = Integer.parseInt(keyValue[1].trim());

                        switch (key) {
                            case "bw":
                                bandwidth = value;
                                break;
                            case "sf":
                                spreadingFactor = value;
                                break;
                            case "cr":
                                codingRate = value;
                                break;
                            case "repeat":  // ✅ CAMBIO: "repeat" en vez de "ack"
                                repeat = value;
                                break;
                            case "power":
                                power = value;
                                break;
                        }
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "Error parseando LoRa config: " + e.getMessage());
            }
        }

        @Override
        public String toString() {
            return "BW: " + bandwidth + " kHz, SF: " + spreadingFactor +
                    ", CR: 4/" + codingRate + ", Vueltas: " + repeat +  // ✅ CAMBIO
                    ", Power: " + power + " dBm";
        }
    }
    */
    private static class ESPNowConfig {
        int power;           // 2-20 dBm
        int channel;         // 1-13
        int rate;            // 0=1Mbps, 1=2Mbps, 2=5.5Mbps, 3=11Mbps

        ESPNowConfig() {
            power = 20;      // Máxima potencia por defecto
            channel = 1;     // Canal Wi-Fi 1
            rate = 1;        // 2 Mbps (recomendado)
        }

        String toJson() {
            return "{\"power\":" + power +
                    ",\"channel\":" + channel +
                    ",\"rate\":" + rate + "}";
        }

        void fromJson(String json) {
            try {
                json = json.replace("{", "").replace("}", "").replace("\"", "");
                String[] pairs = json.split(",");

                for (String pair : pairs) {
                    String[] keyValue = pair.split(":");
                    if (keyValue.length == 2) {
                        String key = keyValue[0].trim();
                        int value = Integer.parseInt(keyValue[1].trim());

                        switch (key) {
                            case "power":
                                power = value;
                                break;
                            case "channel":
                                channel = value;
                                break;
                            case "rate":
                                rate = value;
                                break;
                        }
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "Error parseando ESP-NOW config: " + e.getMessage());
            }
        }

        @Override
        public String toString() {
            String rateText = "";
            switch (rate) {
                case 0: rateText = "1 Mbps"; break;
                case 1: rateText = "2 Mbps"; break;
                case 2: rateText = "5.5 Mbps"; break;
                case 3: rateText = "11 Mbps"; break;
            }
            return "Power: " + power + " dBm, Channel: " + channel + ", Rate: " + rateText;
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎯 DETECTAR MODO DEL DISPOSITIVO
    // ════════════════════════════════════════════════════════════════════

    private void detectDeviceMode() {
        // ✅ SOLO detección por nombre (fallback inicial)
        if (deviceName != null) {
            isTxMode = deviceName.toUpperCase().contains("TX");

            Log.d(TAG, "🔍 Modo detectado por nombre: " + (isTxMode ? "TX" : "RX"));

            runOnUiThread(() -> {
                if (isTxMode) {
                    tvLoRaStatus.setText("📡 Modo: TRANSMISOR (detectado por nombre)");
                } else {
                    tvLoRaStatus.setText("📥 Modo: RECEPTOR (detectado por nombre)");
                }
            });
        } else {
            // Si no hay nombre, mostrar estado inicial
            Log.d(TAG, "⚠️ Sin nombre de dispositivo, esperando conexión...");
            runOnUiThread(() -> {
                tvLoRaStatus.setText("🔍 Detectando modo...");
            });
        }

        // ✅ El comando CMD:GET_MODE se enviará automáticamente en onConnected()
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎨 ACTUALIZAR UI DE ESTADO LORA
    // ════════════════════════════════════════════════════════════════════

    private void updateLoRaStatusUI() {
        runOnUiThread(() -> {
            String configText = currentLoRaConfig.toString();
            btnConfigLoRa.setText("⚙️ Config: " + configText);
        });
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎨 MOSTRAR/OCULTAR PROGRESO LORA
    // ════════════════════════════════════════════════════════════════════

    private void showLoRaProgress(boolean show, String text, int progress) {
        runOnUiThread(() -> {
            if (show) {
                layoutLoRaTransmitting.setVisibility(View.VISIBLE);
                tvLoRaProgress.setText(text);
                progressBarLoRa.setProgress(progress);
            } else {
                layoutLoRaTransmitting.setVisibility(View.GONE);
            }
        });
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎨 ACTUALIZAR PROGRESO LORA
    // ════════════════════════════════════════════════════════════════════

    private void updateLoRaProgress(int percentage, String text) {
        runOnUiThread(() -> {
            progressBarLoRa.setProgress(percentage);
            tvLoRaProgress.setText(text);
        });
    }

    // ════════════════════════════════════════════════════════════════════
    // ⚙️ MOSTRAR DIÁLOGO DE CONFIGURACIÓN LORA
    // ════════════════════════════════════════════════════════════════════
    /*
    private void showLoRaConfigDialog() {
        if (!isConnected) {
            Toast.makeText(this, "⚠️ No conectado", Toast.LENGTH_SHORT).show();
            return;
        }

        Log.d(TAG, "⚙️ Mostrando diálogo de configuración LoRa");

        // Crear vista personalizada
        View dialogView = getLayoutInflater().inflate(R.layout.dialog_lora_config, null);

        // Obtener vistas
        Spinner spinnerBW = dialogView.findViewById(R.id.spinnerBW);
        Spinner spinnerSF = dialogView.findViewById(R.id.spinnerSF);
        Spinner spinnerCR = dialogView.findViewById(R.id.spinnerCR);
        Spinner spinnerRepeat = dialogView.findViewById(R.id.spinnerRepeat);  // ✅ CAMBIO
        Spinner spinnerPower = dialogView.findViewById(R.id.spinnerPower);

        // Configurar adaptadores
        ArrayAdapter<String> adapterBW = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item,
                new String[]{"125 kHz", "250 kHz", "500 kHz"});
        adapterBW.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerBW.setAdapter(adapterBW);

        ArrayAdapter<String> adapterSF = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item,
                new String[]{"SF 7", "SF 9", "SF 12"});
        adapterSF.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerSF.setAdapter(adapterSF);

        ArrayAdapter<String> adapterCR = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item,
                new String[]{"4/5", "4/7", "4/8"});
        adapterCR.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerCR.setAdapter(adapterCR);

        // ✅ CAMBIO: Spinner de repeticiones en lugar de ACK
        ArrayAdapter<String> adapterRepeat = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item,
                new String[]{"1 vuelta", "2 vueltas", "3 vueltas", "5 vueltas"});
        adapterRepeat.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerRepeat.setAdapter(adapterRepeat);

        ArrayAdapter<String> adapterPower = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item,
                new String[]{"10 dBm", "14 dBm", "17 dBm", "20 dBm"});
        adapterPower.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerPower.setAdapter(adapterPower);

        // Seleccionar valores actuales
        switch (currentLoRaConfig.bandwidth) {
            case 125: spinnerBW.setSelection(0); break;
            case 250: spinnerBW.setSelection(1); break;
            case 500: spinnerBW.setSelection(2); break;
        }

        switch (currentLoRaConfig.spreadingFactor) {
            case 7: spinnerSF.setSelection(0); break;
            case 9: spinnerSF.setSelection(1); break;
            case 12: spinnerSF.setSelection(2); break;
        }

        switch (currentLoRaConfig.codingRate) {
            case 5: spinnerCR.setSelection(0); break;
            case 7: spinnerCR.setSelection(1); break;
            case 8: spinnerCR.setSelection(2); break;
        }

        // ✅ CAMBIO: Selección de repeticiones
        switch (currentLoRaConfig.repeat) {
            case 1: spinnerRepeat.setSelection(0); break;
            case 2: spinnerRepeat.setSelection(1); break;
            case 3: spinnerRepeat.setSelection(2); break;
            case 5: spinnerRepeat.setSelection(3); break;
        }

        switch (currentLoRaConfig.power) {
            case 10: spinnerPower.setSelection(0); break;
            case 14: spinnerPower.setSelection(1); break;
            case 17: spinnerPower.setSelection(2); break;
            case 20: spinnerPower.setSelection(3); break;
        }

        // Mostrar diálogo
        new AlertDialog.Builder(this)
                .setTitle("⚙️ Configuración LoRa BROADCAST")
                .setMessage("✅ Los cambios se guardarán en la memoria flash del Heltec\n\n")  // ✅ NUEVO
                .setView(dialogView)
                .setPositiveButton("✅ Aplicar y Guardar", (dialog, which) -> {  // ✅ CAMBIO texto
                    // Obtener valores seleccionados
                    String bwText = spinnerBW.getSelectedItem().toString();
                    currentLoRaConfig.bandwidth = Integer.parseInt(bwText.split(" ")[0]);

                    String sfText = spinnerSF.getSelectedItem().toString();
                    currentLoRaConfig.spreadingFactor = Integer.parseInt(sfText.split(" ")[1]);

                    String crText = spinnerCR.getSelectedItem().toString();
                    currentLoRaConfig.codingRate = Integer.parseInt(crText.split("/")[1]);

                    String repeatText = spinnerRepeat.getSelectedItem().toString();
                    currentLoRaConfig.repeat = Integer.parseInt(repeatText.split(" ")[0]);

                    String powerText = spinnerPower.getSelectedItem().toString();
                    currentLoRaConfig.power = Integer.parseInt(powerText.split(" ")[0]);

                    // Enviar configuración al Heltec
                    applyLoRaConfig();
                })
                .setNegativeButton("❌ Cancelar", null)
                .setNeutralButton("🔄 Obtener Actual", (dialog, which) -> {
                    bleManager.sendCommand("CMD:GET_LORA_CONFIG");
                    Toast.makeText(this, "📡 Solicitando configuración guardada...",
                            Toast.LENGTH_SHORT).show();
                })
                .show();
    }
    */

    private void showESPNowConfigDialog() {
        if (!isConnected) {
            Toast.makeText(this, "⚠️ No conectado", Toast.LENGTH_SHORT).show();
            return;
        }

        Log.d(TAG, "⚙️ Mostrando diálogo de configuración ESP-NOW");

        View dialogView = getLayoutInflater().inflate(R.layout.dialog_espnow_config, null);

        Spinner spinnerPower = dialogView.findViewById(R.id.spinnerPower);
        Spinner spinnerChannel = dialogView.findViewById(R.id.spinnerChannel);
        Spinner spinnerRate = dialogView.findViewById(R.id.spinnerRate);

        // Configurar adaptadores
        ArrayAdapter<String> adapterPower = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item,
                new String[]{"2 dBm", "6 dBm", "10 dBm", "14 dBm", "17 dBm", "20 dBm"});
        adapterPower.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerPower.setAdapter(adapterPower);

        String[] channels = new String[13];
        for (int i = 0; i < 13; i++) {
            channels[i] = "Canal " + (i + 1);
        }
        ArrayAdapter<String> adapterChannel = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, channels);
        adapterChannel.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerChannel.setAdapter(adapterChannel);

        ArrayAdapter<String> adapterRate = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item,
                new String[]{"1 Mbps", "2 Mbps", "5.5 Mbps", "11 Mbps"});
        adapterRate.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerRate.setAdapter(adapterRate);

        // Seleccionar valores actuales
        switch (currentESPNowConfig.power) {
            case 2: spinnerPower.setSelection(0); break;
            case 6: spinnerPower.setSelection(1); break;
            case 10: spinnerPower.setSelection(2); break;
            case 14: spinnerPower.setSelection(3); break;
            case 17: spinnerPower.setSelection(4); break;
            case 20: spinnerPower.setSelection(5); break;
        }
        spinnerChannel.setSelection(currentESPNowConfig.channel - 1);
        spinnerRate.setSelection(currentESPNowConfig.rate);

        new AlertDialog.Builder(this)
                .setTitle("⚙️ Configuración ESP-NOW")
                .setMessage("✅ Los cambios se guardarán en la memoria flash del Heltec\n\n")
                .setView(dialogView)
                .setPositiveButton("✅ Aplicar y Guardar", (dialog, which) -> {
                    String powerText = spinnerPower.getSelectedItem().toString();
                    currentESPNowConfig.power = Integer.parseInt(powerText.split(" ")[0]);

                    String channelText = spinnerChannel.getSelectedItem().toString();
                    currentESPNowConfig.channel = Integer.parseInt(channelText.split(" ")[1]);

                    String rateText = spinnerRate.getSelectedItem().toString();
                    switch (rateText) {
                        case "1 Mbps": currentESPNowConfig.rate = 0; break;
                        case "2 Mbps": currentESPNowConfig.rate = 1; break;
                        case "5.5 Mbps": currentESPNowConfig.rate = 2; break;
                        case "11 Mbps": currentESPNowConfig.rate = 3; break;
                    }

                    applyESPNowConfig();
                })
                .setNegativeButton("❌ Cancelar", null)
                .setNeutralButton("🔄 Obtener Actual", (dialog, which) -> {
                    bleManager.sendCommand("CMD:GET_ESPNOW_CONFIG");
                    Toast.makeText(this, "📡 Solicitando configuración guardada...",
                            Toast.LENGTH_SHORT).show();
                })
                .show();
    }


    // ════════════════════════════════════════════════════════════════════
    // ⚙️ APLICAR CONFIGURACIÓN LORA
    // ════════════════════════════════════════════════════════════════════
    /*
    private void applyLoRaConfig() {
        Log.d(TAG, "⚙️ Aplicando configuración LoRa: " + currentLoRaConfig.toString());

        String command = "CMD:SET_LORA_CONFIG:" + currentLoRaConfig.toJson();
        bleManager.sendCommand(command);

        updateLoRaStatusUI();

        // ✅ CAMBIO: Informar que se guarda persistentemente
        Toast.makeText(this, "✅ Config enviada y guardada en el Heltec", Toast.LENGTH_SHORT).show();
    }
    */
    private void applyESPNowConfig() {
        Log.d(TAG, "⚙️ Aplicando configuración ESP-NOW: " + currentESPNowConfig.toString());

        String command = "CMD:SET_ESPNOW_CONFIG:" + currentESPNowConfig.toJson();
        bleManager.sendCommand(command);

        updateESPNowStatusUI();

        Toast.makeText(this, "✅ Config enviada y guardada en el Heltec", Toast.LENGTH_SHORT).show();
    }

    private void updateESPNowStatusUI() {
        runOnUiThread(() -> {
            String configText = currentESPNowConfig.toString();
            btnConfigLoRa.setText("⚙️ Config: " + configText);
        });
    }

    // ════════════════════════════════════════════════════════════════════
    // 📡 TRANSMITIR ARCHIVO POR LORA (solo TX)
    // ════════════════════════════════════════════════════════════════════

    // 📡 TRANSMITIR ARCHIVO POR LORA (línea 1274)
    private void transmitFileViaLoRa(FileInfo fileInfo) {
        if (!isTxMode) {
            Toast.makeText(this, "⚠️ Solo disponible en modo TX",
                    Toast.LENGTH_SHORT).show();
            return;
        }

        if (isTransmitting) {
            Toast.makeText(this, "⚠️ Ya hay una transmisión en progreso",
                    Toast.LENGTH_SHORT).show();
            return;
        }

        Log.d(TAG, "📡 Preparando transmisión LoRa BROADCAST: " + fileInfo.name);

        final FileInfo file = fileInfo;

        // Mostrar confirmación
        new AlertDialog.Builder(this)
                .setTitle("📡 Transmitir por LoRa BROADCAST")  // ✅ CAMBIO
                .setMessage("¿Transmitir '" + file.name + "' por LoRa?\n\n" +
                        "⚠️ MODO BROADCAST:\n" +  // ✅ NUEVO
                        "- Sin confirmación del receptor\n" +
                        "- Transmisión repetida (" + currentLoRaConfig.repeat + " vueltas)\n" +
                        "- FEC + Interleaving activados\n\n" +
                        "Tamaño: " + formatFileSize(file.size) + "\n" +
                        "Configuración: " + currentLoRaConfig.toString() + "\n\n" +
                        "Asegúrate de que el RX esté escuchando.")
                .setPositiveButton("📡 Transmitir", (dialog, which) -> {
                    isTransmitting = true;
                    showLoRaProgress(true, "Iniciando transmisión BROADCAST...", 0);

                    bleManager.sendCommand("CMD:TX_FILE:" + file.name);

                    Toast.makeText(this, "📡 Transmitiendo en BROADCAST...",
                            Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }


}