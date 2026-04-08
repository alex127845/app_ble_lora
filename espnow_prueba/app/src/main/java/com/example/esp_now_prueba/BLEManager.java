package com.example.esp_now_prueba;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import androidx.core.app.ActivityCompat;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Queue;
import java.util.UUID;
import java.util.concurrent.ConcurrentLinkedQueue;

public class BLEManager {

    private static final String TAG = "BLEManager";

    // ════════════════════════════════════════════════════════════════════
    // 🔧 CONSTANTES - UUIDs del Heltec
    // ════════════════════════════════════════════════════════════════════

    private static final UUID SERVICE_UUID =
            UUID.fromString("4fafc201-1fb5-459e-8fcc-c5c9c331914b");

    private static final UUID CMD_WRITE_UUID =
            UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a8");

    private static final UUID DATA_READ_UUID =
            UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a9");

    private static final UUID PROGRESS_UUID =
            UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26aa");

    // Descriptor para habilitar notificaciones
    private static final UUID CCCD_UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    // Configuración
    private static final int MAX_MTU = 517; // MTU máximo solicitado
    private static final int WRITE_DELAY = 50; // Delay entre escrituras (ms)
    private static final int RECONNECT_DELAY = 3000; // Delay para reconexión (ms)
    private static final int MAX_RECONNECT_ATTEMPTS = 3;

    // ════════════════════════════════════════════════════════════════════
    // 🌐 VARIABLES DE INSTANCIA
    // ════════════════════════════════════════════════════════════════════

    private Context context;
    private BLECallback callback;

    // Bluetooth
    private BluetoothManager bluetoothManager;
    private BluetoothAdapter bluetoothAdapter;
    private BluetoothGatt bluetoothGatt;
    private BluetoothDevice bluetoothDevice;

    // Características BLE
    private BluetoothGattCharacteristic cmdCharacteristic;
    private BluetoothGattCharacteristic dataCharacteristic;
    private BluetoothGattCharacteristic progressCharacteristic;

    // Estado de conexión
    private boolean isConnected = false;
    private boolean isConnecting = false;
    private int reconnectAttempts = 0;

    // Cola de comandos
    private Queue<String> commandQueue = new ConcurrentLinkedQueue<>();
    private boolean isWriting = false;

    // Buffer para datos recibidos
    private StringBuilder dataBuffer = new StringBuilder();

    // Handler para operaciones asíncronas
    private Handler handler = new Handler(Looper.getMainLooper());

    // ════════════════════════════════════════════════════════════════════
    // 📞 INTERFACE DE CALLBACKS
    // ════════════════════════════════════════════════════════════════════

    /**
     * Interface para recibir eventos del BLEManager
     */
    public interface BLECallback {
        /**
         * Se llamó cuando la conexión se estableció correctamente
         */
        void onConnected();

        /**
         * Se llamó cuando se perdió la conexión
         */
        void onDisconnected();

        /**
         * Se llamó cuando se reciben datos del Heltec
         * @param data Datos recibidos como String
         */
        void onDataReceived(String data);

        /**
         * Se llamó cuando se recibe un update de progreso
         * @param percentage Porcentaje (0-100)
         */
        void onProgress(int percentage);

        /**
         * Se llamó cuando ocurre un error
         * @param error Mensaje de error
         */
        void onError(String error);
    }

    // ════════════════════════════════════════════════════════════════════
    // 🏗️ CONSTRUCTOR
    // ════════════════════════════════════════════════════════════════════

