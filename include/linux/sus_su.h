#ifndef __KSU_H_SUS_SU
#define __KSU_H_SUS_SU

/*
 * sus_su uses ksu_handle_devpts, ksu_handle_faccessat, etc.
 * These are defined in KernelSU-Next module (feature/sucompat.c, etc.)
 * and accessible via extern declarations in sus_su.c.
 */

int sus_su_fifo_init(int *maj_dev_num, char *drv_path);
int sus_su_fifo_exit(int *maj_dev_num, char *drv_path);

#endif
