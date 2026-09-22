package com.stonx.manager;

import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.util.Log;

/**
 * CameraFinder — يجد معرّف الكاميرا الحقيقي بالاعتماد على LENS_FACING.
 * بعض الأجهزة فيها أكثر من كاميرا خلفية أو ترقيم مختلف من جهاز لجهاز.
 * لذلك لا نعتمد على ID ثابت، بل نبحث بالخاصية الفعلية.
 */
final class CameraFinder {

    private static final String TAG = "STONX_CAMF";

    static final class Result {
        final String cameraId;
        final int lensFacing;
        Result(String id, int facing) {
            this.cameraId = id;
            this.lensFacing = facing;
        }
    }

    private CameraFinder() {}

    /**
     * @param manager   CameraManager
     * @param wantFront true للكاميرا الأمامية، false للخلفية
     * @return أول كاميرا مطابقة، أو null إذا لم توجد
     */
    static Result find(CameraManager manager, boolean wantFront) throws CameraAccessException {
        int wanted = wantFront
                ? CameraCharacteristics.LENS_FACING_FRONT
                : CameraCharacteristics.LENS_FACING_BACK;

        for (String id : manager.getCameraIdList()) {
            CameraCharacteristics ch = manager.getCameraCharacteristics(id);
            Integer facing = ch.get(CameraCharacteristics.LENS_FACING);
            if (facing != null && facing == wanted) {
                Log.d(TAG, "found cameraId=" + id + " facing=" + describe(facing));
                return new Result(id, facing);
            }
        }
        Log.d(TAG, "no camera found for facing=" + (wantFront ? "front" : "back"));
        return null;
    }

    private static String describe(int facing) {
        switch (facing) {
            case CameraCharacteristics.LENS_FACING_FRONT: return "FRONT";
            case CameraCharacteristics.LENS_FACING_BACK:  return "BACK";
            default:                                       return "EXTERNAL";
        }
    }
}
