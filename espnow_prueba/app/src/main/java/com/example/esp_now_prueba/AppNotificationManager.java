package com.example.esp_now_prueba;

import android.app.Activity;
import android.graphics.Color;
import android.view.View;
import android.widget.TextView;

import com.google.android.material.snackbar.Snackbar;

/**
 * ════════════════════════════════════════════════════════════════════════
 * AppNotificationManager - Gestor Centralizado de Notificaciones
 * ════════════════════════════════════════════════════════════════════════
 *
 * Reemplaza Toast.makeText() con Snackbars profesionales:
 * - showSuccess: fondo verde, para operaciones exitosas
 * - showError:   fondo rojo,  para errores
 * - showInfo:    fondo azul,  para información
 * - showWarning: fondo naranja, para advertencias
 *
 * Uso básico:
 *   AppNotificationManager.showSuccess(activity, "Archivo subido");
 *   AppNotificationManager.showErrorWithAction(activity, "Error", "Reintentar", v -> retry());
 */
public class AppNotificationManager {

    private static final int DURATION_SHORT = 4000;
    private static final int DURATION_LONG  = 5000;

    private static final int COLOR_SUCCESS = 0xFF388E3C;
    private static final int COLOR_ERROR   = 0xFFD32F2F;
    private static final int COLOR_INFO    = 0xFF1976D2;
    private static final int COLOR_WARNING = 0xFFF57C00;

    // ────────────────────────────────────────────────────────────────────
    // MÉTODOS PÚBLICOS
    // ────────────────────────────────────────────────────────────────────

    public static void showSuccess(Activity activity, String message) {
        show(activity, message, COLOR_SUCCESS, DURATION_SHORT, null, null);
    }

    public static void showError(Activity activity, String message) {
        show(activity, message, COLOR_ERROR, DURATION_LONG, null, null);
    }

    public static void showInfo(Activity activity, String message) {
        show(activity, message, COLOR_INFO, DURATION_SHORT, null, null);
    }

    public static void showWarning(Activity activity, String message) {
        show(activity, message, COLOR_WARNING, DURATION_SHORT, null, null);
    }

    public static void showSuccessWithAction(Activity activity, String message,
                                             String actionText,
                                             View.OnClickListener action) {
        show(activity, message, COLOR_SUCCESS, DURATION_LONG, actionText, action);
    }

    public static void showErrorWithAction(Activity activity, String message,
                                           String actionText,
                                           View.OnClickListener action) {
        show(activity, message, COLOR_ERROR, DURATION_LONG, actionText, action);
    }

    // ────────────────────────────────────────────────────────────────────
    // IMPLEMENTACIÓN INTERNA
    // ────────────────────────────────────────────────────────────────────

    private static void show(Activity activity, String message, int bgColor,
                             int duration, String actionText,
                             View.OnClickListener action) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;

        View anchor = activity.getWindow().getDecorView();
        Snackbar snackbar = Snackbar.make(anchor, message, duration);
        snackbar.getView().setBackgroundColor(bgColor);

        TextView tv = snackbar.getView()
                .findViewById(com.google.android.material.R.id.snackbar_text);
        if (tv != null) {
            tv.setTextColor(Color.WHITE);
            tv.setMaxLines(3);
        }

        if (actionText != null && action != null) {
            snackbar.setAction(actionText, action);
            snackbar.setActionTextColor(Color.WHITE);
        }

        snackbar.show();
    }
}
