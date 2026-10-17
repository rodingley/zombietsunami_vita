/* empty import tables for the host test: everything shows up as missing */
#include "../src/so_util.h"
#include "../src/stage1.h"
const so_default_dynlib shim_dynlib[1] = {{0, 0}};
const int shim_dynlib_count = 0;
const so_default_dynlib direct_dynlib[1] = {{0, 0}};
const int direct_dynlib_count = 0;
void imports_init(void) {}
const so_default_dynlib gl_dynlib[1] = {{0, 0}};
const int gl_dynlib_count = 0;
