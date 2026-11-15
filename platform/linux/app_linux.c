/* platform.h's program hooks on Linux: GLFW and raylib already do what they are for elsewhere. */
#include "platform.h"

void plat_init(void) {}
void plat_window_ready(void) {}
void plat_frame(int typing) { (void)typing; }
float plat_density(void) { return 1; }
int plat_touch(void) { return 0; }
