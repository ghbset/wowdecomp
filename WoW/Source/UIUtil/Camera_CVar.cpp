#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/UIUtil/ActionCam.h"

// STATIC: 0x00C24970
CVar* s_cvCameraSmoothTimeMax;

// STATIC: 0x00C24974
CVar* s_cvCameraSmoothTimeMin;

// STATIC: 0x00C24978
CVar* s_cvCameraYawSmoothMax;

// STATIC: 0x00C2497C
CVar* s_cvCameraYawSmoothMin;

// STATIC: 0x00C24980
CVar* s_cvCameraPitchSmoothMax;

// STATIC: 0x00C24984
CVar* s_cvCameraPitchSmoothMin;

// STATIC: 0x00C24988
CVar* s_cvCameraDistanceMaxFactor;

// STATIC: 0x00C2498C
CVar* s_cvCameraDistanceMax;

// STATIC: 0x00C24990
CVar* s_cvCameraSubmergeFinalPitch;

// STATIC: 0x00C24994
CVar* s_cvCameraSurfaceFinalPitch;

// STATIC: 0x00C24998
CVar* s_cvCameraSubmergePitch;

// STATIC: 0x00C2499C
CVar* s_cvCameraSurfacePitch;

// STATIC: 0x00C249A0
CVar* s_cvCameraDive;

// STATIC: 0x00C249A4
CVar* s_cvCameraPivotDYMin;

// STATIC: 0x00C249A8
CVar* s_cvCameraPivotDXMax;

// STATIC: 0x00C249AC
CVar* s_cvCameraPivot;

// STATIC: 0x00C249B0
CVar* s_cvCameraHeightIgnoreStandState;

// STATIC: 0x00C249B4
CVar* s_cameraWaterCollision;

// STATIC: 0x00C249B8
CVar* s_cvCameraTerrainTiltTimeMax;

// STATIC: 0x00C249BC
CVar* s_cvCameraTerrainTiltTimeMin;

// STATIC: 0x00C249C0
CVar* s_cvCameraTerrainTiltState[5][10][3];

// STATIC: 0x00C24C18
CVar* s_cvCameraTerrainTilt;

// STATIC: 0x00C24C1C
CVar* s_cvCameraSmoothStyle;

// STATIC: 0x00C24C20
CVar* s_cvCameraSmoothViewData[5][3][2];

// STATIC: 0x00C24C98
CVar* s_cvCameraSmoothState[5][7][2];

// STATIC: 0x00C24DB0
CVar* s_cvCameraSmoothYaw;

// STATIC: 0x00C24DB4
CVar* s_cvCameraSmoothTrackingStyle;

// STATIC: 0x00C24DB8
CVar* s_cvCameraSmoothPitch;

// STATIC: 0x00C24DBC
CVar* s_cameraSmooth;

// STATIC: 0x00C24DC0
CVar* s_cvCameraCustomViewSmoothing;

// STATIC: 0x00C24DC4
CVar* s_cvCameraView;

// STATIC: 0x00C24DC8
CVar* s_cvCameraViewData[8][3];

// STATIC: 0x00C24E28
CVar* s_cvCameraViewBlendStyle;

// STATIC: 0x00C24E2C
CVar* s_cvCameraFlyingMountHeightSmoothSpeed;

// STATIC: 0x00C24E30
CVar* s_cvCameraYawSmoothSpeed;

// STATIC: 0x00C24E34
CVar* s_cvCameraTargetSmoothSpeed;

// STATIC: 0x00C24E38
CVar* s_cvCameraPitchSmoothSpeed;

// STATIC: 0x00C24E3C
CVar* s_cvCameraHeightSmoothSpeed;

// STATIC: 0x00C24E40
CVar* s_cvCameraGroundSmoothSpeed;

// STATIC: 0x00C24E44
CVar* s_cvCameraFoVSmoothSpeed;

// STATIC: 0x00C24E48
CVar* s_cvCameraDistanceSmoothSpeed;

// STATIC: 0x00C24E4C
CVar* s_cvCameraBobbingSmoothSpeed;

// STATIC: 0x00C24E50
CVar* s_cvCameraYawMoveSpeed;

// STATIC: 0x00C24E54
CVar* s_cvCameraPitchMoveSpeed;

// STATIC: 0x00C24E58
CVar* s_cvCameraDistanceMoveSpeed;

// STATIC: 0x00C24E5C
float s_cameraBobFrequency;

// STATIC: 0x00C24E60
float s_cameraBobVerticalAmplitude;

// STATIC: 0x00C24E64
float s_cameraBobHorizontalAmplitude;

