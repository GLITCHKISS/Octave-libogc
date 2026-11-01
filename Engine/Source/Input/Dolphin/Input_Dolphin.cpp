#if PLATFORM_DOLPHIN

#include "Input/Input.h"
#include "Input/InputUtils.h"

#include "Engine.h"
#include "Log.h"
#include "Maths.h"
#include "System/System.h"

#if PLATFORM_WII
#include <wiiuse/wpad.h>
#endif

namespace
{
    // RUMBLE. The GameCube pad's motor (and the Wiimote's) is only on or off, so a strength is
    // faked by pulsing it: each frame the strength is added to an accumulator and the motor runs
    // on the frames it tips over 1 (0.5 = every other frame, 1 = always on). The motor is only
    // told when it changes between on and off.
    struct RumbleState
    {
        float mStrength = 0.0f;
        float mAccum = 0.0f;
        uint64_t mEndUs = 0; // 0 = not rumbling
        bool mMotorOn = false;
        bool mOnWiimote = false; // (the device the motor was started on, on Wii)
    };

    RumbleState sRumble[INPUT_MAX_GAMEPADS];

    void SetMotor(int32_t i, bool on)
    {
        RumbleState& r = sRumble[i];
        if (on == r.mMotorOn)
        {
            return;
        }
        r.mMotorOn = on;

#if PLATFORM_WII
        // A Wiimote or classic controller rumbles through the Wiimote, a GameCube pad by itself
        if (on)
        {
            r.mOnWiimote = (GetEngineState()->mInput.mGamepads[i].mType != GamepadType::GameCube);
        }
        if (r.mOnWiimote)
        {
            WPAD_Rumble(WPAD_CHAN_0 + i, on ? 1 : 0);
            return;
        }
#endif

        PAD_ControlMotor(i, on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
    }

    void StopRumble(int32_t i)
    {
        SetMotor(i, false);
        sRumble[i] = RumbleState();
    }

    void UpdateRumble()
    {
        uint64_t nowUs = SYS_GetTimeMicroseconds();
        for (int32_t i = 0; i < INPUT_MAX_GAMEPADS; ++i)
        {
            RumbleState& r = sRumble[i];
            if (r.mEndUs == 0)
            {
                continue;
            }
            if (nowUs >= r.mEndUs)
            {
                StopRumble(i);
                continue;
            }

            r.mAccum += r.mStrength;
            bool on = false;
            if (r.mAccum >= 1.0f)
            {
                on = true;
                r.mAccum -= 1.0f;
            }
            SetMotor(i, on);
        }
    }
}

void INP_Initialize()
{
#if PLATFORM_WII
    PAD_Init();
    WPAD_Init();

    WPAD_SetVRes(WPAD_CHAN_ALL, 640, 480);

    for (uint32_t i = 0; i < INPUT_MAX_GAMEPADS; ++i)
    {
        WPAD_SetDataFormat(WPAD_CHAN_0 + i, WPAD_FMT_BTNS_ACC_IR);

        InputState& input = GetEngineState()->mInput;
        input.mGamepads[i].mType = GamepadType::Wiimote;
    }

#else
    PAD_Init();
#endif

    InputInit();
}

void INP_Shutdown()
{
    for (int32_t i = 0; i < INPUT_MAX_GAMEPADS; ++i)
    {
        PAD_ControlMotor(i, PAD_MOTOR_STOP);
#if PLATFORM_WII
        WPAD_Rumble(WPAD_CHAN_0 + i, 0);
#endif
        sRumble[i] = RumbleState();
    }

    InputShutdown();
}

void INP_Update()
{
    InputAdvanceFrame();

    InputState& input = GetEngineState()->mInput;

#if PLATFORM_WII
    WPAD_ScanPads();

    for (uint32_t i = 0; i < INPUT_MAX_GAMEPADS; ++i)
    {
        WPADData* data = WPAD_Data(i);

        input.mGamepads[i].mConnected = (data->err == WPAD_ERR_NONE);

        struct expansion_t expData;
        WPAD_Expansion(i, &expData); // Get expansion info from the first wiimote

        if (expData.type == WPAD_EXP_CLASSIC)
        {
            int32_t held = expData.classic.btns_held;

            if (held != 0)
            {
                input.mGamepads[i].mType = GamepadType::WiiClassic;
            }

            if (input.mGamepads[i].mType != GamepadType::WiiClassic)
            {
                continue;
            }

            // Buttons
            input.mGamepads[i].mButtons[GAMEPAD_A] = held & CLASSIC_CTRL_BUTTON_A;
            input.mGamepads[i].mButtons[GAMEPAD_B] = held & CLASSIC_CTRL_BUTTON_B;
            input.mGamepads[i].mButtons[GAMEPAD_X] = held & CLASSIC_CTRL_BUTTON_X;
            input.mGamepads[i].mButtons[GAMEPAD_Y] = held & CLASSIC_CTRL_BUTTON_Y;
            input.mGamepads[i].mButtons[GAMEPAD_Z] = held & CLASSIC_CTRL_BUTTON_ZL;
            input.mGamepads[i].mButtons[GAMEPAD_R1] = held & CLASSIC_CTRL_BUTTON_FULL_R;
            input.mGamepads[i].mButtons[GAMEPAD_L1] = held & CLASSIC_CTRL_BUTTON_FULL_L;
            input.mGamepads[i].mButtons[GAMEPAD_LEFT] = held & CLASSIC_CTRL_BUTTON_LEFT;
            input.mGamepads[i].mButtons[GAMEPAD_RIGHT] = held & CLASSIC_CTRL_BUTTON_RIGHT;
            input.mGamepads[i].mButtons[GAMEPAD_DOWN] = held & CLASSIC_CTRL_BUTTON_DOWN;
            input.mGamepads[i].mButtons[GAMEPAD_UP] = held & CLASSIC_CTRL_BUTTON_UP;
            input.mGamepads[i].mButtons[GAMEPAD_START] = held & CLASSIC_CTRL_BUTTON_PLUS;
            input.mGamepads[i].mButtons[GAMEPAD_SELECT] = held & CLASSIC_CTRL_BUTTON_MINUS;
            input.mGamepads[i].mButtons[GAMEPAD_HOME] = held & CLASSIC_CTRL_BUTTON_HOME;

            // Axes
            int32_t leftStickX = int32_t(expData.classic.ljs.pos.x - expData.classic.ljs.center.x);
            int32_t leftStickY = int32_t(expData.classic.ljs.pos.y - expData.classic.ljs.center.y);
            int32_t rightStickX = int32_t(expData.classic.rjs.pos.x - expData.classic.rjs.center.x);
            int32_t rightStickY = int32_t(expData.classic.rjs.pos.y - expData.classic.rjs.center.y);
            float leftTrigger = expData.classic.l_shoulder;
            float rightTrigger = expData.classic.r_shoulder;

            input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTHUMB_X] = glm::clamp(leftStickX / 31.0f, -1.0f, 1.0f);
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTHUMB_Y] = glm::clamp(leftStickY / 31.0f, -1.0f, 1.0f);
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTHUMB_X] = glm::clamp(rightStickX / 15.0f, -1.0f, 1.0f);
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTHUMB_Y] = glm::clamp(rightStickY / 15.0f, -1.0f, 1.0f);
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTRIGGER] = leftTrigger;
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTRIGGER] = rightTrigger;
        }
        else
        {
            int32_t held = WPAD_ButtonsHeld(i);

            if (held != 0)
            {
                input.mGamepads[i].mType = GamepadType::Wiimote;
            }

            if (input.mGamepads[i].mType != GamepadType::Wiimote)
            {
                continue;
            }

            // Buttons
            input.mGamepads[i].mButtons[GAMEPAD_A] = held & WPAD_BUTTON_A;
            input.mGamepads[i].mButtons[GAMEPAD_B] = held & WPAD_BUTTON_B;
            input.mGamepads[i].mButtons[GAMEPAD_X] = held & WPAD_BUTTON_1;
            input.mGamepads[i].mButtons[GAMEPAD_Y] = held & WPAD_BUTTON_2;
            input.mGamepads[i].mButtons[GAMEPAD_Z] = held & WPAD_NUNCHUK_BUTTON_Z;
            input.mGamepads[i].mButtons[GAMEPAD_C] = held & WPAD_NUNCHUK_BUTTON_C;
            input.mGamepads[i].mButtons[GAMEPAD_LEFT] = held & WPAD_BUTTON_LEFT;
            input.mGamepads[i].mButtons[GAMEPAD_RIGHT] = held & WPAD_BUTTON_RIGHT;
            input.mGamepads[i].mButtons[GAMEPAD_DOWN] = held & WPAD_BUTTON_DOWN;
            input.mGamepads[i].mButtons[GAMEPAD_UP] = held & WPAD_BUTTON_UP;
            input.mGamepads[i].mButtons[GAMEPAD_START] = held & WPAD_BUTTON_PLUS;
            input.mGamepads[i].mButtons[GAMEPAD_SELECT] = held & WPAD_BUTTON_MINUS;
            input.mGamepads[i].mButtons[GAMEPAD_HOME] = held & WPAD_BUTTON_HOME;

            struct expansion_t expData;
            WPAD_Expansion(i, &expData); // Get expansion info from the first wiimote

            if (expData.type == WPAD_EXP_NUNCHUK)
            {
                int32_t stickX = int32_t(expData.nunchuk.js.pos.x - expData.nunchuk.js.center.x);
                int32_t stickY = int32_t(expData.nunchuk.js.pos.y - expData.nunchuk.js.center.y);

                // Axes
                input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTHUMB_X] = glm::clamp(stickX / 127.0f, -1.0f, 1.0f);
                input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTHUMB_Y] = glm::clamp(stickY / 127.0f, -1.0f, 1.0f);
            }

            // IR support
            struct ir_t irData;
            WPAD_IR(i, &irData);
            INP_SetTouchPosition((int32_t)irData.x, (int32_t)irData.y, i);

            // Orientation
            orient_t orientation;
            WPAD_Orientation(i, &orientation);
            input.mGamepads[i].mOrientation[0] = orientation.pitch;
            input.mGamepads[i].mOrientation[1] = -orientation.yaw;
            input.mGamepads[i].mOrientation[2] = orientation.roll;

            // Accelerometer
            vec3w_t accel;
            WPAD_Accel(i, &accel);
            const float kAccelRange = 1.0f;
            input.mGamepads[i].mAccel[0] = accel.x / kAccelRange;
            input.mGamepads[i].mAccel[1] = accel.y / kAccelRange;
            input.mGamepads[i].mAccel[2] = accel.z / kAccelRange;
        }
    }

