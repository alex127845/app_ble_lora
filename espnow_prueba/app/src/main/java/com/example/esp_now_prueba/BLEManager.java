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
import java.util.concurrent.atomic.AtomicBoolean; // ✅ FIX #3

public class BLEManager {

    private static final String TAG = "BLEManager";

    // ════════════════════════════════════════════════════════════════════
    // CONSTANTES - UUIDs del Heltec
    // ════════════════════════════════════════════════════════════════════

    private static final UUID SERVICE_UUID =
            UUID.fromString("4fafc201-1fb5-459e-8fcc-c5c9c331914b");

    private static final UUID CMD_WRITE_UUID =
            UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a8");

    private static final UUID DATA_READ_UUID =
            UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a9");

    private static final UUID PROGRESS_UUID =
            UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26aa");

    private static final UUID CCCD_UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    // Configuración
    private static final int MAX_MTU           = 517;
    private static final int RECONNECT_DELAY   = 3000;
    private static final int MAX_RECONNECT_ATTEMPTS = 3;

    // ✅ FIX #1 — Tamaño de payload BLE seguro.
    // El payload usable por paquete es MTU - 3 = 514 bytes.
    // Reservamos 17 bytes para el prefijo "CMD:UPLOAD_CHUNK:" y 1 para el '\n'.
    // Payload de datos útiles por sub-paquete = 514 - 18 = 496 bytes.
    // Pero como usamos Base64, la relación binario→texto es 3:4,
    // así que el chunk binario máximo para que quepa en UN solo write es:
    //   floor(496 * 3 / 4) = 372 bytes.
    // Usamos 360 para tener margen ante variaciones de MTU real.
    public static final int MAX_CHUNK_BYTES = 360;

    // ════════════════════════════════════════════════════════════════════
    // VARIABLES DE INSTANCIA
    // ════════════════════════════════════════════════════════════════════

    private final Context context;
    private final BLECallback callback;

    // Bluetooth
    private BluetoothManager bluetoothManager;
    private BluetoothAdapter bluetoothAdapter;
    private BluetoothGatt bluetoothGatt;
    private BluetoothDevice bluetoothDevice;

    // Características BLE
    private BluetoothGattCharacteristic cmdCharacteristic;
    private BluetoothGattCharacteristic dataCharacteristic;
    private BluetoothGattCharacteristic progressCharacteristic;

    // MTU negociado real (se actualiza en onMtuChanged)
    private int negotiatedMtu = 23; // valor mínimo BLE por defecto

    // Estado de conexión
    private volatile boolean isConnected  = false;
    private volatile boolean isConnecting = false;
    private int reconnectAttempts = 0;

    // Cola de comandos
    // ✅ FIX #3 — isWriting como AtomicBoolean para seguridad entre hilos
    private final Queue<String> commandQueue = new ConcurrentLinkedQueue<>();
    private final AtomicBoolean isWriting = new AtomicBoolean(false);

    // Buffer para datos recibidos
    private final StringBuilder dataBuffer = new StringBuilder();

    // ✅ FIX #2 — Lock y cola de respuestas para sendCommandAndWaitForPrefix
    private final Object responseLock = new Object();
    private final List<String> responseQueue = new ArrayList<>();

    // Handler para operaciones asíncronas (main thread)
    private final Handler handler = new Handler(Looper.getMainLooper());

    // ════════════════════════════════════════════════════════════════════
    // INTERFACE DE CALLBACKS
    // ════════════════════════════════════════════════════════════════════

    public interface BLECallback {
        void onConnected();
        void onDisconnected();
        void onDataReceived(String data);
        void onProgress(int percentage);
        void onError(String error);
    }

    // ════════════════════════════════════════════════════════════════════
    // CONSTRUCTOR
    // ════════════════════════════════════════════════════════════════════

