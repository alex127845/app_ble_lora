package com.example.esp_now_prueba;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.util.Base64;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

public class FileManager {

    private static final String TAG = "FileManager";

    // ════════════════════════════════════════════════════════════════════
    // 🔧 CONSTANTES
    // ════════════════════════════════════════════════════════════════════

    // Tamaño de chunk en bytes (debe coincidir con el firmware)
    private static final int CHUNK_SIZE = 200;

    // Timeout para esperar ACK (ms)
    private static final int ACK_TIMEOUT = 2000;

    // Máximo de reintentos por chunk
    private static final int MAX_RETRIES = 3;

    // Carpeta de descargas
    private static final String DOWNLOAD_FOLDER = "HeltecDownloads";

    // ════════════════════════════════════════════════════════════════════
    // 🌐 VARIABLES DE INSTANCIA
    // ════════════════════════════════════════════════════════════════════

    private Context context;

    // Estado de download
    private boolean isDownloading = false;
    private String downloadFileName = "";
    private long downloadFileSize = 0;
    private long downloadBytesReceived = 0;
    private List<byte[]> downloadChunks = new ArrayList<>();
    private int expectedChunks = 0;
    private DownloadCallback downloadCallback;

    // ════════════════════════════════════════════════════════════════════
    // 📞 INTERFACES DE CALLBACKS
    // ════════════════════════════════════════════════════════════════════

    /**
     * Callback para subida de archivos
     */
    public interface UploadCallback {
        /**
         * Progreso de subida
         * @param percentage Porcentaje (0-100)
         */
        void onProgress(int percentage);

        /**
         * Subida completada exitosamente
         */
        void onComplete();

        /**
         * Error durante subida
         * @param error Mensaje de error
         */
        void onError(String error);
    }

    /**
     * Callback para descarga de archivos
     */
    public interface DownloadCallback {
        /**
         * Progreso de descarga
         * @param percentage Porcentaje (0-100)
         */
        void onProgress(int percentage);

        /**
         * Descarga completada exitosamente
         * @param file Archivo descargado
         */
        void onComplete(File file);

        /**
         * Error durante descarga
         * @param error Mensaje de error
         */
        void onError(String error);
    }

    // ════════════════════════════════════════════════════════════════════
    // 🏗️ CONSTRUCTOR
    // ════════════════════════════════════════════════════════════════════

    /**
     * Constructor del FileManager
     *
     * @param context Contexto de la aplicación
     */
    public FileManager(Context context) {
        this.context = context;
        Log.d(TAG, "📂 FileManager inicializado");
    }

    // ════════════════════════════════════════════════════════════════════
    // 📤 UPLOAD - SUBIDA DE ARCHIVOS
    // ════════════════════════════════════════════════════════════════════

