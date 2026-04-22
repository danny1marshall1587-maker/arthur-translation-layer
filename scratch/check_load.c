#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <so_path>\n", argv[0]);
        return 1;
    }
    void* handle = dlopen(argv[1], RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "Error: %s\n", dlerror());
        return 1;
    }
    printf("Success: %s loaded\n", argv[1]);
    dlclose(handle);
    return 0;
}
