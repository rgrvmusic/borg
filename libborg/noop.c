/* Empty preload control for testing loader effects without interposition. */
__attribute__((constructor)) static void noop_init(void)
{
}