#endif

    PADStatus pads[4];
    //PAD_ScanPads();
    PAD_Read(pads);

    for (uint32_t i = 0; i < INPUT_MAX_GAMEPADS; ++i)
    {
        input.mGamepads[i].mConnected = (pads[i].err == PAD_ERR_NONE);

#if PLATFORM_WII
        if (pads[i].button != 0)
        {
            input.mGamepads[i].mType = GamepadType::GameCube;
        }

        if (input.mGamepads[i].mType != GamepadType::GameCube)
        {
            continue;
        }
#endif

        // Buttons
        input.mGamepads[i].mButtons[GAMEPAD_A] = pads[i].button & PAD_BUTTON_A;
        input.mGamepads[i].mButtons[GAMEPAD_B] = pads[i].button & PAD_BUTTON_B;
        input.mGamepads[i].mButtons[GAMEPAD_X] = pads[i].button & PAD_BUTTON_X;
        input.mGamepads[i].mButtons[GAMEPAD_Y] = pads[i].button & PAD_BUTTON_Y;
        input.mGamepads[i].mButtons[GAMEPAD_Z] = pads[i].button & PAD_TRIGGER_Z;
        input.mGamepads[i].mButtons[GAMEPAD_R1] = pads[i].button & PAD_TRIGGER_R;
        input.mGamepads[i].mButtons[GAMEPAD_L1] = pads[i].button & PAD_TRIGGER_L;
        input.mGamepads[i].mButtons[GAMEPAD_LEFT] = pads[i].button & PAD_BUTTON_LEFT;
        input.mGamepads[i].mButtons[GAMEPAD_RIGHT] = pads[i].button & PAD_BUTTON_RIGHT;
        input.mGamepads[i].mButtons[GAMEPAD_DOWN] = pads[i].button & PAD_BUTTON_DOWN;
        input.mGamepads[i].mButtons[GAMEPAD_UP] = pads[i].button & PAD_BUTTON_UP;
        input.mGamepads[i].mButtons[GAMEPAD_START] = pads[i].button & PAD_BUTTON_START;
        input.mGamepads[i].mButtons[GAMEPAD_HOME] = 0;

        // Axes
        input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTHUMB_X] = glm::clamp(pads[i].stickX / 127.0f, -1.0f, 1.0f);
        input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTHUMB_Y] = glm::clamp(pads[i].stickY / 127.0f, -1.0f, 1.0f);
        input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTHUMB_X] = glm::clamp(pads[i].substickX / 127.0f, -1.0f, 1.0f);
        input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTHUMB_Y] = glm::clamp(pads[i].substickY / 127.0f, -1.0f, 1.0f);
        input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTRIGGER] = pads[i].triggerL / 255.0f;
        input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTRIGGER] = pads[i].triggerR / 255.0f;
    }

    // (After the pads are read, so on Wii each one's type is known.)
    UpdateRumble();

    InputPostUpdate();
}

void INP_SetCursorPos(int32_t x, int32_t y)
{

}

void INP_ShowCursor(bool show)
{

}

void INP_LockCursor(bool lock)
{

}

void INP_TrapCursor(bool trap)
{

}

const char* INP_ShowSoftKeyboard(bool show)
{
    return nullptr;
}

bool INP_IsSoftKeyboardShown()
{
    return false;
}

void INP_SetGamepadRumble(float low, float high, float seconds, int32_t gamepadIndex)
{
    if (gamepadIndex < 0 || gamepadIndex >= INPUT_MAX_GAMEPADS)
    {
        return;
    }

    // One motor: the stronger of the two strengths drives it
    float strength = glm::max(glm::clamp(low, 0.0f, 1.0f), glm::clamp(high, 0.0f, 1.0f));
    if (!(seconds > 0.0f) || strength <= 0.0f)
    {
        StopRumble(gamepadIndex);
        return;
    }

    // The accumulator is kept, so calling this every frame keeps the pulsing even
    RumbleState& r = sRumble[gamepadIndex];
    r.mStrength = strength;
    r.mEndUs = SYS_GetTimeMicroseconds() + (uint64_t)(seconds * 1000000.0f);
}


#endif