#include <Python.h>
#include <SDL.h>

int RPVITA_exit_process(int res);

/* Video Player */

void RPVITA_video_init();
PyObject *RPVITA_video_read_video();
void RPVITA_video_start(const char *file);
void RPVITA_video_stop();
int RPVITA_video_get_playing();
void RPVITA_periodic();
