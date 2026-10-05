#include <portmidi.h>
#include <stdio.h>

int main(void) {
    for (int i = 0; i < 10; ++i) {
        PmError error = Pm_Initialize();
        if (error != pmNoError) {
            fprintf(stderr, "Pm_Initialize: %s\n", Pm_GetErrorText(error));
            return 1;
        }
        (void)Pm_CountDevices();
        (void)Pm_GetDefaultInputDeviceID();
        (void)Pm_GetDefaultOutputDeviceID();
        error = Pm_Terminate();
        if (error != pmNoError) {
            fprintf(stderr, "Pm_Terminate: %s\n", Pm_GetErrorText(error));
            return 1;
        }
    }
    return 0;
}
