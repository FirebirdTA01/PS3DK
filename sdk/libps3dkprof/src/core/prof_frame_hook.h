/* The runtime's frame-mark hook, for the live link's capture triggers (live/live_api.c).  No wire or format change.
 *
 * ps3dk_prof_frame calls the installed function on the thread that marks the frame, after the frame record and
 * before the capture-window step of that mark (p_window_frame), so a window the hook begins opens at this very
 * frame and one it ends closes after the frame that just ended: the same frames a capture_begin or capture_end
 * called by the app at the end of the previous frame would give.  The function runs with no runtime lock held, may
 * call the public capture-window API, and must return quickly; it is not called after the stop latch is set.
 * NULL (the default) uninstalls it; the hook's code and data must stay valid for the process lifetime, since a
 * frame mark that loaded the pointer may still be running it after an uninstall. */
#ifndef PS3DK_PROF_FRAME_HOOK_H
#define PS3DK_PROF_FRAME_HOOK_H
void ps3dk_prof__set_frame_hook(void (*fn)(void));

/* Live viewer auto-start (live/live_autostart.c, opt-in).  The core calls these only when that member is linked: the
 * core's references are weak (prof_core.c, prof_drain.c), so a program without the member links neither.
 * after_init: once, at the end of every successful init (any init entry point), on the initializing thread; it must
 * return quickly (it may run in a constructor).  after_shutdown: once, at the end of the shutdown that claimed it,
 * after that shutdown published its finalization, on the shutting-down thread; it may wait for the agent. */
void ps3dk_prof__live_autostart_after_init(void);
void ps3dk_prof__live_autostart_after_shutdown(void);
#endif