    /**
     * Constructor del BLEManager
     *
     * @param context Contexto de la aplicación
     * @param callback Callback para eventos BLE
     */
    public BLEManager(Context context, BLECallback callback) {
        this.context = context;
        this.callback = callback;

        Log.d(TAG, "🔧 BLEManager inicializado");

        // Obtener BluetoothManager
        bluetoothManager = (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (bluetoothManager != null) {
            bluetoothAdapter = bluetoothManager.getAdapter();
            Log.d(TAG, "✅ BluetoothAdapter obtenido");
        } else {
            Log.e(TAG, "❌ BluetoothManager no disponible");
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 🔌 CONECTAR AL DISPOSITIVO
    // ════════════════════════════════════════════════════════════════════

    /**
     * Conectar al dispositivo Heltec por dirección MAC
     *
     * @param deviceAddress Dirección MAC del dispositivo (formato: XX:XX:XX:XX:XX:XX)
     */
    public void connect(String deviceAddress) {
        Log.d(TAG, "🔌 Intentando conectar a: " + deviceAddress);

        if (bluetoothAdapter == null) {
            Log.e(TAG, "❌ BluetoothAdapter no disponible");
            if (callback != null) {
                callback.onError("Bluetooth no disponible");
            }
            return;
        }

        // Verificar si ya está conectado
        if (isConnected || isConnecting) {
            Log.w(TAG, "⚠️ Ya conectado o conectando");
            return;
        }

        // Obtener dispositivo
        try {
            bluetoothDevice = bluetoothAdapter.getRemoteDevice(deviceAddress);
            Log.d(TAG, "✅ Dispositivo obtenido: " + bluetoothDevice.getAddress());
        } catch (IllegalArgumentException e) {
            Log.e(TAG, "❌ Dirección MAC inválida: " + e.getMessage());
            if (callback != null) {
                callback.onError("Dirección MAC inválida");
            }
            return;
        }

        // Verificar permisos
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ActivityCompat.checkSelfPermission(context,
                    android.Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) {
                Log.e(TAG, "❌ Sin permiso BLUETOOTH_CONNECT");
                if (callback != null) {
                    callback.onError("Permiso BLUETOOTH_CONNECT requerido");
                }
                return;
            }
        }

        // Conectar GATT
        isConnecting = true;
        reconnectAttempts = 0;

        Log.d(TAG, "📡 Conectando GATT...");
        bluetoothGatt = bluetoothDevice.connectGatt(
                context,
                false,  // autoConnect = false para conexión rápida
                gattCallback,
                BluetoothDevice.TRANSPORT_LE // Forzar BLE
        );
    }

    // ════════════════════════════════════════════════════════════════════
    // 🔌 DESCONECTAR DEL DISPOSITIVO
    // ════════════════════════════════════════════════════════════════════

    /**
     * Desconectar del dispositivo Heltec
     */
    public void disconnect() {
        Log.d(TAG, "🔌 Desconectando...");

        isConnected = false;
        isConnecting = false;
        reconnectAttempts = MAX_RECONNECT_ATTEMPTS; // Prevenir reconexión

        // Limpiar cola de comandos
        commandQueue.clear();
        isWriting = false;

        // Desconectar GATT
        if (bluetoothGatt != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (ActivityCompat.checkSelfPermission(context,
                        android.Manifest.permission.BLUETOOTH_CONNECT)
                        == PackageManager.PERMISSION_GRANTED) {
                    bluetoothGatt.disconnect();
                    bluetoothGatt.close();
                }
            } else {
                bluetoothGatt.disconnect();
                bluetoothGatt.close();
            }
            bluetoothGatt = null;
        }

        // Limpiar características
        cmdCharacteristic = null;
        dataCharacteristic = null;
        progressCharacteristic = null;

        Log.d(TAG, "✅ Desconectado");
    }

    // ════════════════════════════════════════════════════════════════════
    // 📤 ENVIAR COMANDO
    // ════════════════════════════════════════════════════════════════════

    /**
     * Enviar comando al Heltec
     * Los comandos se encolan para evitar saturar el BLE
     *
     * @param command Comando a enviar (sin \n al final)
     */
    public void sendCommand(String command) {
        if (!isConnected) {
            Log.w(TAG, "⚠️ No conectado, comando no enviado: " + command);
            return;
        }

        Log.d(TAG, "📤 Encolando comando: " + command);

        // Agregar a cola
        commandQueue.offer(command);

        // Procesar cola si no está escribiendo
        if (!isWriting) {
            processCommandQueue();
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📤 PROCESAR COLA DE COMANDOS
    // ════════════════════════════════════════════════════════════════════

    /**
     * Procesa la cola de comandos uno por uno con delay
     */
    private void processCommandQueue() {
        if (commandQueue.isEmpty() || isWriting) {
            return;
        }

        String command = commandQueue.poll();
        if (command == null) return;

        isWriting = true;

        Log.d(TAG, "✍️ Escribiendo comando: " + command);

        // Agregar \n al final
        if (!command.endsWith("\n")) {
            command += "\n";
        }

        // Escribir a característica
        writeCharacteristic(command);

        // Esperar antes del siguiente comando
        handler.postDelayed(() -> {
            isWriting = false;
            processCommandQueue();
        }, WRITE_DELAY);
    }

    // ════════════════════════════════════════════════════════════════════
    // ✍️ ESCRIBIR CARACTERÍSTICA
    // ════════════════════════════════════════════════════════════════════

    /**
     * Escribe datos a la característica CMD_WRITE
     *
     * @param data Datos a escribir
     */
    private void writeCharacteristic(String data) {
        if (cmdCharacteristic == null || bluetoothGatt == null) {
            Log.e(TAG, "❌ Característica o GATT no disponibles");
            return;
        }

        // Verificar permisos
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ActivityCompat.checkSelfPermission(context,
                    android.Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) {
                Log.e(TAG, "❌ Sin permiso BLUETOOTH_CONNECT");
                return;
            }
        }

        try {
            byte[] bytes = data.getBytes(StandardCharsets.UTF_8);

            // Android 13+ (API 33+) usa nuevo método
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                int result = bluetoothGatt.writeCharacteristic(
                        cmdCharacteristic,
                        bytes,
                        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                );

                if (result != BluetoothGatt.GATT_SUCCESS) {
                    Log.e(TAG, "❌ Error escribiendo (nuevo): " + result);
                }
            } else {
                // Android 12 y anteriores
                cmdCharacteristic.setValue(bytes);
                cmdCharacteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                boolean success = bluetoothGatt.writeCharacteristic(cmdCharacteristic);

                if (!success) {
                    Log.e(TAG, "❌ Error escribiendo (legacy)");
                }
            }

            Log.d(TAG, "✅ Comando escrito: " + data.trim() + " (" + bytes.length + " bytes)");

        } catch (Exception e) {
            Log.e(TAG, "❌ Excepción escribiendo: " + e.getMessage());
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📡 GATT CALLBACK - Eventos del Bluetooth
    // ════════════════════════════════════════════════════════════════════

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {

        /**
         * Cambio de estado de conexión
         */
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (ActivityCompat.checkSelfPermission(context,
                        android.Manifest.permission.BLUETOOTH_CONNECT)
                        != PackageManager.PERMISSION_GRANTED) {
                    return;
                }
            }

            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.d(TAG, "🟢 Conectado a GATT (status: " + status + ")");

                isConnecting = false;
                reconnectAttempts = 0;

                // Solicitar MTU máximo para mejor rendimiento
                Log.d(TAG, "📏 Solicitando MTU: " + MAX_MTU);
                gatt.requestMtu(MAX_MTU);

            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.d(TAG, "🔴 Desconectado de GATT (status: " + status + ")");

                isConnected = false;
                isConnecting = false;

                // Notificar desconexión
                if (callback != null) {
                    handler.post(() -> callback.onDisconnected());
                }

                // Intentar reconexión si no fue intencional
                if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
                    reconnectAttempts++;
                    Log.d(TAG, "🔄 Reintentando conexión (" + reconnectAttempts + "/" +
                            MAX_RECONNECT_ATTEMPTS + ")");

                    handler.postDelayed(() -> {
                        if (bluetoothDevice != null && !isConnected) {
                            connect(bluetoothDevice.getAddress());
                        }
                    }, RECONNECT_DELAY);
                } else {
                    Log.e(TAG, "❌ Máximo de reintentos alcanzado");
                    if (callback != null) {
                        handler.post(() -> callback.onError("Conexión perdida"));
                    }
                }
            }
        }

