/*
 * IPFUnpacker is a free tool for extract and decrypt ipf files
 *
 * Spl3en <spl3en.contact@gmail.com> 2015 ~ 2016
 * Lara Maia <dev@lara.click> 2017
 *
 * IPFUnpacker is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * IPFUnpacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with IPFUnpacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ipf_unpacker.h"
#include "dbg/dbg.h"
#include "crc32/crc32.h"
#include "fs/fs.h"
#include "zlib/zlib.h"
#include "md5/md5.h"
#include "ipf.h"
#include "ies.h"
#include <libgen.h>
#include <stdbool.h>
#include <getopt.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

char *supported_extensions[] = {"xml", "ies", "jpg", "png", "tga", "lua"};


void keys_generate (uint32_t *keys)
{
    uint8_t password[20] = {0x6F, 0x66, 0x4F, 0x31, 0x61, 0x30, 0x75, 0x65, 0x58, 0x41, 0x3F, 0x20, 0x5B, 0xFF, 0x73, 0x20, 0x68, 0x20, 0x25, 0x3F};

    keys[0] = 0x12345678;
    keys[1] = 0x23456789;
    keys[2] = 0x34567890;

    for (int i = 0; i < sizeof(password); i++) {
        keys_update (keys, password[i]);
    }
}

void keys_update (uint32_t *keys, char b)
{
    keys[0] = compute_crc32 (keys[0], b);
    keys[1] = 0x8088405 * ((uint8_t) keys[0] + keys[1]) + 1;
    keys[2] = compute_crc32 (keys[2], BYTE3 (keys[1]));
}

void ipf_decrypt (uint8_t *buffer, size_t size)
{
    uint32_t keys[3];
    keys_generate (keys);
    size = ((size - 1) >> 1) + 1;

    for (int i = 0; i < size; i++, buffer += 2) {
        uint16_t v = (keys[2] & 0xFFFD) | 2;
        *buffer ^= (v * (v ^ 1)) >> 8;
        keys_update (keys, *buffer);
    }
}

void ipf_encrypt (uint8_t *buffer, size_t size)
{
    uint32_t keys[3];
    keys_generate (keys);
    size = ((size - 1) >> 1) + 1;

    for (int i = 0; i < size; i++, buffer += 2) {
        uint16_t v = (keys[2] & 0xFFFD) | 2;
        keys_update (keys, *buffer);
        *buffer ^= (v * (v ^ 1)) >> 8;
    }
}

typedef struct {
    FILE *output;
} IesParams;
static bool process_ies (IesTable *table, void *userdata)
{
    IesParams *params = (IesParams *) userdata;
    FILE *output = params->output;

    for (int colId = 0; colId < table->header->colsCount; colId++) {
        IesColumn *col = &table->columns[colId];
        fprintf (output, "%s", col->name);
        if (colId != table->header->colsCount - 1) {
            fprintf (output, ",");
        }
    }
    fprintf(output, "\n");

    for (int rowId = 0; rowId < table->header->rowsCount; rowId++)
    {
        IesRow *row = &table->rows[rowId];
        for (int cellId = 0; cellId < row->cellsCount; cellId++)
        {
            IesCell(0, 0) *_cell = (void *) row->cells[cellId];
            IesCell(_cell->optSize, _cell->strSize) *cell = (void *) _cell;
            IesColumn *col = cell->col;

            switch (col->type) {
                case 0: {
                    if (cell->flt.value == (int) cell->flt.value) {
                        fprintf (output, "%d", (int) cell->flt.value);
                    } else {
                        fprintf (output, "%f", cell->flt.value);
                    }
                    if (cellId != row->cellsCount - 1) {
                        fprintf (output, ",");
                    }
                    // info ("\t[%d][%s] => %f", cellId, col->name, cell->flt.value);
                } break;

                case 1:
                case 2: {
                    fprintf (output, "\"%s\"", cell->str.value);
                    if (cellId != row->cellsCount - 1) {
                        fprintf (output, ",");
                    }
                    // info ("\t[%d][%s] => <%s>", cellId, col->name, cell->str.value);
                } break;
            }
        }
        fprintf(output, "\n");
    }

    return true;
}

typedef struct {
    PackAction action;
    Zlib *zlib;
    char **extensions;
    size_t extensionsCount;
    char *output;
} IpfParams;
static bool process_ipf (uint8_t *data, size_t dataSize, char *archive, char *filename, void *userdata)
{
    int status = 0;
    IpfParams *params = (IpfParams *) userdata;
    PackAction action = params->action;
    Zlib *zlib = params->zlib;
    char **extensions = params->extensions;
    size_t extensionsCount = params->extensionsCount;
    char *output = params->output;

    // Check if the file is encrypted
    int crypted_extension (char *filename) {
        return (
            // Those files aren't encrypted
            file_is_extension (filename, "mp3")
        ||  file_is_extension (filename, "fsb")
        ||  file_is_extension (filename, "jpg"));
    }

    // Check if the file is worth being decompressed
    int worth_decompress (char *filename, char **extensions, size_t extensionsCount) {
        for (int i = 0; i < extensionsCount; i++) {
            if (file_is_extension (filename, extensions[i])) {
                return true;
            }
        }

        return false;
    }

    switch (action)
    {
        case ACTION_EXTRACT: {
            uint8_t *fileContent = data;
            size_t fileSize = dataSize;

            // We don't decompress all the files, only those interesting
            if (worth_decompress (filename, extensions, extensionsCount) && !(crypted_extension (filename))) {
                if (!(zlibDecompress (zlib, data, dataSize))) {
                    error ("Cannot decompress '%s'.", filename);
                    goto cleanup;
                }

                fileContent = zlib->buffer;
                fileSize = zlib->header.size;
            }

            // Get basename and dirname
            char *name = basename (filename);
            char *path = dirname (filename);
            name = (name) ? name : filename;

            if (!(path && name)) {
                error ("Cannot extract directory / filename : %s / %s.", archive, filename);
                goto cleanup;
            }

            char target_dir[PATH_MAX];
            char target_path[PATH_MAX];

            if(output)
            {
                sprintf(target_dir, "%s/%s/%s", output, archive, path);
            } else {
                sprintf(target_dir, "%s/%s", archive, path);
            }

            mkpath(target_dir);

            sprintf(target_path, "%s/%s", target_dir, name);

            // If we decompressed it, write the data in the file
            if (worth_decompress (name, extensions, extensionsCount))
            {
                if(file_is_extension(name, "ies")) {
                    // IES parser
                    FILE *ies = fopen(target_path, "wb+");
                    IesParams iesParams = {.output = ies};
                    ies_read(fileContent, fileSize, process_ies, &iesParams);
                    fclose(ies);
                }
                else {
                    if (!(file_write(target_path, fileContent, fileSize))) {
                        error("Cannot write data to '%s'.", target_path);
                        goto cleanup;
                    }
                }
            }
            else {
                // Write the MD5 inside the file
                char md5[33] = {0};
                MD5_bufferEx (data, dataSize, md5);
                if (!(file_write(target_path, (uint8_t *)md5, sizeof(md5) - 1))) {
                    error("Cannot write md5 to '%s'.", target_path);
                    goto cleanup;
                }
            }
        }
        break;

        case ACTION_DECRYPT:
            if (!(crypted_extension (filename))) {
                ipf_decrypt (data, dataSize);
            }
        break;

        case ACTION_ENCRYPT:
            if (!(crypted_extension (filename))) {
                ipf_encrypt (data, dataSize);
            }
        break;
    }

    status = true;
cleanup:
    return status;
}

int show_help(char **argv)
{
    printf("Usage: %s [-d|-c|-e --quiet] ipf_file <output_dir>\n\n", argv[0]);
    printf("  -d, --decrypt       decrypts an ipf file\n");
    printf("  -c, --encrypt       encrypts an ipf file\n");
    printf("  -e, --extract       extract files to dir\n");
    printf("  --quiet             disable output\n\n");

    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    int options;
    int option_index = 0;
    static int quiet_flag;
    static char input_file[PATH_MAX];
    IpfParams params = {0};

    static struct option long_options[] =
        {
            {"decrypt", required_argument, NULL, 'd'},
            {"encrypt", required_argument, NULL, 'c'},
            {"extract", required_argument, NULL, 'e'},
            {"quiet", no_argument, &quiet_flag, 1},
            {NULL, no_argument, NULL, 0}
        };

    while(1)
    {
        options = getopt_long(argc, argv, "d:c:e:q", long_options, &option_index);

        // no more options to parse
        if(options == -1)
        {
            // default
            if(argc == 1)
            {
                show_help(argv);
            }
            break;
        }

        switch(options)
        {
            case 'd':
                memcpy(input_file, optarg, strlen(optarg)+1);
                params.action = ACTION_DECRYPT;
                break;
            case 'c':
                memcpy(input_file, optarg, strlen(optarg)+1);
                params.action = ACTION_ENCRYPT;
                break;
            case 'e':
                memcpy(input_file, optarg, strlen(optarg)+1);
                params.action = ACTION_EXTRACT;
                params.zlib = calloc(1, sizeof(Zlib)+1);
                break;
            case '?':
                // something wrong with the params.
                // getopt_long already print the error
                show_help(argv);
                break;
        }
    }

    if(quiet_flag)
    {
        printf("[STUB] quiet flag unavailable for now\n");
    }

    if(optind < argc) {
        printf("Setting output dir to %s\n", argv[optind]);
        params.output = argv[optind];
    } else {
        char *file_name = basename(input_file);
        printf("Warning: Output dir not defined. Setting to %.*s dir\n", (int)strlen(file_name)-4, file_name);
    }

    params.extensions = supported_extensions;
    params.extensionsCount = sizeof(supported_extensions) / sizeof(*supported_extensions);

    // Read the ipf_encrypted IPF
    size_t size;
    uint8_t *pfile;

    if (!(pfile = file_map(input_file, &size))) {
        fprintf(stderr, "Cannot map '%s'\n", input_file);
        return 1;
    }

    printf("Processing '%s'\n", input_file);

    if (!(ipf_read(pfile, size, process_ipf, &params))) {
        fprintf(stderr, "Cannot read '%s'\n", input_file);
        return 1;
    }

    file_flush(input_file, pfile, size);
    free(params.zlib);

    printf("Done!\n");

    return 0;
}
