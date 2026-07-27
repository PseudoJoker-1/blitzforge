#include "../include/wotb_mod_api.h"

static int VerifyPublicTypes(void) {
    WotbModInfo info = {0};
    WotbModFrameInfo frame = {0};
    WotbModResourceMountInfo mount = {0};
    WotbModResourceLoadRequest load = {0};
    info.struct_size = (uint32_t)sizeof(info);
    frame.struct_size = (uint32_t)sizeof(frame);
    mount.struct_size = (uint32_t)sizeof(mount);
    load.struct_size = (uint32_t)sizeof(load);
    return info.struct_size > 0 &&
           frame.struct_size > 0 &&
           mount.struct_size > 0 &&
           load.struct_size > 0;
}

int wotbmod_c_abi_compile_test(void) {
    return VerifyPublicTypes() ? 0 : 1;
}