    /**
     * Subir archivo al Heltec dividiéndolo en chunks
     *
     * @param inputStream Stream del archivo a subir
     * @param fileSize Tamaño total del archivo
     * @param bleManager Manager BLE para enviar chunks
     * @param callback Callback para notificar progreso
     */
    public void uploadFileInChunks(InputStream inputStream,
                                   long fileSize,
                                   BLEManager bleManager,
                                   UploadCallback callback) {

        Log.d(TAG, "📤 Iniciando upload en chunks");
        Log.d(TAG, "   Tamaño: " + fileSize + " bytes");

        // Calcular número de chunks
        int totalChunks = (int) Math.ceil((double) fileSize / CHUNK_SIZE);
        Log.d(TAG, "   Chunks totales: " + totalChunks);

        try {
            byte[] buffer = new byte[CHUNK_SIZE];
            int chunkNumber = 0;
            int bytesRead;
            long totalBytesRead = 0;

            // Leer y enviar chunks
            while ((bytesRead = inputStream.read(buffer)) > 0) {

                // Crear chunk del tamaño exacto leído
                byte[] chunk = new byte[bytesRead];
                System.arraycopy(buffer, 0, chunk, 0, bytesRead);

                // Codificar a Base64
                String base64Chunk = Base64.encodeToString(chunk, Base64.NO_WRAP);

                // Crear comando
                String command = "CMD:UPLOAD_CHUNK:" + base64Chunk;

                // Enviar comando
                bleManager.sendCommand(command);

                chunkNumber++;
                totalBytesRead += bytesRead;

                // Calcular progreso
                int percentage = (int) ((totalBytesRead * 100) / fileSize);

                // Notificar progreso cada 10% o en el último chunk
                if (percentage % 10 == 0 || chunkNumber >= totalChunks) {
                    Log.d(TAG, "📦 Chunk " + chunkNumber + "/" + totalChunks +
                            " (" + percentage + "%) - " + bytesRead + " bytes");

                    if (callback != null) {
                        callback.onProgress(percentage);
                    }
                }

                // Pequeña pausa entre chunks para no saturar
                Thread.sleep(100);
            }

            Log.d(TAG, "✅ Upload completado: " + chunkNumber + " chunks enviados");

            // Esperar confirmación final del Heltec
            Thread.sleep(500);

            if (callback != null) {
                callback.onComplete();
            }

        } catch (IOException e) {
            Log.e(TAG, "❌ Error leyendo archivo: " + e.getMessage());
            if (callback != null) {
                callback.onError("Error leyendo archivo: " + e.getMessage());
            }
        } catch (InterruptedException e) {
            Log.e(TAG, "❌ Upload interrumpido: " + e.getMessage());
            if (callback != null) {
                callback.onError("Upload interrumpido");
            }
        } catch (Exception e) {
            Log.e(TAG, "❌ Error en upload: " + e.getMessage());
            if (callback != null) {
                callback.onError("Error: " + e.getMessage());
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📥 DOWNLOAD - DESCARGA DE ARCHIVOS
    // ════════════════════════════════════════════════════════════════════

    /**
     * Iniciar descarga de archivo del Heltec
     *
     * @param fileName Nombre del archivo
     * @param fileSize Tamaño del archivo
     */
    public void startDownload(String fileName, long fileSize) {
        Log.d(TAG, "📥 Iniciando download: " + fileName + " (" + fileSize + " bytes)");

        isDownloading = true;
        downloadFileName = fileName;
        downloadFileSize = fileSize;
        downloadBytesReceived = 0;
        downloadChunks.clear();

        // Calcular chunks esperados
        expectedChunks = (int) Math.ceil((double) fileSize / CHUNK_SIZE);
        Log.d(TAG, "   Chunks esperados: " + expectedChunks);
    }

    /**
     * Recibir chunk de descarga
     *
     * @param chunkNumber Número de chunk
     * @param base64Data Datos en Base64
     * @param callback Callback para notificar progreso
     */
    public void receiveChunk(int chunkNumber, String base64Data, DownloadCallback callback) {
        if (!isDownloading) {
            Log.w(TAG, "⚠️ Chunk recibido pero no hay download activo");
            return;
        }

        try {
            // Decodificar Base64
            byte[] chunkData = Base64.decode(base64Data, Base64.NO_WRAP);

            // Guardar chunk
            downloadChunks.add(chunkData);
            downloadBytesReceived += chunkData.length;

            // Calcular progreso
            int percentage = (int) ((downloadBytesReceived * 100) / downloadFileSize);

            // Log cada 10 chunks o en el último
            if (downloadChunks.size() % 10 == 0 || downloadChunks.size() >= expectedChunks) {
                Log.d(TAG, "📦 Chunk " + downloadChunks.size() + "/" + expectedChunks +
                        " (" + percentage + "%) - " + chunkData.length + " bytes");
            }

            // Guardar callback para uso posterior
            this.downloadCallback = callback;

            // Notificar progreso
            if (callback != null) {
                callback.onProgress(percentage);
            }

        } catch (IllegalArgumentException e) {
            Log.e(TAG, "❌ Error decodificando Base64: " + e.getMessage());
            if (callback != null) {
                callback.onError("Error decodificando datos");
            }
        }
    }

    /**
     * Completar descarga y guardar archivo
     */
    public void completeDownload() {
        if (!isDownloading) {
            Log.w(TAG, "⚠️ completeDownload llamado pero no hay download activo");
            return;
        }

        Log.d(TAG, "🏁 Completando download...");
        Log.d(TAG, "   Chunks recibidos: " + downloadChunks.size());
        Log.d(TAG, "   Bytes recibidos: " + downloadBytesReceived);

        try {
            // Crear archivo de salida
            File outputFile = createDownloadFile(downloadFileName);

            if (outputFile == null) {
                throw new IOException("No se pudo crear archivo de salida");
            }

            // Escribir todos los chunks
            FileOutputStream fos = new FileOutputStream(outputFile);

            for (byte[] chunk : downloadChunks) {
                fos.write(chunk);
            }

            fos.flush();
            fos.close();

            // Verificar tamaño
            long actualSize = outputFile.length();

            Log.d(TAG, "✅ Archivo guardado: " + outputFile.getAbsolutePath());
            Log.d(TAG, "   Tamaño esperado: " + downloadFileSize);
            Log.d(TAG, "   Tamaño real: " + actualSize);

            if (actualSize != downloadFileSize) {
                Log.w(TAG, "⚠️ Advertencia: Tamaño no coincide");
            }

            // Limpiar estado
            isDownloading = false;
            downloadChunks.clear();

            // Notificar completado
            if (downloadCallback != null) {
                downloadCallback.onComplete(outputFile);
            }

        } catch (IOException e) {
            Log.e(TAG, "❌ Error guardando archivo: " + e.getMessage());

            isDownloading = false;
            downloadChunks.clear();

            if (downloadCallback != null) {
                downloadCallback.onError("Error guardando archivo: " + e.getMessage());
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📁 UTILIDADES DE ARCHIVOS
    // ════════════════════════════════════════════════════════════════════

    /**
     * Obtener nombre de archivo desde URI
     *
     * @param uri URI del archivo
     * @return Nombre del archivo
     */
    public String getFileName(Uri uri) {
        String result = null;

        if (uri.getScheme() != null && uri.getScheme().equals("content")) {
            // URI de content provider
            try (Cursor cursor = context.getContentResolver().query(
                    uri, null, null, null, null)) {

                if (cursor != null && cursor.moveToFirst()) {
                    int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (nameIndex >= 0) {
                        result = cursor.getString(nameIndex);
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "Error obteniendo nombre: " + e.getMessage());
            }
        }

        if (result == null) {
            // Obtener desde path
            result = uri.getPath();
            if (result != null) {
                int cut = result.lastIndexOf('/');
                if (cut != -1) {
                    result = result.substring(cut + 1);
                }
            }
        }

        // Si aún no hay nombre, generar uno
        if (result == null || result.isEmpty()) {
            result = "archivo_" + System.currentTimeMillis();
        }

        Log.d(TAG, "📄 Nombre de archivo: " + result);
        return result;
    }

    /**
     * Obtener tamaño de archivo desde URI
     *
     * @param uri URI del archivo
     * @return Tamaño en bytes
     */
    public long getFileSize(Uri uri) {
        long size = 0;

        if (uri.getScheme() != null && uri.getScheme().equals("content")) {
            // URI de content provider
            try (Cursor cursor = context.getContentResolver().query(
                    uri, null, null, null, null)) {

                if (cursor != null && cursor.moveToFirst()) {
                    int sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE);
                    if (sizeIndex >= 0) {
                        size = cursor.getLong(sizeIndex);
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "Error obteniendo tamaño: " + e.getMessage());
            }
        }

        // Si no se pudo obtener, intentar con inputStream
        if (size == 0) {
            try {
                InputStream inputStream = context.getContentResolver().openInputStream(uri);
                if (inputStream != null) {
                    size = inputStream.available();
                    inputStream.close();
                }
            } catch (IOException e) {
                Log.e(TAG, "Error calculando tamaño: " + e.getMessage());
            }
        }

        Log.d(TAG, "📊 Tamaño de archivo: " + size + " bytes");
        return size;
    }

    /**
     * Crear archivo de descarga en carpeta de Descargas
     *
     * @param fileName Nombre del archivo
     * @return Archivo creado o null si error
     */
    private File createDownloadFile(String fileName) {
        try {
            // Obtener carpeta de Descargas
            File downloadsDir = Environment.getExternalStoragePublicDirectory(
                    Environment.DIRECTORY_DOWNLOADS);

            // Crear subcarpeta HeltecDownloads
            File heltecDir = new File(downloadsDir, DOWNLOAD_FOLDER);

            if (!heltecDir.exists()) {
                boolean created = heltecDir.mkdirs();
                if (created) {
                    Log.d(TAG, "📁 Carpeta creada: " + heltecDir.getAbsolutePath());
                }
            }

            // Crear archivo con timestamp si ya existe
            File outputFile = new File(heltecDir, fileName);

            if (outputFile.exists()) {
                // Agregar timestamp al nombre
                String timestamp = new SimpleDateFormat("yyyyMMdd_HHmmss",
                        Locale.getDefault()).format(new Date());

                String nameWithoutExt = fileName;
                String extension = "";

                int dotIndex = fileName.lastIndexOf('.');
                if (dotIndex > 0) {
                    nameWithoutExt = fileName.substring(0, dotIndex);
                    extension = fileName.substring(dotIndex);
                }

                String newFileName = nameWithoutExt + "_" + timestamp + extension;
                outputFile = new File(heltecDir, newFileName);

                Log.d(TAG, "⚠️ Archivo existe, usando: " + newFileName);
            }

            // Crear archivo vacío
            boolean created = outputFile.createNewFile();

            if (created) {
                Log.d(TAG, "✅ Archivo creado: " + outputFile.getAbsolutePath());
                return outputFile;
            } else {
                Log.e(TAG, "❌ No se pudo crear archivo");
                return null;
            }

        } catch (IOException e) {
            Log.e(TAG, "❌ Error creando archivo: " + e.getMessage());
            return null;
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 🔐 UTILIDADES DE CODIFICACIÓN
    // ════════════════════════════════════════════════════════════════════

    /**
     * Codificar bytes a Base64
     *
     * @param data Datos a codificar
     * @return String en Base64
     */
    public static String encodeBase64(byte[] data) {
        return Base64.encodeToString(data, Base64.NO_WRAP);
    }

    /**
     * Decodificar Base64 a bytes
     *
     * @param base64 String en Base64
     * @return Bytes decodificados
     */
    public static byte[] decodeBase64(String base64) {
        return Base64.decode(base64, Base64.NO_WRAP);
    }

    /**
     * Validar si un string es Base64 válido
     *
     * @param base64 String a validar
     * @return true si es válido
     */
    public static boolean isValidBase64(String base64) {
        try {
            byte[] decoded = Base64.decode(base64, Base64.NO_WRAP);
            String encoded = Base64.encodeToString(decoded, Base64.NO_WRAP);
            return encoded.equals(base64);
        } catch (IllegalArgumentException e) {
            return false;
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 📊 GETTERS
    // ════════════════════════════════════════════════════════════════════

    /**
     * @return true si hay una descarga en progreso
     */
    public boolean isDownloading() {
        return isDownloading;
    }

    /**
     * @return Nombre del archivo que se está descargando
     */
    public String getDownloadFileName() {
        return downloadFileName;
    }

    /**
     * @return Porcentaje de descarga (0-100)
     */
    public int getDownloadProgress() {
        if (downloadFileSize == 0) return 0;
        return (int) ((downloadBytesReceived * 100) / downloadFileSize);
    }

    /**
     * Cancelar descarga en progreso
     */
    public void cancelDownload() {
        if (isDownloading) {
            Log.w(TAG, "⚠️ Descarga cancelada por usuario");

            isDownloading = false;
            downloadChunks.clear();
            downloadBytesReceived = 0;

            if (downloadCallback != null) {
                downloadCallback.onError("Descarga cancelada");
            }
        }
    }
}

