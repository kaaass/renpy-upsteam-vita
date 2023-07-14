#include "psp2/kernel/processmgr.h"

int RPVITA_exit_process(int res) {
    sceKernelExitProcess(res);
}
