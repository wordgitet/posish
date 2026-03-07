/* SPDX-License-Identifier: 0BSD */

/* posish - internal expansion marker bytes */

#ifndef POSISH_EXPAND_MARKERS_H
#define POSISH_EXPAND_MARKERS_H

#define QUOTED_IFS_SPACE '\x81'
#define QUOTED_IFS_TAB '\x82'
#define QUOTED_IFS_NEWLINE '\x83'
#define QUOTED_GLOB_STAR '\x84'
#define QUOTED_GLOB_QMARK '\x85'
#define QUOTED_GLOB_LBRACK '\x86'
#define QUOTED_EMPTY_MARK '\x87'
#define QUOTED_LITERAL_PREFIX '\x88'
#define PARAM_AT_SPLIT '\x89'
#define PATTERN_LIT_STAR '\x12'
#define PATTERN_LIT_QMARK '\x13'
#define PATTERN_LIT_LBRACK '\x14'
#define PATTERN_LIT_RBRACK '\x15'
#define PATTERN_LIT_BSLASH '\x16'

#endif
