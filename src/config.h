/* See LICENSE file for copyright and license details. */
/*
* Command-line options. Parsed with getopt_long in main.c.
*
* Note: -h is --height, not help. Help is -? / --help only, so that the
* conventional "-h" shorthand for height used by this viewer keeps working.
* "--mcp-lsp" is long-only (no short form) to avoid burning a letter.
*
* The shader header/footer strings used to live here too, but they are
* shader_common.c's job now — one definition shared by the viewer and the
* MCP server so they can't drift.
*/
#include <getopt.h>
static const char options_string[] = "?fw:h:s:0:1:2:3:x:y:k:mp:";
static struct option long_options[] = {
{"width", required_argument, 0, 'w'},
{"height", required_argument, 0, 'h'},
{"window_x", required_argument, 0, 'x'},
{"window_y", required_argument, 0, 'y'},
{"fullscreen", no_argument, 0, 'f'},
{"source", required_argument, 0, 's'},
{"texture0", required_argument, 0, '0'},
{"texture1", required_argument, 0, '1'},
{"texture2", required_argument, 0, '2'},
{"texture3", required_argument, 0, '3'},
{"keyboard", required_argument, 0, 'k'},
{"mcp", no_argument, 0, 'm'},
{"mcp-lsp", no_argument, 0, 'M'},
{"mcp-port", required_argument, 0, 'p'},
{"help", no_argument, 0, '?'},
{0, 0, 0, 0}
};
