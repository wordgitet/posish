/* SPDX-License-Identifier: 0BSD */

/* posish - input interface */

#ifndef POSISH_INPUT_H
#define POSISH_INPUT_H

int input_read_file(const char *path, char **out_contents);

#endif
