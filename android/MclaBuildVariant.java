package org.mcla.recomp;

import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Bundle;

final class MclaBuildVariant {
    private MclaBuildVariant() {}

    static boolean isRedMagicAudioTest(Context context) {
        return "redmagic-audio-test".equals(name(context));
    }

    static boolean isHelioG85(Context context) {
        String variant = name(context);
        return "mali".equals(variant) || "helio-g85".equals(variant);
    }

    private static String name(Context context) {
        try {
            Bundle metadata = context.getPackageManager().getApplicationInfo(
                    context.getPackageName(), PackageManager.GET_META_DATA).metaData;
            return metadata == null ? "" : metadata.getString("org.mcla.recomp.GPU_VARIANT", "");
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }
}
