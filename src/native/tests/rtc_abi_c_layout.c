#include "gamebridge_rtc.h"
#include <stddef.h>
_Static_assert(sizeof(gb_rtc_result) == 4, "fixed result width");
_Static_assert(sizeof(gb_rtc_config) == 24, "config layout");
_Static_assert(sizeof(gb_rtc_video) == 40, "video layout");
_Static_assert(sizeof(gb_rtc_audio) == 32, "audio layout");
_Static_assert(sizeof(gb_rtc_media_event)==16,"media callback layout");
_Static_assert(offsetof(gb_rtc_video, data_size) == 16, "video payload length");
int gb_rtc_c_layout(void) { return GB_RTC_ABI_VERSION == 1; }
