/* Test-only LD_PRELOAD shim. Never installed into system library paths.
 * Skip network OTA discovery during explicit integration runs; UART upgrade
 * callbacks and all production binaries are untouched. ABI: hd_ota_check_full. */
int hd_ota_check_full(int type, const char *current, void *info, char **version) {
    (void)type; (void)current; (void)info;
    if (version) *version = 0;
    return -2;
}
