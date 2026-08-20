/* Stand-alone test of rnc2_unpack() against the Lotus 2 install blob.
 *
 * Reads the compressed body out of original/Lotus2CD32/Lotus2CD32.slave
 * (at slave+0x10AA = file offset 0x10CA), decodes it, and reports the
 * first few bytes so we can decide whether the output is meaningful.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int rnc2_unpack(const unsigned char *input, unsigned char *output,
                       unsigned int *out_size);

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/lotus2/Lotus2CD32/Lotus2CD32.slave";
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    unsigned char *img = malloc(size);
    fread(img, 1, size, f);
    fclose(f);
    /* The slave + 0x10AA lives at file offset 0x20 + 0x10AA = 0x10CA. */
    const unsigned char *blob = img + 0x10ca;
    /* Compact code hunk ends at 0x1880 file offset 0x1898, so the blob
     * has up to 0x1894 - 0x10CA = 0x7CA bytes available; the header
     * announces the exact compressed-size inside the blob itself. */
    unsigned char out[8192];
    unsigned int out_size = 0;
    int rc = rnc2_unpack(blob, out, &out_size);
    printf("rc=%d out_size=%u\n", rc, out_size);
    if (rc == 0) {
        printf("first 64 bytes of decompressed body:\n");
        for (int i = 0; i < 64 && i < (int)out_size; i++) {
            printf("%02x ", out[i]);
            if ((i+1) % 16 == 0) printf("\n");
        }
        printf("\n");
        printf("bytes 0x08..0x10 (likely jmp table):\n");
        for (int i = 0x08; i < 0x18 && i < (int)out_size; i++) {
            printf("%02x ", out[i]);
        }
        printf("\n");
    }
    free(img);
    return 0;
}