// STATIC: 0x00C24E68
CVar* s_cvCameraBobbing;

// STATIC: 0x00C24E6C
CVar* s_cvMouseInvertPitch;

// STATIC: 0x00C24E70
CVar* s_cvMouseInvertYaw;

// STATIC: 0x00C24E74
CVar* s_cvCameraSavedPitch;

// STATIC: 0x00C24E78
CVar* s_cvCameraSavedVehicleDistance;

// STATIC: 0x00C24E7C
CVar* s_cvCameraSavedDistance;

// STATIC: 0x00AD1B54
static const char* s_cameraViewNames[8] = { "", "A", "B", "C", "D", "E", "Com", "Barber Shop" };

// STATIC: 0x00AD1B74
static const char* s_cameraViewDataNames[3] = { "Distance", "Pitch", "Yaw" };

// STATIC: 0x00AD1B8C
static const char* s_defaultCameraViewBlendStyle = "1";

// STATIC: 0x00AD1B90
static const char* s_defaultCameraView = "2";

// STATIC: 0x00AD1B94
static const char* s_cameraSmoothStyleNames[5] = { "Never", "Smart", "Always", "Spline", "Smarter" };

// STATIC: 0x00AD1C08
static const char* s_cameraSmoothParamNames[2] = { "Delay", "Factor" };

// STATIC: 0x00AD1C10
static const char* s_cameraSmoothStateNames[7] = { "Idle", "Stop", "Track", "Move", "Strafe", "Turn", "Fear" };

// STATIC: 0x00AD1C2C
static const char* s_defaultCameraSmoothStyle = "4";

