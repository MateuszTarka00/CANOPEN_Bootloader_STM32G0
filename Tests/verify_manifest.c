/** Host-side cross-check of Python signatures against the target's C verifier. */
#include "firmware_auth.h"
#include "sha256.h"
#include "persistent_storage.h"
#include "device_memory.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    uint8_t record[BL_MANIFEST_RECORD_SIZE];
    uint8_t image[DEVICE_IMAGE_MAX];
    uint8_t digest[32];
    BlImageManifest manifest;
    BlSha256 sha;
    FILE *file;
    size_t size;
    if ((argc != 3) && (argc != 4)) { return 2; }
    file = fopen(argv[1], "rb");
    if (file == NULL) { return 2; }
    size = fread(record, 1U, sizeof(record), file);
    fclose(file);
    if ((size != sizeof(record)) ||
        !BlManifest_Decode(record, sizeof(record), &manifest) ||
        !BlAuth_VerifyManifest(&manifest)) { return 1; }
    if (argc == 4) {
        uint8_t factory_record[FI_RECORD_SIZE];
        FiData factory;
        file = fopen(argv[3], "rb");
        if (file == NULL) { return 2; }
        size = fread(factory_record, 1U, sizeof(factory_record), file);
        fclose(file);
        if ((size != sizeof(factory_record)) ||
            (Fi_Decode(factory_record, sizeof(factory_record), &factory) != FI_OK) ||
            !Fi_Compatible(&factory, &manifest.compatibility)) { return 1; }
    }
    file = fopen(argv[2], "rb");
    if (file == NULL) { return 2; }
    size = fread(image, 1U, sizeof(image), file);
    fclose(file);
    if ((size != manifest.image_size) || (Ps_Crc(image, (uint32_t)size) != manifest.image_crc)) {
        return 1;
    }
    BlSha256_Init(&sha);
    BlSha256_Update(&sha, image, (uint32_t)size);
    BlSha256_Final(&sha, digest);
    return memcmp(digest, manifest.image_digest, sizeof(digest)) == 0 ? 0 : 1;
}
