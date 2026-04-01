#include <stdio.h>
#include <stdlib.h>

static void write_diag(const char *message) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    const char *path = "/tmp/bambu-oss-plugin-debug.log";
    char buffer[1024];

    if (xdg && *xdg) {
        snprintf(buffer, sizeof(buffer), "%s/BambuStudio/oss-plugin-debug.log", xdg);
        path = buffer;
    } else if (home && *home) {
        snprintf(buffer, sizeof(buffer), "%s/.config/BambuStudio/oss-plugin-debug.log", home);
        path = buffer;
    }

    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    fprintf(f, "%s\n", message);
    fclose(f);
}

__attribute__((constructor))
static void bambu_source_init(void) {
    write_diag("libBambuSource constructor");
}
