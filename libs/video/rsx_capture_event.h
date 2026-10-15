/* Diagnostic-only input event latch shared by the input and render threads. */
#ifndef PS3RECOMP_RSX_CAPTURE_EVENT_H
#define PS3RECOMP_RSX_CAPTURE_EVENT_H
void rsx_capture_pad_configure(unsigned mask);
void rsx_capture_pad_notify(unsigned mask);
unsigned rsx_capture_pad_take(void);
#endif