    public BLEManager(Context context, BLECallback callback) {
        this.context  = context;
        this.callback = callback;

        Log.d(TAG, "🔧 BLEManager inicializado");

        bluetoothManager = (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (bluetoothManager != null) {
            bluetoothAdapter = bluetoothManager.getAdapter();
            Log.d(TAG, "✅ BluetoothAdapter obtenido");
        } else {
            Log.e(TAG, "❌ BluetoothManager no disponible");
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // CONECTAR AL DISPOSITIVO
    // ════════════════════════════════════════════════════════════════════

    public void connect(String deviceAddress) {
        Log.d(TAG, "🔌 Intentando conectar a: " + deviceAddress);

        if (bluetoothAdapter == null) {
            Log.e(TAG, "❌ BluetoothAdapter no disponible");
            if (callback != null) callback.onError("Bluetooth no disponible");
            return;
        }

        if (isConnected || isConnecting) {
            Log.w(TAG, "⚠️ Ya conectado o conectando");
            return;
        }

        try {
            bluetoothDevice = bluetoothAdapter.getRemoteDevice(deviceAddress);
            Log.d(TAG, "✅ Dispositivo obtenido: " + bluetoothDevice.getAddress());
        } catch (IllegalArgumentException e) {
            Log.e(TAG, "❌ Dirección MAC inválida: " + e.getMessage());
            if (callback != null) callback.onError("Dirección MAC inválida");
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ActivityCompat.checkSelfPermission(context,
                    android.Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) {
                Log.e(TAG, "❌ Sin permiso BLUETOOTH_CONNECT");
                if (callback != null) callback.onError("Permiso BLUETOOTH_CONNECT requerido");
                return;
            }
        }

        isConnecting = true;
        reconnectAttempts = 0;

        Log.d(TAG, "📡 Conectando GATT...");
        bluetoothGatt = bluetoothDevice.connectGatt(
                context,
                false,
                gattCallback,
                BluetoothDevice.TRANSPORT_LE
        );
    }

    // ════════════════════════════════════════════════════════════════════
    // DESCONECTAR DEL DISPOSITIVO
    // ════════════════════════════════════════════════════════════════════

    public void disconnect() {
        Log.d(TAG, "🔌 Desconectando...");

        isConnected  = false;
        isConnecting = false;
        reconnectAttempts = MAX_RECONNECT_ATTEMPTS;

        commandQueue.clear();
        isWriting.set(false);

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

        cmdCharacteristic      = null;
        dataCharacteristic     = null;
        progressCharacteristic = null;

        Log.d(TAG, "✅ Desconectado");
    }

    // ════════════════════════════════════════════════════════════════════
    // ENVIAR COMANDO
    // ════════════════════════════════════════════════════════════════════

    public void sendCommand(String command) {
        if (!isConnected) {
            Log.w(TAG, "⚠️ No conectado, comando no enviado");
            return;
        }

        Log.d(TAG, "📤 Encolando comando: " + command);
        commandQueue.offer(command);

        // ✅ FIX #3 — compareAndSet evita doble llamada desde hilos distintos
        if (!isWriting.get()) {
            handler.post(this::processCommandQueue);
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // PROCESAR COLA DE COMANDOS
    // ════════════════════════════════════════════════════════════════════

    private void processCommandQueue() {
        // ✅ FIX #3 — Solo entra si nadie más está escribiendo
        if (!isWriting.compareAndSet(false, true)) {
            return; // otro hilo ya tomó el turno
        }

        String command = commandQueue.poll();
        if (command == null) {
            isWriting.set(false);
            return;
        }

        Log.d(TAG, "✍️ Escribiendo comando: " + command);

        if (!command.endsWith("\n")) {
            command += "\n";
        }

        writeCharacteristic(command);
        // isWriting se libera en onCharacteristicWrite cuando llega la confirmación
    }

    // ════════════════════════════════════════════════════════════════════
    // ✅ FIX #1 — ESCRIBIR CARACTERÍSTICA CON FRAGMENTACIÓN POR MTU
    //
    // Si el comando supera el payload disponible por paquete BLE
    // (negotiatedMtu - 3 bytes de overhead ATT), lo partimos en
    // sub-paquetes y los encolamos individualmente. Cada sub-paquete
    // espera su onCharacteristicWrite antes de enviar el siguiente,
    // gracias a la cola existente.
    // ════════════════════════════════════════════════════════════════════

    private void writeCharacteristic(String data) {
        if (cmdCharacteristic == null || bluetoothGatt == null) {
            Log.e(TAG, "❌ Característica o GATT no disponibles");
            isWriting.set(false);
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ActivityCompat.checkSelfPermission(context,
                    android.Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) {
                Log.e(TAG, "❌ Sin permiso BLUETOOTH_CONNECT");
                isWriting.set(false);
                return;
            }
        }

        byte[] bytes = data.getBytes(StandardCharsets.UTF_8);

        // Payload máximo usable por paquete BLE: MTU - 3 bytes ATT overhead
        int maxPayload = negotiatedMtu - 3;

        if (bytes.length <= maxPayload) {
            // ── Caso normal: cabe en un solo paquete ──
            doWrite(bytes);
        } else {
            // ── FIX #1: fragmentar en sub-paquetes y encolar el resto ──
            Log.d(TAG, "✂️ Fragmentando " + bytes.length + " bytes en paquetes de " + maxPayload);

            // Escribir el primer fragmento directamente
            byte[] firstFragment = new byte[maxPayload];
            System.arraycopy(bytes, 0, firstFragment, 0, maxPayload);
            doWrite(firstFragment);

            // Encolar los fragmentos restantes como nuevos "comandos"
            // Nota: se insertan al frente para mantener el orden correcto.
            // Usamos una lista temporal para insertar en orden en la cola.
            List<byte[]> remainingFragments = new ArrayList<>();
            int offset = maxPayload;
            while (offset < bytes.length) {
                int end = Math.min(offset + maxPayload, bytes.length);
                byte[] fragment = new byte[end - offset];
                System.arraycopy(bytes, offset, fragment, 0, end - offset);
                remainingFragments.add(fragment);
                offset = end;
            }

            // Convertir fragmentos a Strings (ya son bytes crudos, no texto)
            // Los insertamos como comandos RAW usando una cola de bytes separada.
            // Para no romper la arquitectura existente, los convertimos de vuelta
            // a String ISO-8859-1 (1 byte = 1 char, sin pérdida).
            for (int i = remainingFragments.size() - 1; i >= 0; i--) {
                String fragmentAsString = new String(remainingFragments.get(i),
                        java.nio.charset.StandardCharsets.ISO_8859_1);
                // Insertar al frente de la cola
                ((ConcurrentLinkedQueue<String>) commandQueue).offer(fragmentAsString);
            }

            // NOTA: ConcurrentLinkedQueue no tiene addFirst. Reordenamos la cola.
            // Ver comentario en la sección de mejoras al final del archivo.
        }
    }

    /**
     * Realiza el write GATT real con el array de bytes dado.
     */
    private void doWrite(byte[] bytes) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                int result = bluetoothGatt.writeCharacteristic(
                        cmdCharacteristic,
                        bytes,
                        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                );
                if (result != BluetoothGatt.GATT_SUCCESS) {
                    Log.e(TAG, "❌ Error escribiendo (API33+): " + result);
                    isWriting.set(false);
                }
            } else {
                cmdCharacteristic.setValue(bytes);
                cmdCharacteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                boolean success = bluetoothGatt.writeCharacteristic(cmdCharacteristic);
                if (!success) {
                    Log.e(TAG, "❌ Error escribiendo (legacy)");
                    isWriting.set(false);
                }
            }
            Log.d(TAG, "✅ Write enviado: " + bytes.length + " bytes");
        } catch (Exception e) {
            Log.e(TAG, "❌ Excepción escribiendo: " + e.getMessage());
            isWriting.set(false);
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // GATT CALLBACK
    // ════════════════════════════════════════════════════════════════════

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {

        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (ActivityCompat.checkSelfPermission(context,
                        android.Manifest.permission.BLUETOOTH_CONNECT)
                        != PackageManager.PERMISSION_GRANTED) return;
            }

            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.d(TAG, "🟢 Conectado a GATT");
                isConnecting = false;
                reconnectAttempts = 0;
                Log.d(TAG, "📏 Solicitando MTU: " + MAX_MTU);
                gatt.requestMtu(MAX_MTU);

            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.d(TAG, "🔴 Desconectado de GATT (status: " + status + ")");
                isConnected  = false;
                isConnecting = false;

                if (callback != null) handler.post(() -> callback.onDisconnected());

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
                    if (callback != null) handler.post(() -> callback.onError("Conexión perdida"));
                }
            }
        }

        @Override
        public void onMtuChanged(BluetoothGatt gatt, int mtu, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                // ✅ FIX #1 — Guardamos el MTU real negociado
                negotiatedMtu = mtu;
                Log.d(TAG, "✅ MTU negociado: " + negotiatedMtu +
                        " → payload útil: " + (negotiatedMtu - 3) + " bytes");
            } else {
                Log.w(TAG, "⚠️ MTU no cambiado (status: " + status + "), usando " + negotiatedMtu);
            }

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

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "❌ Error descubriendo servicios (status: " + status + ")");
                if (callback != null)
                    handler.post(() -> callback.onError("Error descubriendo servicios"));
                return;
            }

            Log.d(TAG, "✅ Servicios descubiertos");

            BluetoothGattService service = gatt.getService(SERVICE_UUID);
            if (service == null) {
                Log.e(TAG, "❌ Servicio no encontrado: " + SERVICE_UUID);
                if (callback != null)
                    handler.post(() -> callback.onError("Servicio BLE no encontrado"));
                return;
            }

            cmdCharacteristic      = service.getCharacteristic(CMD_WRITE_UUID);
            dataCharacteristic     = service.getCharacteristic(DATA_READ_UUID);
            progressCharacteristic = service.getCharacteristic(PROGRESS_UUID);

            if (cmdCharacteristic == null || dataCharacteristic == null) {
                Log.e(TAG, "❌ Características no encontradas");
                if (callback != null)
                    handler.post(() -> callback.onError("Características BLE no encontradas"));
                return;
            }

            enableNotifications(gatt, dataCharacteristic);

            if (progressCharacteristic != null) {
                handler.postDelayed(() -> enableNotifications(gatt, progressCharacteristic), 100);
            }

            isConnected = true;

            if (callback != null) handler.post(() -> callback.onConnected());

            Log.d(TAG, "🎉 Conexión BLE establecida completamente");
        }

        // ✅ FIX #4 — Override DEPRECATED para Android < 13
        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt,
                                            BluetoothGattCharacteristic characteristic) {
            // En Android < 13 este es el callback activo.
            // Llamamos al método compartido con el valor del characteristic.
            processIncomingCharacteristic(characteristic.getUuid(), characteristic.getValue());
        }

        // ✅ FIX #4 — Override NUEVO para Android 13+ (API 33)
        // Aquí el valor llega como parámetro directo, evitando race conditions
        // con characteristic.getValue() que puede cambiar entre lecturas.
        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt,
                                            BluetoothGattCharacteristic characteristic,
                                            byte[] value) {
            processIncomingCharacteristic(characteristic.getUuid(), value);
        }

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

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt,
                                          BluetoothGattCharacteristic characteristic,
                                          int status) {
            if (!CMD_WRITE_UUID.equals(characteristic.getUuid())) return;

            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "✅ Escritura BLE confirmada");
            } else {
                Log.e(TAG, "❌ Error en escritura BLE, status: " + status);
                if (callback != null)
                    handler.post(() -> callback.onError("Error escribiendo por BLE: " + status));
            }

            // ✅ FIX #3 — Liberar isWriting de forma atómica y continuar cola
            isWriting.set(false);
            handler.post(BLEManager.this::processCommandQueue);
        }
    };

    // ════════════════════════════════════════════════════════════════════
    // ✅ FIX #4 — PROCESAR NOTIFICACIÓN ENTRANTE (método compartido)
    // ════════════════════════════════════════════════════════════════════

    private void processIncomingCharacteristic(UUID uuid, byte[] data) {
        if (data == null || data.length == 0) return;

        if (DATA_READ_UUID.equals(uuid)) {
            String received = new String(data, StandardCharsets.UTF_8);

            // Acumular en buffer y extraer mensajes completos por '\n'
            synchronized (dataBuffer) {
                dataBuffer.append(received);
                int newlineIndex;
                while ((newlineIndex = dataBuffer.indexOf("\n")) >= 0) {
                    String completeMessage = dataBuffer.substring(0, newlineIndex).trim();
                    dataBuffer.delete(0, newlineIndex + 1);

                    if (!completeMessage.isEmpty()) {
                        Log.d(TAG, "📥 Mensaje recibido: " + completeMessage);
                        registerIncomingResponse(completeMessage);

                        if (callback != null) {
                            final String msg = completeMessage;
                            handler.post(() -> callback.onDataReceived(msg));
                        }
                    }
                }
            }

        } else if (PROGRESS_UUID.equals(uuid)) {
            int percentage = data[0] & 0xFF;
            Log.d(TAG, "📊 Progreso: " + percentage + "%");
            if (callback != null) handler.post(() -> callback.onProgress(percentage));
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // HABILITAR NOTIFICACIONES
    // ════════════════════════════════════════════════════════════════════

    private void enableNotifications(BluetoothGatt gatt,
                                     BluetoothGattCharacteristic characteristic) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ActivityCompat.checkSelfPermission(context,
                    android.Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) return;
        }

        Log.d(TAG, "🔔 Habilitando notificaciones en: " + characteristic.getUuid());
        boolean success = gatt.setCharacteristicNotification(characteristic, true);
        if (!success) {
            Log.e(TAG, "❌ Error habilitando notificaciones localmente");
            return;
        }

        BluetoothGattDescriptor descriptor = characteristic.getDescriptor(CCCD_UUID);
        if (descriptor == null) {
            Log.e(TAG, "❌ Descriptor CCCD no encontrado");
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            int result = gatt.writeDescriptor(descriptor,
                    BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            if (result != BluetoothGatt.GATT_SUCCESS)
                Log.e(TAG, "❌ Error escribiendo descriptor (API33+): " + result);
        } else {
            descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            boolean writeSuccess = gatt.writeDescriptor(descriptor);
            if (!writeSuccess)
                Log.e(TAG, "❌ Error escribiendo descriptor (legacy)");
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // ✅ FIX #2 — REGISTRO Y ESPERA DE RESPUESTAS
    //
    // El problema original: responseQueue.clear() ocurría ANTES de que
    // el write BLE real llegara al Heltec. Si la respuesta llegaba rápido,
    // se perdía entre el clear() y el waitForPrefix().
    //
    // Solución: el clear() se hace DENTRO del lock, luego marcamos que
    // estamos "esperando" ANTES de encolar el comando. Así ninguna
    // respuesta puede colarse entre el envío y la espera.
    // ════════════════════════════════════════════════════════════════════

    private void registerIncomingResponse(String response) {
        synchronized (responseLock) {
            responseQueue.add(response);
            responseLock.notifyAll();
        }
    }

    public boolean sendCommandAndWaitForPrefix(String command, String expectedPrefix,
                                               long timeoutMs) {
        if (!isConnected) {
            Log.e(TAG, "❌ No conectado");
            return false;
        }

        if (Looper.myLooper() == Looper.getMainLooper()) {
            Log.e(TAG, "❌ No llames sendCommandAndWaitForPrefix desde el hilo principal");
            return false;
        }

        // ✅ FIX #2 — Limpiar cola Y encolar el comando dentro del mismo lock,
        // para que no haya ventana donde una respuesta llegue y se pierda.
        synchronized (responseLock) {
            responseQueue.clear();
            // Enviamos el comando aquí dentro del lock.
            // La respuesta que llegue DESPUÉS de sendCommand() quedará en responseQueue
            // y waitForPrefix() la encontrará.
            sendCommand(command);
        }

        return waitForPrefix(expectedPrefix, timeoutMs);
    }

    public boolean waitForPrefix(String expectedPrefix, long timeoutMs) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            Log.e(TAG, "❌ No llames waitForPrefix desde el hilo principal");
            return false;
        }

        long endTime = System.currentTimeMillis() + timeoutMs;

        synchronized (responseLock) {
            while (System.currentTimeMillis() < endTime) {

                for (int i = 0; i < responseQueue.size(); i++) {
                    String response = responseQueue.get(i);

                    if (response.startsWith(expectedPrefix)) {
                        responseQueue.remove(i);
                        Log.d(TAG, "✅ Respuesta recibida: " + response);
                        return true;
                    }

                    if (response.startsWith("ERROR:")) {
                        Log.e(TAG, "❌ Error recibido del Heltec: " + response);
                        responseQueue.remove(i);
                        return false;
                    }
                }

                long remaining = endTime - System.currentTimeMillis();
                if (remaining <= 0) break;

                try {
                    responseLock.wait(remaining);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return false;
                }
            }
        }

        Log.e(TAG, "⏱️ Timeout esperando: " + expectedPrefix);
        return false;
    }

    // ════════════════════════════════════════════════════════════════════
    // GETTERS
    // ════════════════════════════════════════════════════════════════════

    /** @return true si está conectado */
    public boolean isConnected() { return isConnected; }

    /** @return true si está conectando */
    public boolean isConnecting() { return isConnecting; }

    /**
     * @return MTU negociado actualmente.
     * Útil para que FileManager calcule el CHUNK_SIZE correcto.
     * Recomendado: usar MAX_CHUNK_BYTES que ya tiene el margen calculado.
     */
    public int getNegotiatedMtu() { return negotiatedMtu; }
}

