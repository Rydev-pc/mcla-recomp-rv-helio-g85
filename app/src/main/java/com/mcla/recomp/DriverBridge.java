package com.mcla.recomp;

import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;

final class DriverBridge {

    static final String TURNIP = "turnip";
    static final String SYSTEM = "system";
    /** "custom:NAME" loads a Vulkan driver package unpacked in drivers/NAME next to driver.txt. */
    static final String CUSTOM_PREFIX = "custom:";

    private static final String TAG = "MclaDriver";
    private static boolean sInitialized;
    private static String sStatus;

    private DriverBridge() {}

    /** Loads and initializes the Vulkan driver proxy. Returns a JSON status string. */
    static synchronized String initialize(Context context, String mode) {
        if (sInitialized) {
            return sStatus;
        }
        String nativeDir = context.getApplicationInfo().nativeLibraryDir;
        boolean custom = mode != null && mode.startsWith(CUSTOM_PREFIX);
        File driverDir = new File(context.getFilesDir(), "drivers");
        String library = "libvulkan.so";

        if (custom) {
            try {
                String name = mode.substring(CUSTOM_PREFIX.length()).trim();
                driverDir = prepareCustom(context, name);
                library = customLibrary(driverDir);
                Log.i(TAG, "custom driver " + name + " library=" + library);
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "custom driver unusable, falling back to system", e);
                mode = SYSTEM;
                custom = false;
            }
        }

        if (!driverDir.isDirectory() && !driverDir.mkdirs()) {
            Log.w(TAG, "cannot create " + driverDir);
        }

        // Load the app-bundled libvulkan.so proxy (built from driver_proxy.cpp).
        // It is searched before the system namespace, so all Vulkan calls route
        // through our proxy first.
        System.load(new File(nativeDir, "libvulkan.so").getAbsolutePath());

        String status = nativeInit(
            nativeDir,
            driverDir.getAbsolutePath(),
            custom ? library : "",
            custom,
            custom);  // allowSystemFallback for custom drivers

        sInitialized = true;
        sStatus = status;
        boolean ok = false;
        try {
            ok = new org.json.JSONObject(status).optBoolean("ok", false);
        } catch (org.json.JSONException ignored) {}
        if (ok) {
            Log.i(TAG, "driver ready: " + status);
        } else {
            Log.e(TAG, "driver failed: " + status);
        }
        return status;
    }

    /** Null when the initialized driver can run the game, otherwise a player-facing message. */
    static String unusableReason(String status) {
        org.json.JSONObject json;
        try {
            json = new org.json.JSONObject(status);
        } catch (org.json.JSONException e) {
            return "The Vulkan driver could not be initialized (unreadable status).";
        }
        if (!json.optBoolean("ok", false)) {
            return "No usable Vulkan driver was found on this device.\n\n"
                    + json.optString("error", "unknown error");
        }
        org.json.JSONArray devices = json.optJSONArray("devices");
        long best = 0;
        String name = "unknown GPU";
        for (int i = 0; devices != null && i < devices.length(); ++i) {
            org.json.JSONObject device = devices.optJSONObject(i);
            if (device == null) continue;
            long api = device.optLong("api_version", 0);
            if (api >= best) {
                best = api;
                name = device.optString("name", name);
            }
        }
        // Mali-G52 offers Vulkan 1.3 via PanVK (experimental) or Vulkan 1.1 via stock driver.
        // The game needs Vulkan 1.2. If only 1.1 is available, report the issue.
        if (best >= (1L << 22) | (2L << 12)) return null;
        String fallbackFrom = json.optString("fallback_from");
        return "This device's GPU (" + name + ") only offers Vulkan " + (best >> 22) + "."
                + ((best >> 12) & 0x3FF) + " through its own driver, and the game needs Vulkan 1.2."
                + (fallbackFrom.isEmpty()
                        ? ""
                        : "\n\nThe selected driver could not be used: " + fallbackFrom);
    }

    /** Copies an unpacked package from external storage into exec-capable private storage. */
    private static File prepareCustom(Context context, String name) throws IOException {
        if (name.isEmpty() || name.contains("/") || name.contains("..")) {
            throw new IOException("bad custom driver name: " + name);
        }
        File source = new File(context.getExternalFilesDir(null), "drivers/" + name);
        File[] files = source.listFiles();
        if (files == null || files.length == 0) {
            throw new IOException("no custom driver files in " + source);
        }
        File destination = new File(new File(context.getFilesDir(), "drivers"), "custom-" + name);
        deleteRecursively(destination);
        if (!destination.mkdirs()) {
            throw new IOException("cannot create " + destination);
        }
        for (File file : files) {
            if (!file.isFile()) continue;
            try (java.io.InputStream in = new java.io.FileInputStream(file);
                 java.io.OutputStream out = new java.io.FileOutputStream(new File(destination, file.getName()))) {
                copy(in, out);
            }
        }
        return destination;
    }

    /** meta.json libraryName, else the only .so in the package. */
    private static String customLibrary(File dir) throws IOException {
        File meta = new File(dir, "meta.json");
        if (meta.isFile()) {
            try (java.io.InputStream in = new java.io.FileInputStream(meta)) {
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                copy(in, bytes);
                String name = new org.json.JSONObject(bytes.toString("UTF-8")).optString("libraryName", "");
                if (!name.isEmpty() && new File(dir, name).isFile()) return name;
            } catch (org.json.JSONException ignored) {}
        }
        File[] libraries = dir.listFiles((d, n) -> n.endsWith(".so"));
        if (libraries == null || libraries.length != 1) {
            throw new IOException("cannot pick the driver library in " + dir);
        }
        return libraries[0].getName();
    }

    private static void copy(java.io.InputStream in, java.io.OutputStream out) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        int read;
        while ((read = in.read(buffer)) > 0) {
            out.write(buffer, 0, read);
        }
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        if (file.exists()) {
            file.setWritable(true);
            file.delete();
        }
    }

    /** Native method implemented in driver_proxy.cpp (libvulkan.so). */
    static native String nativeInit(String nativeDir, String driverDir, String driverFilename,
                                    boolean custom, boolean allowSystemFallback);
}
