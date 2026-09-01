/*
 * gwalk — read a captured gather back as the command stream it is.
 *
 * The traces are a flat wall of words. What matters in them is the shape:
 * which method each run of words is destined for, and how many. Walking the
 * opcodes turns the wall into that list, so a block can be looked up by the
 * register it writes rather than by hunting hex by eye.
 *
 * Reads the `GCMD[n]: w w w ...` lines of an isp_trace capture, in file
 * order, and walks host1x opcodes: INCR (1), NONINCR (2) and IMM (4).
 *
 * Usage: gwalk <trace file> [--method=0xNNN] [--data]
 *   --method  print only blocks going to this method, with their words
 *   --data    print the words of every block
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char **argv)
{
    const char *path = 0;
    long want = -1;
    int show_data = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--method=", 9) == 0)
            want = strtol(argv[i] + 9, 0, 0);
        else if (strcmp(argv[i], "--data") == 0) show_data = 1;
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: gwalk <trace> [--method=0xNNN]\n");
                 return 1; }

    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }

    size_t cap = 1 << 20, n = 0;
    uint32_t *w = malloc(cap * sizeof *w);
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "]:");
        if (!p) continue;
        p += 2;
        for (;;) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\n' || *p == 0) break;
            char *end;
            unsigned long v = strtoul(p, &end, 16);
            if (end == p) break;
            if (n == cap) { cap *= 2; w = realloc(w, cap * sizeof *w); }
            w[n++] = (uint32_t)v;
            p = end;
        }
    }
    fclose(f);
    fprintf(stderr, "%zu words\n", n);

    /* A run of words whose leading opcode makes sense is taken as a block;
     * anything else is skipped a word at a time, since a capture can begin
     * in the middle of somebody's data. */
    for (size_t i = 0; i < n; ) {
        uint32_t v = w[i];
        unsigned op = v >> 28, m = (v >> 16) & 0xFFF, c = v & 0xFFFF;
        if ((op == 1 || op == 2) && c > 0 && c <= 1024 && i + c < n) {
            if (want < 0 || (long)m == want) {
                printf("[%6zu] %s 0x%03x x%u\n", i,
                       op == 1 ? "INCR" : "NONI", m, c);
                if (show_data || want >= 0)
                    for (unsigned k = 0; k < c; k++)
                        printf("   [%3u] %08x%s", k, w[i + 1 + k],
                               (k % 6 == 5 || k + 1 == c) ? "\n" : "");
            }
            i += 1 + c;
            continue;
        }
        if (op == 4) {
            if (want < 0 || (long)m == want)
                printf("[%6zu] IMM  0x%03x = 0x%04x\n", i, m, c);
            i++;
            continue;
        }
        i++;
    }
    free(w);
    return 0;
}