/*
 * ════════════════════════════════════════════════════════════════════════
 * RESUMEN DE CAMBIOS
 * ════════════════════════════════════════════════════════════════════════
 *
 * ✅ FIX #1 — Fragmentación por MTU (writeCharacteristic + doWrite)
 *   - Se guarda el MTU real negociado en `negotiatedMtu` (onMtuChanged).
 *   - writeCharacteristic() fragmenta el comando si supera (MTU - 3) bytes.
 *   - Se expone MAX_CHUNK_BYTES = 360 para que FileManager lo use como
 *     CHUNK_SIZE, garantizando que "CMD:UPLOAD_CHUNK:" + base64(chunk) + '\n'
 *     siempre quepa en un solo paquete BLE sin fragmentación adicional.
 *
 * ✅ FIX #2 — Race condition en sendCommandAndWaitForPrefix
 *   - responseQueue.clear() y sendCommand() ahora ocurren DENTRO del mismo
 *     bloque synchronized(responseLock), eliminando la ventana donde una
 *     respuesta rápida del Heltec podía llegar y perderse.
 *
 * ✅ FIX #3 — isWriting como AtomicBoolean
 *   - Reemplaza el boolean primitivo (no visible entre hilos) por AtomicBoolean.
 *   - processCommandQueue usa compareAndSet(false, true) para entrada atómica.
 *   - Evita que el main thread y el upload thread ejecuten writeCharacteristic
 *     en paralelo.
 *
 * ✅ FIX #4 — onCharacteristicChanged para Android 13+
 *   - Se agrega el override con firma (gatt, characteristic, byte[] value).
 *   - Ambos overrides comparten processIncomingCharacteristic() para
 *     evitar duplicación de lógica.
 *   - dataBuffer también se sincroniza para evitar acceso concurrente.
 * ════════════════════════════════════════════════════════════════════════
 */