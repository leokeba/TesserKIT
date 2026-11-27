// Host test runner: ./tesserkit_tests [-v] [filter] (test/check.h)
#include "device_cases.h"
#include "network_cases.h"
#include "time_cases.h"
#include "firmware_cases.h"
#include "backup_cases.h"
#include "auth_cases.h"
#include "timer_cases.h"
#include "artnet_cases.h"
#include "sacn_cases.h"
#include "mqtt_cases.h"
#include "bluetooth_cases.h"
#include "nowtp_cases.h"
#include "files_cases.h"
#include "sequencer_cases.h"
#include "power_cases.h"
#include "udp_cases.h"
#include "peers_cases.h"
#include "record_cases.h"
#include "settings_cases.h"
#include "chaos_cases.h"
#include "heap_cases.h"
#include "pwm_cases.h"
#include "pulses_cases.h"
#include "button_cases.h"
#include "board_cases.h"
#include "clock_cases.h"

int main(int argc, char** argv) {
    const char* env = getenv("CHECK_VERBOSE");
    bool verbose = env && *env && strcmp(env, "0") != 0;
    const char* filter = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else {
            filter = argv[i];
        }
    }
    return check::run(filter, verbose) ? 1 : 0;
}
