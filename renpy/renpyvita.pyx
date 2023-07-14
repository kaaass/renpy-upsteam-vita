cdef extern from "renpy_vita.h":
    int RPVITA_exit_process(int res)

def exit_process(res):
    RPVITA_exit_process(res)
