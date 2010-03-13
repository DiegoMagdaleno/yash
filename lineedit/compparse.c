/* Yash: yet another shell */
/* compparse.c: simple parser for command line completion */
/* (C) 2007-2010 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "../common.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wchar.h>
#include "../strbuf.h"
#include "../util.h"
#include "../xfnmatch.h"
#include "complete.h"
#include "compparse.h"
#include "editing.h"


/* This structure contains data used during parsing */
typedef struct parseinfo_T {
    xwcsbuf_T buf;
    size_t bufindex;
    size_t mainindex;
    le_context_T *ctxt;
} parseinfo_T;
/* The `buf' buffer contains the first `le_main_index' characters of the edit
 * buffer. During parsing, alias substitution may be performed on this buffer.
 * The `bufindex' and `mainindex' indices indicate the point the parser is
 * currently parsing. `bufindex' is an index into the `buf' buffer and
 * `mainindex' into the edit buffer (`le_main_buffer').
 * The `ctxt' member points to the structure in which the final result is saved.
 */


static bool cparse_commands(parseinfo_T *pi)
    __attribute__((nonnull));


/* Parses the contents of the edit buffer (`le_main_buffer') from the beginning
 * up to the current cursor position (`le_main_index') and determines the
 * current completion context.
 * The result is put in `*ctxt'.
 * Returns true iff successful.
 * The context data must be freed using the `le_free_context' function
 * regardless of whether successful or not. */
bool le_get_context(le_context_T *ctxt)
{
    assert(wcslen(le_main_buffer.contents) == le_main_buffer.length);

    parseinfo_T pi;
    pi.bufindex = pi.mainindex = le_main_index;
    pi.ctxt = ctxt;
    wb_ncat_force(wb_init(&pi.buf), le_main_buffer.contents, le_main_index);

    cparse_commands(&pi);

    wb_destroy(&pi.buf);

    if (ctxt->type == CTXT_NORMAL
	    && is_pathname_matching_pattern(ctxt->pattern)) {
	ctxt->substsrc = true;
    } else {
	xwcsbuf_T buf;
	ctxt->substsrc = false;
	ctxt->pattern =
	    wb_towcs(wb_wccat(wb_initwith(&buf, ctxt->pattern), L'*'));
    }
    ctxt->cpattern = xfnm_compile(
	    ctxt->pattern, XFNM_HEADONLY | XFNM_TAILONLY);
    if (ctxt->cpattern == NULL) {
	le_compdebug("failed to compile pattern \"%ls\"", ctxt->pattern);
	return false;
    }
    return true;
}

/* Following parser functions return true iff parsing is finished and the result
 * is saved in `pi->ctxt'.
 * The result is saved in the following variables of the context structure:
 *     quote, type, pwordc, pwords, src, pattern, srcindex. */

/* Parses commands from the current position until a right parenthesis (")") is
 * found or the whole line is parsed. */
bool cparse_commands(parseinfo_T *pi)
{
    //TODO
}

/* Frees the contents of the specified `le_context_T' data. */
void le_free_context(le_context_T *ctxt)
{
    recfree(ctxt->pwords, free);
    free(ctxt->src);
    free(ctxt->pattern);
    xfnm_free(ctxt->cpattern);
}


/* vim: set ts=8 sts=4 sw=4 noet tw=80: */