// STATIC: 0x00AD1C30
static const char* s_cameraSmoothStateDefaults[5][7][2] = {
    {
        { "0.0", "0.0" },
        { "0.0", "0.0" },
        { "0.0", "0.0" },
        { "0.0", "0.0" },
        { "0.0", "0.0" },
        { "0.0", "0.0" },
        { "0.0", "0.0" },
    },
    {
        { "0.0", "0.0" },
        { "0.0", "0.0" },
        { "0.4", "10.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.4", "10.0" },
    },
    {
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
    },
    {
        { "0.0", "4.0" },
        { "0.0", "4.0" },
        { "0.0", "4.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.0", "4.0" },
    },
    {
        { "0.0", "0.0" },
        { "0.0", "0.0" },
        { "0.4", "10.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
        { "0.4", "10.0" },
    },
};

// STATIC: 0x00AD1D48
static const char* s_cameraSmoothViewDataDefaults[5][3][2] = {
    {
        { "0.0", "0.0" },
        { "0.0", "0.0" },
        { "0.0", "0.0" },
    },
    {
        { "0.0", "0.0" },
        { "0.0", "0.0" },
        { "0.0", "1.0" },
    },
    {
        { "0.0", "0.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
    },
    {
        { "0.0", "0.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
    },
    {
        { "0.0", "0.0" },
        { "0.0", "1.0" },
        { "0.0", "1.0" },
    },
};

// STATIC: 0x00AD1DC0
static const char* s_cameraTerrainTiltStateNames[10] = { "Fall", "Fear", "Idle", "Jump", "Move", "Strafe", "Swim", "Taxi", "Track", "Turn" };

// STATIC: 0x00AD1DE8
static const char* s_cameraTerrainTiltParamNames[3] = { "Absorb", "Delay", "Factor" };

// STATIC: 0x00AD1DF8
static const char* s_cameraTerrainTiltStateDefaults[5][10][3] = {
    {
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
    },
    {
        { "1.0", "0.0", "0.75" },
        { "1.0", "0.0", "1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "1.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
        { "0.0", "0.0", "1.0" },
        { "0.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
    },
    {
        { "1.0", "0.0", "0.75" },
        { "1.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
        { "0.0", "0.0", "-1.0" },
        { "1.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
        { "0.0", "0.0", "1.0" },
        { "0.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
    },
    {
        { "1.0", "0.0", "0.75" },
        { "1.0", "0.0", "1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "1.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
        { "0.0", "0.0", "1.0" },
        { "0.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
    },
    {
        { "1.0", "0.0", "0.75" },
        { "1.0", "0.0", "1.0" },
        { "0.0", "0.0", "-1.0" },
        { "0.0", "0.0", "-1.0" },
        { "1.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
        { "0.0", "0.0", "1.0" },
        { "0.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
        { "1.0", "0.0", "1.0" },
    },
};

// VERIFY: 53117c38 auto
// OFFSET: 0x005FD630
static bool ValidateCameraView(CVar* var, const char* oldValue, const char* value, void* arg) {
    float view = SStrToFloat(value);

    if (view >= 0.0f && view <= 7.0f) {
        return true;
    }

    ConsoleWriteA("Value out of range (%f - %f)\n", DEFAULT_COLOR, 0.0, 7.0);
    return false;
}

// VERIFY: 35928b19 auto
// OFFSET: 0x005FD680
static bool ValidateCameraDistance(CVar* var, const char* oldValue, const char* value, void* arg) {
    float distance = SStrToFloat(value);

    if (distance >= 0.0f && distance <= 50.0f) {
        return true;
    }

    ConsoleWriteA("Value out of range (%f - %f)\n", DEFAULT_COLOR, 0.0, 50.0);
    return false;
}

// VERIFY: c58861df auto
// OFFSET: 0x005FD6D0
static bool ValidateCameraPitch(CVar* var, const char* oldValue, const char* value, void* arg) {
    float minPitch = MIN_PITCH_ANGLE * 57.29578f;
    float maxPitch = MAX_PITCH_ANGLE * 57.29578f;
    float pitch = SStrToFloat(value);

    if (pitch >= minPitch && pitch <= maxPitch) {
        return true;
    }

    ConsoleWriteA("Value out of range (%f - %f)\n", DEFAULT_COLOR, minPitch, maxPitch);
    return false;
}

// VERIFY: 72713edf auto
// OFFSET: 0x005FD750
static bool ValidateCameraTime(CVar* var, const char* oldValue, const char* value, void* arg) {
    float time = SStrToFloat(value);

    if (time >= 0.001f && time <= 300.0f) {
        return true;
    }

    ConsoleWriteA("Value out of range (%f - %f)\n", DEFAULT_COLOR, 0.001f, 300.0f);
    return false;
}

// VERIFY: 513f85d3 auto
// OFFSET: 0x005FD7B0
static bool ValidateCameraYaw(CVar* var, const char* oldValue, const char* value, void* arg) {
    float yaw = SStrToFloat(value);

    if (yaw >= 0.0f && yaw <= 360.0f) {
        return true;
    }

    ConsoleWriteA("Value out of range (%f - %f)\n", DEFAULT_COLOR, 0.0, 360.00001422012247);
    return false;
}

// VERIFY: a191501b auto
// OFFSET: 0x005FD800
static bool ValidateCameraAngularSpeed(CVar* var, const char* oldValue, const char* value, void* arg) {
    float speed = SStrToFloat(value);

    if (speed >= 0.1f && speed <= 360.0f) {
        return true;
    }

    ConsoleWriteA("Value out of range (%f - %f)\n", DEFAULT_COLOR, 0.10000000039264378, 360.00001422012247);
    return false;
}

// VERIFY: 73659631 auto
// OFFSET: 0x005FD860
static bool ValidateCameraLinearSpeed(CVar* var, const char* oldValue, const char* value, void* arg) {
    float speed = SStrToFloat(value);

    if (speed >= 0.0027777778f && speed <= 50.0f) {
        return true;
    }

    ConsoleWriteA("Value out of range (%f - %f)\n", DEFAULT_COLOR, 0.0027777778f, 50.0f);
    return false;
}

// VERIFY: 09a53f38 auto
// OFFSET: 0x005FD8C0
static bool ValidateCameraSmoothStyle(CVar* var, const char* oldValue, const char* value, void* arg) {
    float style = SStrToFloat(value);

    if (style >= 0.0f && style <= 5.0f) {
        return true;
    }

    ConsoleWriteA("Value out of range (%f - %f)\n", DEFAULT_COLOR, 0.0, 5.0);
    return false;
}

// STATIC: 0x00AD2054
static bool (*s_cameraViewDataValidators[3])(CVar*, const char*, const char*, void*) = { ValidateCameraDistance, ValidateCameraPitch, ValidateCameraYaw };

// VERIFY: f67b59e2 manual
// OFFSET: 0x005FD910
void CameraRegisterCVars() {
    s_cvCameraSavedDistance = CVar::Register("cameraSavedDistance", nullptr, 0x20, s_cameraViewDefaults[SStrToInt(s_defaultCameraView)].m_values[0], nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSavedVehicleDistance = CVar::Register("cameraSavedVehicleDistance", nullptr, 0x20, "-1.0", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSavedPitch = CVar::Register("cameraSavedPitch", nullptr, 0x20, s_cameraViewDefaults[SStrToInt(s_defaultCameraView)].m_values[1], nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvMouseInvertYaw = CVar::Register("mouseInvertYaw", nullptr, 0x10, "0", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvMouseInvertPitch = CVar::Register("mouseInvertPitch", nullptr, 0x10, "0", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraBobbing = CVar::Register("cameraBobbing", nullptr, 0x0, "0", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cameraBobHorizontalAmplitude = 2.0f;
    s_cameraBobVerticalAmplitude = 2.0f;
    s_cameraBobFrequency = 0.8f;
    s_cvCameraDistanceMoveSpeed = CVar::Register("cameraDistanceMoveSpeed", nullptr, 0x10, "8.33", ValidateCameraLinearSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraPitchMoveSpeed = CVar::Register("cameraPitchMoveSpeed", nullptr, 0x10, "90", ValidateCameraAngularSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraYawMoveSpeed = CVar::Register("cameraYawMoveSpeed", nullptr, 0x10, "180", ValidateCameraAngularSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraBobbingSmoothSpeed = CVar::Register("cameraBobbingSmoothSpeed", nullptr, 0x10, "0.8", ValidateCameraLinearSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraFoVSmoothSpeed = CVar::Register("cameraFoVSmoothSpeed", nullptr, 0x10, "0.5", ValidateCameraAngularSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraDistanceSmoothSpeed = CVar::Register("cameraDistanceSmoothSpeed", nullptr, 0x10, "8.33", ValidateCameraLinearSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraGroundSmoothSpeed = CVar::Register("cameraGroundSmoothSpeed", nullptr, 0x10, "7.5", ValidateCameraAngularSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraHeightSmoothSpeed = CVar::Register("cameraHeightSmoothSpeed", nullptr, 0x10, "1.2", ValidateCameraLinearSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraPitchSmoothSpeed = CVar::Register("cameraPitchSmoothSpeed", nullptr, 0x10, "45", ValidateCameraAngularSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraTargetSmoothSpeed = CVar::Register("cameraTargetSmoothSpeed", nullptr, 0x10, "90", ValidateCameraAngularSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraYawSmoothSpeed = CVar::Register("cameraYawSmoothSpeed", nullptr, 0x10, "180", ValidateCameraAngularSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraFlyingMountHeightSmoothSpeed = CVar::Register("cameraFlyingMountHeightSmoothSpeed", nullptr, 0x10, "2.0", ValidateCameraLinearSpeed, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraViewBlendStyle = CVar::Register("cameraViewBlendStyle", nullptr, 0x10, s_defaultCameraViewBlendStyle, nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraView = CVar::Register("cameraView", nullptr, 0x10, s_defaultCameraView, ValidateCameraView, CATEGORY_DEFAULT, false, nullptr, false);

    char name[64];

    for (uint32_t view = 0; view < 8; view++) {
        for (uint32_t i = 0; i < 3; i++) {
            name[0] = '\0';
            SStrPack(name, "camera", sizeof(name));
            SStrPack(name, s_cameraViewDataNames[i], sizeof(name));
            SStrPack(name, s_cameraViewNames[view], sizeof(name));
            s_cvCameraViewData[view][i] = CVar::Register(name, nullptr, 0x50, s_cameraViewDefaults[view].m_values[i], s_cameraViewDataValidators[i], CATEGORY_DEFAULT, false, nullptr, false);
        }
    }

    s_cameraSmooth = CVar::Register("camerasmooth", nullptr, 0x10, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSmoothPitch = CVar::Register("cameraSmoothPitch", nullptr, 0x10, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSmoothYaw = CVar::Register("cameraSmoothYaw", nullptr, 0x10, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSmoothStyle = CVar::Register("cameraSmoothStyle", nullptr, 0x10, s_defaultCameraSmoothStyle, ValidateCameraSmoothStyle, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSmoothTrackingStyle = CVar::Register("cameraSmoothTrackingStyle", nullptr, 0x10, s_defaultCameraSmoothStyle, ValidateCameraSmoothStyle, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraCustomViewSmoothing = CVar::Register("cameraCustomViewSmoothing", nullptr, 0x10, "0", nullptr, CATEGORY_DEFAULT, false, nullptr, false);

    for (uint32_t style = 0; style < 5; style++) {
        for (uint32_t state = 0; state < 7; state++) {
            for (uint32_t i = 0; i < 2; i++) {
                name[0] = '\0';
                SStrPack(name, "cameraSmooth", sizeof(name));
                SStrPack(name, s_cameraSmoothStyleNames[style], sizeof(name));
                SStrPack(name, s_cameraSmoothStateNames[state], sizeof(name));
                SStrPack(name, s_cameraSmoothParamNames[i], sizeof(name));
                s_cvCameraSmoothState[style][state][i] = CVar::Register(name, nullptr, 0x10, s_cameraSmoothStateDefaults[style][state][i], nullptr, CATEGORY_DEFAULT, false, nullptr, false);
            }
        }

        for (uint32_t data = 0; data < 3; data++) {
            for (uint32_t i = 0; i < 2; i++) {
                name[0] = '\0';
                SStrPack(name, "cameraSmoothViewData", sizeof(name));
                SStrPack(name, s_cameraSmoothStyleNames[style], sizeof(name));
                SStrPack(name, s_cameraViewDataNames[data], sizeof(name));
                SStrPack(name, s_cameraSmoothParamNames[i], sizeof(name));
                s_cvCameraSmoothViewData[style][data][i] = CVar::Register(name, nullptr, 0x10, s_cameraSmoothViewDataDefaults[style][data][i], nullptr, CATEGORY_DEFAULT, false, nullptr, false);
            }
        }

        for (uint32_t state = 0; state < 10; state++) {
            for (uint32_t i = 0; i < 3; i++) {
                name[0] = '\0';
                SStrPack(name, "cameraTerrainTilt", sizeof(name));
                SStrPack(name, s_cameraSmoothStyleNames[style], sizeof(name));
                SStrPack(name, s_cameraTerrainTiltStateNames[state], sizeof(name));
                SStrPack(name, s_cameraTerrainTiltParamNames[i], sizeof(name));
                s_cvCameraTerrainTiltState[style][state][i] = CVar::Register(name, nullptr, 0x10, s_cameraTerrainTiltStateDefaults[style][state][i], nullptr, CATEGORY_DEFAULT, false, nullptr, false);
            }
        }
    }

    s_cvCameraTerrainTilt = CVar::Register("cameraTerrainTilt", nullptr, 0x10, "0", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraTerrainTiltTimeMin = CVar::Register("cameraTerrainTiltTimeMin", nullptr, 0x10, "3.0", ValidateCameraTime, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraTerrainTiltTimeMax = CVar::Register("cameraTerrainTiltTimeMax", nullptr, 0x10, "10.0", ValidateCameraTime, CATEGORY_DEFAULT, false, nullptr, false);
    s_cameraWaterCollision = CVar::Register("cameraWaterCollision", nullptr, 0x10, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraHeightIgnoreStandState = CVar::Register("cameraHeightIgnoreStandState", nullptr, 0x10, "0", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraPivot = CVar::Register("cameraPivot", nullptr, 0x10, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraPivotDXMax = CVar::Register("cameraPivotDXMax", nullptr, 0x10, "0.05", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraPivotDYMin = CVar::Register("cameraPivotDYMin", nullptr, 0x10, "0.00", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraDive = CVar::Register("cameraDive", nullptr, 0x10, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSurfacePitch = CVar::Register("cameraSurfacePitch", nullptr, 0x10, "0.0", ValidateCameraPitch, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSubmergePitch = CVar::Register("cameraSubmergePitch", nullptr, 0x10, "18.0", ValidateCameraPitch, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSurfaceFinalPitch = CVar::Register("cameraSurfaceFinalPitch", nullptr, 0x10, "5.0", ValidateCameraPitch, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSubmergeFinalPitch = CVar::Register("cameraSubmergeFinalPitch", nullptr, 0x10, "5.0", ValidateCameraPitch, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraDistanceMax = CVar::Register("cameraDistanceMax", nullptr, 0x10, "15.0", ValidateCameraDistance, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraDistanceMaxFactor = CVar::Register("cameraDistanceMaxFactor", nullptr, 0x10, "1.0", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraPitchSmoothMin = CVar::Register("cameraPitchSmoothMin", nullptr, 0x10, "0.0", ValidateCameraPitch, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraPitchSmoothMax = CVar::Register("cameraPitchSmoothMax", nullptr, 0x10, "30.0", ValidateCameraPitch, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraYawSmoothMin = CVar::Register("cameraYawSmoothMin", nullptr, 0x10, "0.0", ValidateCameraYaw, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraYawSmoothMax = CVar::Register("cameraYawSmoothMax", nullptr, 0x10, "0.0", ValidateCameraYaw, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSmoothTimeMin = CVar::Register("cameraSmoothTimeMin", nullptr, 0x10, "0.1", ValidateCameraTime, CATEGORY_DEFAULT, false, nullptr, false);
    s_cvCameraSmoothTimeMax = CVar::Register("cameraSmoothTimeMax", nullptr, 0x10, "2.0", ValidateCameraTime, CATEGORY_DEFAULT, false, nullptr, false);
    ActionCam::RegisterCVars();   // not stock: test_camera* + wxl_camera* (ActionCam.cpp)
}
