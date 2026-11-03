// Sums an lcov trace file over two parts of the tree:
//   ck    files under src/ck-core/ (the Charm++ core)
//   conv  files under contrib/reconverse/src/ (the reconverse runtime)
// coverage/report.sh feeds it the lcov export of the cumulative tier.
// Header copies in the build's include/ directory are not counted.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct Totals {
    long files, LF, LH, FNF, FNH;
};

static void print(const char *name, const struct Totals *t) {
    printf("Summary %s (%ld files):\n", name, t->files);
    printf("\tlines......: %.1f%% (%ld of %ld lines)\n",
           t->LF ? 100.0 * t->LH / t->LF : 0.0, t->LH, t->LF);
    printf("\tfunctions..: %.1f%% (%ld of %ld functions)\n",
           t->FNF ? 100.0 * t->FNH / t->FNF : 0.0, t->FNH, t->FNF);
}

int main(int argc, char **argv) {
    if(argc != 2) {
        fprintf(stderr, "Usage:\n\tck-convcore-summary [lcov trace file]\n");
        exit(1);
    }

    FILE *tracefile = fopen(argv[1], "r");
    if(tracefile == NULL) {
        fprintf(stderr, "Error: couldn't open tracefile %s\n", argv[1]);
        exit(1);
    }

    struct Totals ck = {0, 0, 0, 0, 0}, conv = {0, 0, 0, 0, 0};
    struct Totals *cur = NULL;
    char *lineptr = NULL;
    size_t n = 0;
    while(getline(&lineptr, &n, tracefile) != -1) {
        if(strncmp(lineptr, "SF:", 3) == 0) {
            if(strstr(lineptr, "/src/ck-core/") != NULL)
                cur = &ck;
            else if(strstr(lineptr, "/contrib/reconverse/src/") != NULL)
                cur = &conv;
            else
                cur = NULL;
            if(cur)
                cur->files++;
        } else if(cur == NULL) {
            continue;
        } else if(strncmp(lineptr, "FNF:", 4) == 0) {
            cur->FNF += atol(lineptr + 4);
        } else if(strncmp(lineptr, "FNH:", 4) == 0) {
            cur->FNH += atol(lineptr + 4);
        } else if(strncmp(lineptr, "LF:", 3) == 0) {
            cur->LF += atol(lineptr + 3);
        } else if(strncmp(lineptr, "LH:", 3) == 0) {
            cur->LH += atol(lineptr + 3);
        } else if(strncmp(lineptr, "end_of_record", 13) == 0) {
            cur = NULL;
        }
    }
    free(lineptr);
    fclose(tracefile);

    print("ck (src/ck-core)", &ck);
    print("conv (contrib/reconverse/src)", &conv);
    return 0;
}
