/* laplace model, in a build without MKL. */
#include "engine.h"
#include <stdio.h>

int cmd_model(int argc, char **argv){
    (void)argc; (void)argv;
    fprintf(stderr, "laplace model needs MKL and OpenMP: build with the oneAPI environment (MKLROOT) set\n");
    return 1;
}
