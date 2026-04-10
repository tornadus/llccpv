#ifndef LLCCPV_PICKER_H
#define LLCCPV_PICKER_H

#include "util.h"

struct picker_result {
    char device_path[64];
    uint32_t pixfmt;   /* selected pixel format, 0 = auto */
    int width;         /* selected resolution, 0 = auto-detect from source */
    int height;
    int scale_mode;    /* -1 = not set (use CLI default), 0/1/2/3 = enum scale_mode */
    int color_range;   /* -1 = not set, 0 = limited (TV), 1 = full (PC) */
    float sharpness;   /* FSR RCAS sharpness, <0 = not set (use default) */
};

#ifdef __cplusplus
extern "C" {
#endif

/* Show the device picker dialog. Populates result on success.
 * Returns 0 if the user selected a device, -1 if cancelled.
 * If auto_select is true and exactly one device is found with
 * an obvious best format, skips the UI. */
int picker_show(struct picker_result *result, bool auto_select);

#ifdef __cplusplus
}
#endif

#endif