        /**
         * MTU cambiado
         */
        @Override
        public void onMtuChanged(BluetoothGatt gatt, int mtu, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "✅ MTU cambiado a: " + mtu);
            } else {
                Log.w(TAG, "⚠️ Error cambiando MTU (status: " + status + ")");
            }

            // Descubrir servicios
            Log.d(TAG, "🔍 Descubriendo servicios...");
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (ActivityCompat.checkSelfPermission(context,
                        android.Manifest.permission.BLUETOOTH_CONNECT)
                        == PackageManager.PERMISSION_GRANTED) {
                    gatt.discoverServices();
                }
            } else {
                gatt.discoverServices();
            }
        }

        /**
         * Servicios descubiertos
         */
        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "✅ Servicios descubiertos");

                // Obtener servicio del Heltec
                BluetoothGattService service = gatt.getService(SERVICE_UUID);

                if (service == null) {
                    Log.e(TAG, "❌ Servicio no encontrado: " + SERVICE_UUID);
                    if (callback != null) {
                        handler.post(() -> callback.onError("Servicio BLE no encontrado"));
                    }
                    return;
                }

                Log.d(TAG, "✅ Servicio encontrado");

                // Obtener características
                cmdCharacteristic = service.getCharacteristic(CMD_WRITE_UUID);
                dataCharacteristic = service.getCharacteristic(DATA_READ_UUID);
                progressCharacteristic = service.getCharacteristic(PROGRESS_UUID);

                if (cmdCharacteristic == null || dataCharacteristic == null) {
                    Log.e(TAG, "❌ Características no encontradas");
                    if (callback != null) {
                        handler.post(() -> callback.onError("Características BLE no encontradas"));
                    }
                    return;
                }

                Log.d(TAG, "✅ Características encontradas");

                // Habilitar notificaciones en DATA_READ
                enableNotifications(gatt, dataCharacteristic);

                // Habilitar notificaciones en PROGRESS (si existe)
                if (progressCharacteristic != null) {
                    handler.postDelayed(() -> {
                        enableNotifications(gatt, progressCharacteristic);
                    }, 100);
                }

                // Marcar como conectado
                isConnected = true;

                // Notificar conexión exitosa
                if (callback != null) {
                    handler.post(() -> callback.onConnected());
                }

                Log.d(TAG, "🎉 Conexión BLE establecida completamente");

            } else {
                Log.e(TAG, "❌ Error descubriendo servicios (status: " + status + ")");
                if (callback != null) {
                    handler.post(() -> callback.onError("Error descubriendo servicios"));
                }
            }
        }

        /**
         * Característica cambiada (notificación recibida)
         */
        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt,
                                            BluetoothGattCharacteristic characteristic) {

            UUID uuid = characteristic.getUuid();

            // Datos recibidos (DATA_READ)
            if (DATA_READ_UUID.equals(uuid)) {
                byte[] data;

                // Android 13+ usa nuevo método
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    data = characteristic.getValue();
                } else {
                    data = characteristic.getValue();
                }

                if (data != null && data.length > 0) {
                    String received = new String(data, StandardCharsets.UTF_8);

                    // Acumular datos en buffer
                    dataBuffer.append(received);

                    // Procesar si termina en \n
                    if (received.endsWith("\n")) {
                        String completeMessage = dataBuffer.toString().trim();
                        dataBuffer.setLength(0); // Limpiar buffer

                        Log.d(TAG, "📥 Datos recibidos: " + completeMessage);

                        if (callback != null) {
                            final String msg = completeMessage;
                            handler.post(() -> callback.onDataReceived(msg));
                        }
                    }
                }
            }

            // Progreso recibido (PROGRESS)
            else if (PROGRESS_UUID.equals(uuid)) {
                byte[] data;

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    data = characteristic.getValue();
                } else {
                    data = characteristic.getValue();
                }

                if (data != null && data.length > 0) {
                    int percentage = data[0] & 0xFF; // Convertir a unsigned

                    Log.d(TAG, "📊 Progreso: " + percentage + "%");

                    if (callback != null) {
                        handler.post(() -> callback.onProgress(percentage));
                    }
                }
            }
        }
        /**Descriptor escrito (para habilitar notificaciones)*/
        @Override
        public void onDescriptorWrite(BluetoothGatt gatt,
                                      BluetoothGattDescriptor descriptor,
                                      int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "✅ Notificaciones habilitadas en: " +
                        descriptor.getCharacteristic().getUuid());
            } else {
                Log.e(TAG, "❌ Error habilitando notificaciones (status: " + status + ")");
            }
        }
    };
    // ════════════════════════════════════════════════════════════════════
    // 🔔 HABILITAR NOTIFICACIONES
    // ════════════════════════════════════════════════════════════════════
    /**
     * Habilitar notificaciones en una característica
     * @param gatt Instancia de BluetoothGatt
     * @param characteristic Característica en la que habilitar notificaciones
     */
    private void enableNotifications(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ActivityCompat.checkSelfPermission(context,
                    android.Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) {
                return;
            }
        }
        Log.d(TAG, "🔔 Habilitando notificaciones en: " + characteristic.getUuid());
        // Habilitar notificaciones localmente
        boolean success = gatt.setCharacteristicNotification(characteristic, true);

        if (!success) {
            Log.e(TAG, "❌ Error habilitando notificaciones localmente");
            return;
        }

        // Habilitar notificaciones en el descriptor
        BluetoothGattDescriptor descriptor = characteristic.getDescriptor(CCCD_UUID);

        if (descriptor == null) {
            Log.e(TAG, "❌ Descriptor CCCD no encontrado");
            return;
        }

        // Android 13+ usa nuevo método
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            int result = gatt.writeDescriptor(
                    descriptor,
                    BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            );

            if (result != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "❌ Error escribiendo descriptor (nuevo): " + result);
            }
        } else {
            // Android 12 y anteriores
            descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            boolean writeSuccess = gatt.writeDescriptor(descriptor);

            if (!writeSuccess) {
                Log.e(TAG, "❌ Error escribiendo descriptor (legacy)");
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📊 GETTERS
    // ════════════════════════════════════════════════════════════════════

    /**@return true si está conectado**/
    public boolean isConnected() {
        return isConnected;
    }
    /**@return true si está conectando**/
    public boolean isConnecting() {
        return isConnecting;
    }
}


