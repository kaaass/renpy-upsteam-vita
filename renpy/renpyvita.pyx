cdef extern from "renpy_vita.h":
    int RPVITA_exit_process(int res)
    void RPVITA_video_init()
    object RPVITA_video_read_video()
    void RPVITA_video_start(char *file)
    void RPVITA_video_stop()
    int RPVITA_video_get_playing()

def exit_process(res):
    RPVITA_exit_process(res)

def video_init():
    RPVITA_video_init()

def video_read_video():
    return RPVITA_video_read_video()

def video_start(file):
    RPVITA_video_start(file)

def video_stop():
    RPVITA_video_stop()

def video_get_playing():
    return RPVITA_video_get_playing()
