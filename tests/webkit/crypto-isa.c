#include <gcrypt.h>
#include <gnu/libc-version.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>

// Exercise bundled dispatch code whose ELF attributes include ARMv8 crypto,
// while the supported renderer baseline remains ARMv7-A/NEON hard-float.
int main(void)
{
    static const unsigned char expected[] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                              0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                              0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                              0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    const char *version = gcry_check_version("1.9.4");
    if (!version)
        return 1;
    if (gcry_control(GCRYCTL_DISABLE_SECMEM, 0) || gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0))
        return 2;
    gcry_error_t result = gcry_control(GCRYCTL_SELFTEST, 0);
    if (result) {
        fprintf(stderr, "gcrypt selftest: %s\n", gcry_strerror(result));
        return 3;
    }
    unsigned char actual[sizeof(expected)];
    gcry_md_hash_buffer(GCRY_MD_SHA256, actual, "abc", 3);
    if (memcmp(actual, expected, sizeof(expected)))
        return 4;
    printf("CRYPTO_ISA_OK gcrypt=%s glibc=%s hwcap=%lx hwcap2=%lx\n", version,
           gnu_get_libc_version(), getauxval(AT_HWCAP), getauxval(AT_HWCAP2));
    return 0;
}
