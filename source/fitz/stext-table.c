// Copyright (C) 2024 Artifex Software, Inc.
//
// This file is part of MuPDF.
//
// MuPDF is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// MuPDF is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with MuPDF. If not, see <https://www.gnu.org/licenses/agpl-3.0.en.html>
//
// Alternative licensing terms are available from the licensor.
// For commercial licensing, see <https://www.artifex.com/> or contact
// Artifex Software, Inc., 39 Mesa Street, Suite 108A, San Francisco,
// CA 94129, USA, for further information.

#include "mupdf/fitz.h"

#include <assert.h>

/* #define DEBUG_WRITE_AS_PS */

/* #define DEBUG_TABLE_STRUCTURE */

/*
 * The algorithm.
 *
 *	The goal of the algorithm is to identify tables on a page.
 *	First we have to find the tables on a page, then we have to
 *	figure out where the columns/rows are, and then how the
 *	cells span them.
 *
 *	We do this as a series of steps.
 *
 *	To illustrate what's going on, let's use an example page
 *	that we can follow through all the steps.
 *
 *	+---------------------------+
 *	|                           |
 *	|      #### ## ### ##       |	<- Title
 *	|                           |
 *	|    ##### ##### #### ##    |		\
 *	|    ## ###### ###### ##    |    |
 *	|    #### ####### ######    |    |- Abstract
 *	|    ####### #### ## ###    |    |
 *	|    ### ##### ######       |   /
 *	|                           |
 *	|   #########   #########   |   2 Columns of text
 *	|   #########   #########   |
 *	|   ########    #########   |
 *	|               #########   |
 *	|   +-------+   #######     |   <- With an image on the left
 *	|   |       |               |
 *	|   |       |   ## ## # #   |   <- And a table on the right
 *	|   +-------+   ## ## # #   |
 *	|               ## ## # #   |
 *	|   #########   ## ## # #   |
 *	|   #########   ## ## # #   |
 *	|   #########   ## ## # #   |
 *	|                           |
 *	+---------------------------+
 *
 *
 * Step 1: Segmentation.
 *
 *	First, we segment the page, trying to break it down into a
 *	series of non-overlapping rectangles. We do this (in stext-boxer.c)
 *	by looking for where the content isn't. If we can identify breaks
 *	that run through the page (either from top to bottom or from left
 *	to right), then we can split the page there, and recursively consider
 *	the two halves of the page.
 *
 *	It's not a perfect algorithm, but it manages to in many cases.
 *
 *	After segmenting the above example, first we'll find the horizontal
 *	splits, giving:
 *
 *	+---------------------------+
 *	|                           |
 *	|      #### ## ### ##       |
 *	+---------------------------+
 *	|    ##### ##### #### ##    |
 *	|    ## ###### ###### ##    |
 *	|    #### ####### ######    |
 *	|    ####### #### ## ###    |
 *	|    ### ##### ######       |
 *	+---------------------------+
 *	|   #########   #########   |
 *	|   #########   #########   |
 *	|   ########    #########   |
 *	|               #########   |
 *	|   +-------+   #######     |
 *	|   |       |               |
 *	|   |       |   ## ## # #   |
 *	|   +-------+   ## ## # #   |
 *	|               ## ## # #   |
 *	|   #########   ## ## # #   |
 *	|   #########   ## ## # #   |
 *	|   #########   ## ## # #   |
 *	|                           |
 *	+---------------------------+
 *
 *	Then we'll recurse and find the vertical split between
 *	the columns:
 *
 *	+---------------------------+
 *	|                           |
 *	|      #### ## ### ##       |
 *	+---------------------------+
 *	|    ##### ##### #### ##    |
 *	|    ## ###### ###### ##    |
 *	|    #### ####### ######    |
 *	|    ####### #### ## ###    |
 *	|    ### ##### ######       |
 *	+-------------+-------------+
 *	|   ######### | #########   |
 *	|   ######### | #########   |
 *	|   ########  | #########   |
 *	|             | #########   |
 *	|   +-------+ | #######     |
 *	|   |       | |             |
 *	|   |       | | ## ## # #   |
 *	|   +-------+ | ## ## # #   |
 *	|             | ## ## # #   |
 *	|   ######### | ## ## # #   |
 *	|   ######### | ## ## # #   |
 *	|   ######### | ## ## # #   |
 *	|             |             |
 *	+-------------+-------------+
 *
 *	Then we recurse again and find the horizontal splits
 *	within the columns:
 *
 *	+---------------------------+
 *	|                           |
 *	|      #### ## ### ##       |
 *	+---------------------------+
 *	|    ##### ##### #### ##    |
 *	|    ## ###### ###### ##    |
 *	|    #### ####### ######    |
 *	|    ####### #### ## ###    |
 *	|    ### ##### ######       |
 *	+-------------+-------------+
 *	|   ######### | #########   |
 *	|   ######### | #########   |
 *	|   ########  | #########   |
 *	+-------------+ #########   |
 *	|   +-------+ | #######     |
 *	|   |       | +-------------+
 *	|   |       | | ## ## # #   |
 *	|   +-------+ | ## ## # #   |
 *	+-------------+ ## ## # #   |
 *	|   ######### | ## ## # #   |
 *	|   ######### | ## ## # #   |
 *	|   ######### | ## ## # #   |
 *	|             |             |
 *	+-------------+-------------+
 *
 *	We recurse a fixed maximum number of times (currently
 *	6, IIRC) or until we fail to find any suitable splits.
 *
 *	This completes the page segmentation step.
 *
 * Step 2: Grid finding
 *
 *	Next, we look at each of those segments and try to identify
 *	where grids might be.
 *
 *	Imagine the bottom right section of that page as
 *	a board with lego blocks on where there is text.
 *	Now imagine viewing that from the bottom of the page.
 *	The gaps between the columns of the table are where you
 *	can see through to the top between the blocks.
 *
 *	Similarly, if you view it from the side, the gaps between the
 *	rows of the page are where you can see through to the other
 *	side.
 *
 *	So, how do we code that? Well, we run through the page content
 *	(obviously, restricted to the content that falls into this
 *	segment of the page - that'll go without saying from here on
 *	in). For each bit of content, we look at the "x extent" of that
 *	content - for instance a given string might start at position
 *	10 and continue to position 100. We build a list of all these
 *	start, and stop positions, and keep them in a sorted list.
 *
 *	Then we walk this list from left to right, keeping a sum. I
 *	call this sum "wind", because it's very similar to the winding
 *	number that you get when doing scan conversion of bezier shapes.
 *
 *	wind starts out as 0. We increment it whenever we pass a 'start'
 *	position, and decrement it whenever we pass a 'stop' position.
 *	So at any given x position along the line wind tells us the
 *	number of pieces of content that overlap that x position.
 *	So wind(left) = 0 = wind(right), and wind(x) >= x for all x.
 *
 *	So, if we walk from left to right, the trace of wind might
 *	look something like:
 *
 *	             __
 *	  ___       /  \  _        __
 *	 /   \     /    \/ \     _/  \_
 *	/     \___/         \___/      \
 *
 *	The left and right edges of the table are pretty clear.
 *	The regions where wind drops to 0 represent the column dividers.
 *	The left and right hand side of those regions gives us the min
 *	and max values for that divider.
 *
 *	We can then repeat this process for Y ranges instead of X ranges
 *	to get the row dividers.
 *
 *	BUT, this only works for pure grid tables. It falls down for
 *	cases where we have merged cells (which is very common due to
 *	titles etc).
 *
 *	We can modify the algorithm slightly to allow for this.
 *
 *	Consider the following table:
 *
 *	+-----------------------------------+
 *	|  Long Table title across the top  |
 *	+---------------+---------+---------+
 *	| Name          | Result1 | Result2 |
 *	+---------------+----+----+----+----+
 *	| Homer Simpson |  1 | 23 |  4 | 56 |
 *	| Barney Gumble |  1 | 23 |  4 | 56 |
 *	| Moe           |  1 | 23 |  4 | 56 |
 *	| Apu           |  1 | 23 |  4 | 56 |
 *	| Ned Flanders  |  1 | 23 |  4 | 56 |
 *	+---------------+----+----+----+----+
 *
 *	The wind trace for that looks something like
 *	(with a certain degree of artistic license for the
 *	limitations of ascii art):
 *
 *	   ________
 *	  /        \      _   __     _   _
 *	 /          \____/ \_/  \___/ \_/ \
 *	/                                  \
 *
 *	So, the trace never quite drops back to zero in the
 *	middle due to the spanning of the top title.
 *
 *	So, instead of just looking for points where the trace
 *	drops to zero, we instead look for local minima. Each local
 *	minima represents a place where there might be a grid dividier.
 *	The value of wind at such points can be considered the
 *	"uncertainty" with which there might be a divider there.
 *	Clear dividers (with a wind value of 0) have no uncertainty.
 *	Places where cells are spanned have a higher value of uncertainty.
 *
 *	The output from this step is the list of possible grid positions
 *	(X and Y), with uncertainty values.
 *
 * Step 3: Cell analysis
 *
 *	So, armed with the output from step 2, we can examine each grid
 *	found. If we have W x-dividers and H y-dividers, we know we have
 *	a potential table with (W-1) x (H-1) cells in it.
 *
 *	We represent this as a W x H grid of cells, each like:
 *
 *	        .       .
 *	        .       .
 *	   . . .+-------+. . .	Each cell holds information about the
 *	        |       .	edges above, and to the left of it.
 *	        |       .
 *	        |       .
 *	   . . .+. . . .+. . .
 *	        .       .
 *	        .       .
 *
 *	h_line: Is there a horizontal divider drawn on the page that
 *	corresponds to the top of this cell (i.e. is there a cell border
 *	here?)
 *	v_line: Is there a vertical divider drawn on the page that
 *	corresponds to the left of this cell (i.e. is there a cell border
 *	here?)
 *	h_crossed: Does content cross this line (i.e. are we merged
 *	with the cell above?)
 *	v_crossed: Does content cross this line (i.e. are we merged
 *	with the cell to the left?)
 *	full: Is there any content in this cell at all?
 *
 *	We need a W x H grid of cells to represent the entire table due
 *	to the potential right and bottom edge lines. The right and
 *	bottom rows of cells should never be full, or be crossed, but
 *	it's easiest just to use a simple representation that copes with
 *	the h_line and v_line values naturally.
 *
 *	So, we start with the cells structure empty, and we run through
 *	the page content, filling in the details as we go.
 *
 *	At the end of the process, we have enough information to draw
 *	an asciiart representation of our table. It might look something
 *	like this (this comes from dotted-gridlines-tables.pdf):
 *
 *	+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *	| | |   | | |#|#|#|#| | | |
 *	+ + + + + + +v+ +v+v+ + + +
 *	| | |   | | |#|#|#|#| | | |
 *	+ + + + + + + +v+ + + + + +
 *	| |#|   | |#|#|#|#|#|#|#|#|
 *	+ +v+ + + +v+v+ +v+v+v+v+v+
 *	| |#|   |#|#|#|#|#|#|#|#|#|
 *	+ + + + +v+ + +v+ + + + + +
 *	|#|#|  #|#|#|#|#|#|#|#|#|#|
 *	+v+v+ +v+ +v+v+ +v+v+v+v+v+
 *	|#|#|  #|#|#|#|#|#|#|#|#|#|
 *	+ + + + +v+ + +v+ + + + + +
 *	| |#|   |#|#|#|#|#|#|#|#|#|
 *	+ +v+ + + +v+v+ +v+v+v+v+v+
 *	| |#|   | |#|#|#|#|#|#|#|#|
 *	+ + + + + + + +v+ + + + + +
 *	| | |   | | |#|#|#|#| | | |
 *	+ + + + + + +v+ +v+v+ + + +
 *	| | |   | | |#|#|#|#| | | |
 *	+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#  |#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#  |#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#>#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#>#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#>#|#| |#| | | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#  |#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#  |#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#  |#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#  |#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *	  |#    |#|#|#|#|#|#|#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *	This shows where lines are detected ( - and | ),
 *	where they are crossed ( > and v) and where cells
 *	are full ( # ).
 *
 * Step 4: Row and column merging.
 *
 *	Based on the information above, we then try to merge
 *	cells and columns to simplify the table.
 *
 *	The best rules I've come up with this so far are:
 *	We can merge two adjacent columns if all the pairs of
 *	cells in the two columns are mergeable.
 *
 *	Cells are held to be mergable or not based upon the following
 *	rules:
 *		If there is a line between 2 cells - not mergeable.
 *		else if the uncertainty between 2 cells is 0 - not mergeable.
 *		else if the line between the 2 cells is crossed - mergeable.
 *		else if strictly one of the cells is full - mergeable.
 *		else not mergeable.
 *
 *	So in the above example, column 2 (numbered from 0) can be merged
 *	with column 3.
 *
 *	This gives:
 *
 *	+-+-+-+-+-+-+-+-+-+-+-+-+
 *	| | | | | |#|#|#|#| | | |
 *	+ + + + + +v+ +v+v+ + + +
 *	| | | | | |#|#|#|#| | | |
 *	+ + + + + + +v+ + + + + +
 *	| |#| | |#|#|#|#|#|#|#|#|
 *	+ +v+ + +v+v+ +v+v+v+v+v+
 *	| |#| |#|#|#|#|#|#|#|#|#|
 *	+ + + +v+ + +v+ + + + + +
 *	|#|#|#|#|#|#|#|#|#|#|#|#|
 *	+v+v+v+ +v+v+ +v+v+v+v+v+
 *	|#|#|#|#|#|#|#|#|#|#|#|#|
 *	+ + + +v+ + +v+ + + + + +
 *	| |#| |#|#|#|#|#|#|#|#|#|
 *	+ +v+ + +v+v+ +v+v+v+v+v+
 *	| |#| | |#|#|#|#|#|#|#|#|
 *	+ + + + + + +v+ + + + + +
 *	| | | | | |#|#|#|#| | | |
 *	+ + + + + +v+ +v+v+ + + +
 *	| | | | | |#|#|#|#| | | |
 *	+-+-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#|#| |#| | | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *	  | |#|#| | |#| | |#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *	  |#  |#|#|#|#|#|#|#|#|#|
 *	+ +-+-+-+-+-+-+-+-+-+-+-+
 *
 *	We then perform the same merging process for rows as for
 *	columns - though there are no rows in the above example
 *	that can be merged.
 *
 *	You'll note that, for example, we don't merge row 0 and
 *	row 1 in the above, because we have a pair of cells that
 *	are both full without crossing.
 *
 * Step 5: Cell spanning
 *
 *	Now we actually start to output the table. We keep a 'sent_table'
 *	(a grid of W x H bools) to keep track of whether we've output
 *	the content for a given cell or not yet.
 *
 *	For each cell we reach, assuming sent_table[x,y] is false,
 *	we merge it with as many cells on the right as required,
 *	according to 'v_crossed' values (subject to not passing
 *	v_lines or uncertainty == 0's).
 *
 *	We then try to merge cells below according to 'h_crossed'
 *	values (subject to not passing h_lines or uncertainty == 0's).
 *
 *	In theory this can leave us with some cases where we'd like
 *	to merge some cells (because of crossed) and can't (because
 *	of lines or sent_table[]) values. In the absence of better
 *	cell spanning algorithms we have no choice here.
 *
 *	Then we output the contents and set sent_table[] values as
 *	appropriate.
 *
 *	If a row has no cells in it, we currently omit the TR. If/when
 *	we figure out how to indicate rowspan/colspan in stext, we can
 *	revisit that.
 */


static fz_stext_block *
add_grid_block(fz_context *ctx, fz_stext_page *page, fz_stext_block **first_block)
{
	fz_stext_block *block = fz_pool_alloc(ctx, page->pool, sizeof(**first_block));
	memset(block, 0, sizeof(*block));
	block->type = FZ_STEXT_BLOCK_GRID;
	block->bbox = fz_empty_rect; /* Fixes bug 703267. */
	block->next = *first_block;
	if (*first_block)
		(*first_block)->prev = block;
	*first_block = block;
	return block;
}

static void
insert_block_before(fz_stext_block *block, fz_stext_block *before, fz_stext_page *page, fz_stext_struct *dest)
{
	if (before)
	{
		/* We have a block to insert it before, so we know it's not the last. */
		block->next = before;
		block->prev = before->prev;
		if (before->prev)
			before->prev->next = block;
		else if (dest)
			dest->first_block = block;
		else
			page->first_block = block;
		before->prev = block;
	}
	else if (dest)
	{
		/* Will be the last block. */
		block->next = NULL;
		block->prev = dest->last_block;
		if (dest->last_block)
			dest->last_block->next = block;
		if (dest->first_block == NULL)
			dest->first_block = block;
		dest->last_block = block;
	}
	else
	{
		/* Will be the last block. */
		block->next = NULL;
		block->prev = page->last_block;
		if (page->last_block)
			page->last_block->next = block;
		if (page->first_block)
			page->first_block = block;
		page->last_block = block;
	}
}

static fz_stext_struct *
add_struct_block_before(fz_context *ctx, fz_stext_block *before, fz_stext_page *page, fz_stext_struct *parent, fz_structure std, const char *raw)
{
	fz_stext_block *block;
	int idx = 0;
	size_t z;
	fz_stext_struct *newstruct;

	if (raw == NULL)
		raw = "";
	z = strlen(raw);

	/* We're going to insert a struct block. We need an idx, so walk the list */
	for (block = parent ? parent->first_block : page->first_block; block != before; block = block->next)
	{
		if (block->type == FZ_STEXT_BLOCK_STRUCT)
		{
			assert(block->u.s.index >= idx);
			idx = block->u.s.index + 1;
		}
	}
	/* So we'll add our block as idx. But all the other struct blocks that follow us need to have
	 * larger values. */

	/* Update all the subsequent structs to have a higher idx */
	if (before)
	{
		int idx2 = idx+1;
		for (block = before->next; block != NULL; block = block->next)
		{
			if (block->type != FZ_STEXT_BLOCK_STRUCT)
				continue;
			if (block->u.s.index > idx2)
				break;
			block->u.s.index = idx2++;
		}
	}

	/* Now make our new struct block and insert it. */
	block = fz_pool_alloc(ctx, page->pool, sizeof(*block));
	block->type = FZ_STEXT_BLOCK_STRUCT;
	block->bbox = fz_empty_rect; /* Fixes bug 703267. */
	insert_block_before(block, before, page, parent);

	block->u.s.down = newstruct = fz_pool_alloc(ctx, page->pool, sizeof(*block->u.s.down) + z);
	block->u.s.index = idx;
	newstruct->parent = parent;
	newstruct->standard = std;
	memcpy(newstruct->raw, raw, z);
	newstruct->raw[z] = 0;
	newstruct->up = block;

	return newstruct;
}

typedef struct
{
	int len;
	int max;
	struct {
		int left;
		float pos;
		int freq;
	} *list;
} div_list;

static void
div_list_push(fz_context *ctx, div_list *div, int left, float pos)
{
	int i;

	/* FIXME: Could be bsearch. */
	for (i = 0; i < div->len; i++)
	{
		if (div->list[i].pos > pos)
			break;
		else if (div->list[i].pos == pos && div->list[i].left == left)
		{
			div->list[i].freq++;
			return;
		}
	}

	if (div->len == div->max)
	{
		int newmax = div->max * 2;
		if (newmax == 0)
			newmax = 32;
		div->list = fz_realloc(ctx, div->list, sizeof(div->list[0]) * newmax);
		div->max = newmax;
	}

	if (i < div->len)
		memmove(&div->list[i+1], &div->list[i], sizeof(div->list[0]) * (div->len - i));
	div->len++;
	div->list[i].left = left;
	div->list[i].pos = pos;
	div->list[i].freq = 1;
}

static fz_stext_grid_positions *
make_table_positions(fz_context *ctx, div_list *xs, float min, float max)
{
	int wind;
	fz_stext_grid_positions *pos;
	int len = xs->len;
	int i;
	int hi = 0;

	/* Count the number of edges */
	int local_min = 0;
	int edges = 2;

	if (len == 0)
		return NULL;

	assert(xs->list[0].left);
	for (i = 0; i < len; i++)
	{
		if (xs->list[i].left)
		{
			if (local_min)
				edges++;
		}
		else
			local_min = 1;
	}
	assert(!xs->list[i-1].left);

	pos = fz_calloc(ctx, 1, sizeof(*pos) + (edges-1) * sizeof(pos->list[0]));
	pos->len = edges;

	/* Copy the edges in */
	wind = 0;
	local_min = 0;
	edges = 1;
	pos->list[0].pos = xs->list[0].pos;
	pos->list[0].min = min;
	pos->list[0].max = pos->list[0].pos;
	pos->list[0].uncertainty = 0;
#ifdef DEBUG_TABLE_HUNT
	printf("|%g ", post->list[0].pos);
#endif
	for (i = 0; i < len; i++)
	{
		if (xs->list[i].left)
		{
			if (local_min)
			{
				pos->list[edges].min = xs->list[i-1].pos;
				pos->list[edges].max = xs->list[i].pos;
				pos->list[edges].pos = (xs->list[i-1].pos + xs->list[i].pos)/2;
				pos->list[edges++].uncertainty = wind;
#ifdef DEBUG_TABLE_HUNT
				if (wind)
					printf("?%g(%d) ", post->list[0].pos, wind);
				else
					printf("|%g ", post->list[0].pos);
#endif
			}
			wind += xs->list[i].freq;
			if (wind > hi)
				hi = wind;
		}
		else
		{
			wind -= xs->list[i].freq;
			local_min = 1;
		}
	}
	assert(wind == 0);
	pos->list[edges].pos = xs->list[i-1].pos;
	pos->list[edges].min = xs->list[i-1].pos;
	pos->list[edges].max = max;
	pos->list[edges].uncertainty = 0;
	pos->max_uncertainty = hi;
#ifdef DEBUG_TABLE_HUNT
	printf("|%g\n", post->list[edges].pos);
#endif

	return pos;
}

static fz_stext_grid_positions *
clone_grid_positions(fz_context *ctx, fz_stext_page *page, fz_stext_grid_positions *xs)
{
	size_t z = sizeof(*xs) + (xs->len-1) * sizeof(xs->list[0]);
	fz_stext_grid_positions *xs2 = fz_pool_alloc(ctx, page->pool, z);

	memcpy(xs2, xs, z);

	return xs2;
}

static void
sanitize_positions(fz_context *ctx, div_list *xs)
{
	int i, j;

#ifdef DEBUG_TABLE_HUNT
	printf("OK:\n");
	for (i = 0; i < xs->len; i++)
	{
		if (xs->list[i].left)
			printf("[");
		printf("%g(%d)", xs->list[i].pos, xs->list[i].freq);
		if (!xs->list[i].left)
			printf("]");
		printf(" ");
	}
	printf("\n");
#endif

	/* Now, combine runs of left and right */
	for (i = 0; i < xs->len; i++)
	{
		if (xs->list[i].left)
		{
			j = i;
			while (i < xs->len-1 && xs->list[i+1].left)
			{
				i++;
				xs->list[j].freq += xs->list[i].freq;
				xs->list[i].freq = 0;
			}
		}
		else
		{
			while (i < xs->len-1 && !xs->list[i+1].left)
			{
				i++;
				xs->list[i].freq += xs->list[i-1].freq;
				xs->list[i-1].freq = 0;
			}
		}
	}

#ifdef DEBUG_TABLE_HUNT
	printf("Shrunk:\n");
	for (i = 0; i < xs->len; i++)
	{
		if (xs->list[i].left)
			printf("[");
		printf("%g(%d)", xs->list[i].pos, xs->list[i].freq);
		if (!xs->list[i].left)
			printf("]");
		printf(" ");
	}
	printf("\n");
#endif

	/* Now remove the 0 frequency ones. */
	j = 0;
	for (i = 0; i < xs->len; i++)
	{
		if (xs->list[i].freq == 0)
			continue;
		if (i != j)
			xs->list[j] = xs->list[i];
		j++;
	}
	xs->len = j;

#ifdef DEBUG_TABLE_HUNT
	printf("Compacted:\n");
	for (i = 0; i < xs->len; i++)
	{
		if (xs->list[i].left)
			printf("[");
		printf("%g(%d)", xs->list[i].pos, xs->list[i].freq);
		if (!xs->list[i].left)
			printf("]");
		printf(" ");
	}
	printf("\n");
#endif
}

static void
walk_blocks(fz_context *ctx, div_list *xs, div_list *ys, fz_stext_block *first_block, int descend)
{
	fz_stext_block *block;
	fz_stext_line *line;
	fz_stext_char *ch;

	for (block = first_block; block != NULL; block = block->next)
	{
		switch (block->type)
		{
		case FZ_STEXT_BLOCK_STRUCT:
			if (descend && block->u.s.down)
				walk_blocks(ctx, xs, ys, block->u.s.down->first_block, descend);
			break;
		case FZ_STEXT_BLOCK_VECTOR:
			break;
		case FZ_STEXT_BLOCK_TEXT:
			for (line = block->u.t.first_line; line != NULL; line = line->next)
			{
				float rpos;
				int left = 1;
				int right = 0;
				div_list_push(ctx, ys, 1, line->bbox.y0);
				div_list_push(ctx, ys, 0, line->bbox.y1);
				for (ch = line->first_char; ch != NULL; ch = ch->next)
				{
					if (ch->c == ' ')
					{
						if (ch->next == NULL)
						{
							/* This is a trailing space. We've seen cases where we get
							 * trailing spaces on cell contents and this screws stuff
							 * up (e.g. dotted-gridlines-tables.pdf). */
							if (right)
							{
								/* Send a 'right' as the left position of this space. */
								float lpos = fz_min(ch->quad.ll.x, ch->quad.ul.x);
								div_list_push(ctx, xs, 0, lpos);
								left = 1;
								right = 0;
							}
						}
						else if (ch->next->c == ' ')
						{
							/* Run of multiple spaces. Send a 'right' as the left position
							 * of this space, and then skip forwards. */
							if (right)
							{
								float lpos = fz_min(ch->quad.ll.x, ch->quad.ul.x);
								div_list_push(ctx, xs, 0, lpos);
								while (ch->next && ch->next->c == ' ')
									ch = ch->next;
								left = 1;
								right = 0;
							}
						}
						else
						{
							/* Ignore any other spaces. Don't start or end a run on them. */
						}
					}
					else
					{
						if (left)
						{
							float lpos = fz_min(ch->quad.ll.x, ch->quad.ul.x);
							div_list_push(ctx, xs, 1, lpos);
							left = 0;
						}
						rpos = fz_max(ch->quad.lr.x, ch->quad.ur.x);
						right = 1;
					}
				}
				if (right)
					div_list_push(ctx, xs, 0, rpos);
			}
			break;
		}
	}
}

/* One of our datastructures (cells_t) is an array of details about the
 * cells that make up our table. It's a w * h array of cell_t's. Each
 * cell contains data on one of the cells in the table, as you'd expect.
 *
 *     .       .
 *     .       .
 * - - +-------+ - -
 *     |       .
 *     |       .
 *     |       .
 * - - + - - - + - -
 *     .       .
 *     .       .
 *
 * For any given cell, we store details about the top (lowest y coord)
 * and left (lowest x coord) edges. Specifically we store whether
 * there is a line at this position (h_line and v_line) (i.e. a drawn
 * border), and we also store whether content crosses this edge (h_crossed
 * and y_crossed). Finally, we store whether the cell has any content
 * in it at all (full).
 *
 * A table which has w positions across and h positions vertically, will
 * only really have (w-1) * (h-1) cells. We store w*h though to allow for
 * the right and bottom edges to have their lines represented.
 */

typedef struct
{
	int h_line;
	int v_line;
	int h_crossed;
	int v_crossed;
	int full;
} cell_t;

typedef struct
{
	int w;
	int h;
	cell_t cell[1];
} cells_t;

typedef struct
{
	cells_t *cells;
	fz_stext_grid_positions *xpos;
	fz_stext_grid_positions *ypos;
} grid_walker_data;

static cell_t *
get_cell(cells_t *cells, int x, int y)
{
	return &cells->cell[x + y * cells->w];
}

static int
find_grid_pos_with_reinforcement(fz_context *ctx, fz_stext_grid_positions *pos, float x, int expand)
{
	int i;

	for (i = 0; i < pos->len; i++)
	{
		int r;
		if (x > pos->list[i].max)
			continue;
		if (x < pos->list[i].min)
		{
			if (expand && i > 0)
			{
				float mid = (pos->list[i].min + pos->list[i-1].max)/2;
				if (x < mid)
					return i-1;
				else
					return i;
			}
			return -1;
		}
		r = pos->list[i].reinforcement++;
		pos->list[i].pos = (pos->list[i].pos * r + x) / (r+1);
		return i;
	}

	return -1;
}

static int
find_cell(fz_stext_grid_positions *pos, float x)
{
	int i;

	for (i = 0; i < pos->len; i++)
		if (x < pos->list[i].pos)
			return i-1;
	if (x == pos->list[pos->len-1].pos)
		return pos->len-1;

	return -1;
}

static int
add_h_line(fz_context *ctx, grid_walker_data *gd, float x0, float x1, float y0, float y1)
{
	int start = find_grid_pos_with_reinforcement(ctx, gd->xpos, x0, 1);
	int end = find_grid_pos_with_reinforcement(ctx, gd->xpos, x1, 1);
	float y = (y0 + y1) / 2;
	int yidx = find_grid_pos_with_reinforcement(ctx, gd->ypos, y, 0);
	int i;

	if (start < 0 || end < 0 || yidx < 0 || start >= end)
		return 1;

	for (i = start; i < end; i++)
		get_cell(gd->cells, i, yidx)->h_line++;

	return 0;
}

static int
add_v_line(fz_context *ctx, grid_walker_data *gd, float y0, float y1, float x0, float x1)
{
	int start = find_grid_pos_with_reinforcement(ctx, gd->ypos, y0, 1);
	int end = find_grid_pos_with_reinforcement(ctx, gd->ypos, y1, 1);
	float x = (x0 + x1) / 2;
	int xidx = find_grid_pos_with_reinforcement(ctx, gd->xpos, x, 0);
	int i;

	if (start < 0 || end < 0 || xidx < 0 || start >= end)
		return 1;

	for (i = start; i < end; i++)
		get_cell(gd->cells, xidx, i)->v_line++;

	return 0;
}

static void
walk_grid_lines(fz_context *ctx, grid_walker_data *gd, fz_stext_block *block)
{
	for (; block != NULL; block = block->next)
	{
		if (block->type == FZ_STEXT_BLOCK_STRUCT)
		{
			if (block->u.s.down)
				walk_grid_lines(ctx, gd, block->u.s.down->first_block);
			continue;
		}
		else if (block->type == FZ_STEXT_BLOCK_VECTOR)
		{
			fz_rect r = block->bbox;
			float w = r.x1 - r.x0;
			float h = r.y1 - r.y0;
			int failed = 0;
			if (w > h && h < 1)
			{
				/* Thin, wide line */
				failed = add_h_line(ctx, gd, r.x0, r.x1, r.y0, r.y1);
			}
			else if (w < h && w < 1)
			{
				/* Thin, wide line */
				failed = add_v_line(ctx, gd, r.y0, r.y1, r.x0, r.x1);
			}
			else
			{
				/* Rectangle */
				int failed2;
				failed2 = add_h_line(ctx, gd, r.x0, r.x1, r.y0, r.y0);
				failed2 |= add_h_line(ctx, gd, r.x0, r.x1, r.y1, r.y1);
				failed = add_v_line(ctx, gd, r.y0, r.y1, r.x0, r.x0);
				failed |= add_v_line(ctx, gd, r.y0, r.y1, r.x1, r.x1);
				failed &= failed2;
			}
			if (failed)
			{
				/* Try merging multiple successive vectors to get better
				 * results. */
				if (w > h)
				{
					while (block->next != NULL &&
						block->next->type == FZ_STEXT_BLOCK_VECTOR &&
						block->next->bbox.y0 == r.y0 &&
						block->next->bbox.y1 == r.y1 &&
						(block->next->bbox.x0 < r.x1 + 1 || block->next->bbox.x1 > r.x0 - 1))
					{
						block = block->next;
						r = fz_union_rect(r, block->bbox);
					}
#ifdef DEBUG
					if (add_h_line(ctx, gd, r.x0, r.x1, r.y0, r.y1))
#endif
						add_h_line(ctx, gd, r.x0, r.x1, r.y0, r.y1);
				}
				else
				{
					while (block->next != NULL &&
						block->next->type == FZ_STEXT_BLOCK_VECTOR &&
						block->next->bbox.x0 == r.x0 &&
						block->next->bbox.x1 == r.x1 &&
						(block->next->bbox.y0 < r.y1 + 1 || block->next->bbox.y1 > r.y0 - 1))
					{
						block = block->next;
						r = fz_union_rect(r, block->bbox);
					}
#ifdef DEBUG
					if (add_v_line(ctx, gd, r.y0, r.y1, r.x0, r.x1))
#endif
						add_v_line(ctx, gd, r.y0, r.y1, r.x0, r.x1);
				}
			}
		}
	}
}

static void
erase_grid_lines(fz_context *ctx, grid_walker_data *gd, fz_stext_block *block)
{
	fz_rect bounds = {
		gd->xpos->list[0].pos,
		gd->ypos->list[0].pos,
		gd->xpos->list[gd->xpos->len-1].pos,
		gd->ypos->list[gd->ypos->len-1].pos };

	for (; block != NULL; block = block->next)
	{
		if (block->type == FZ_STEXT_BLOCK_STRUCT)
		{
			if (block->u.s.down)
				erase_grid_lines(ctx, gd, block->u.s.down->first_block);
			continue;
		}
		else if (block->type == FZ_STEXT_BLOCK_TEXT)
		{
			fz_stext_line *line;

			if (block->bbox.x0 >= bounds.x1 || block->bbox.y0 >= bounds.y1 ||
				block->bbox.x1 <= bounds.x0 || block->bbox.y1 <= bounds.y0)
				continue;

			for (line = block->u.t.first_line; line != NULL; line = line->next)
			{
				fz_stext_char *ch = line->first_char;

				/* Skip leading spaces */
				while (ch != NULL && ch->c == ' ')
					ch = ch->next;

				for (; ch != NULL; ch = ch->next)
				{
					fz_rect r;
					int x, y, x0, x1, y0, y1;

					if (ch->c == 32)
					{
						/* Trailing space, skip it. */
						if (ch->next == NULL)
							break;
						if (ch->next->c == 32)
						{
							/* Run of spaces. Skip 'em. */
							while (ch->next && ch->next->c == 32)
								ch = ch->next;
							continue;
						}
						/* A single space. Accept it. */
					}
					r = fz_rect_from_quad(ch->quad);
					x0 = find_cell(gd->xpos, r.x0);
					x1 = find_cell(gd->xpos, r.x1);
					y0 = find_cell(gd->ypos, r.y0);
					y1 = find_cell(gd->ypos, r.y1);
					if (x0 < 0 || x1 <0 || y0 < 0 || y1 < 0)
						continue;
					if (x0 < x1)
					{
						for (y = y0; y <= y1; y++)
							for (x = x0; x < x1; x++)
								get_cell(gd->cells, x+1, y)->v_crossed++;
					}
					if (y0 < y1)
					{
						for (y = y0; y < y1; y++)
							for (x = x0; x <= x1; x++)
								get_cell(gd->cells, x, y+1)->h_crossed++;
					}
					for (y = y0; y <= y1; y++)
						for (x = x0; x <= x1; x++)
							get_cell(gd->cells, x, y)->full++;
				}
			}
		}
	}
}

static cells_t *new_cells(fz_context *ctx, int w, int h)
{
	cells_t *cells = fz_calloc(ctx, 1, sizeof(cells_t) + sizeof(cells->cell[0]) * (w * h - 1));
	cells->w = w;
	cells->h = h;

	return cells;
}

#ifdef DEBUG_TABLE_STRUCTURE
static void
asciiart_table(grid_walker_data *gd)
{
	int w = gd->xpos->len;
	int h = gd->ypos->len;
	int x, y;

	for (y = 0; y < h; y++)
	{
		for (x = 0; x < w-1; x++)
		{
			cell_t *cell = get_cell(gd->cells, x, y);
			int line = cell->h_line;
			int erase = cell->h_crossed;
			printf("+");
			if (line && !erase)
			{
				printf("-");
			}
			else if (!line && erase)
			{
				printf("v");
			}
			else if (line && erase)
			{
				printf("*");
			}
			else
			{
				printf(" ");
			}
		}
		printf("+\n");
		if (y == h-1)
			break;
		for (x = 0; x < w; x++)
		{
			cell_t *cell = get_cell(gd->cells, x, y);
			int line = cell->v_line;
			int erase = cell->v_crossed;
			if (line && !erase)
			{
				printf("|");
			}
			else if (!line && erase)
			{
				printf(">");
			}
			else if (line && erase)
			{
				printf("*");
			}
			else
			{
				printf(" ");
			}
			if (x < w-1)
			{
				if (cell->full)
					printf("#");
				else
					printf(" ");
			}
			else
				printf("\n");
		}
	}
}
#endif

static void
recalc_bbox(fz_stext_block *block)
{
	fz_rect bbox = fz_empty_rect;
	fz_stext_line *line;

	for (line = block->u.t.first_line; line != NULL; line = line->next)
		bbox = fz_union_rect(bbox, line->bbox);

	block->bbox = bbox;
}

static void
unlink_line_from_block(fz_stext_line *line, fz_stext_block *block)
{
	fz_stext_line *next_line = line->next;

	if (line->prev)
		line->prev->next = next_line;
	else
		block->u.t.first_line = next_line;
	if (next_line)
		next_line->prev = line->prev;
	else
		block->u.t.last_line = line->prev;
}

static void
append_line_to_block(fz_stext_line *line, fz_stext_block *block)
{
	if (block->u.t.last_line == NULL)
	{
		block->u.t.first_line = block->u.t.last_line = line;
		line->prev = NULL;
	}
	else
	{
		line->prev = block->u.t.last_line;
		block->u.t.last_line->next = line;
		block->u.t.last_line = line;
	}
	line->next = NULL;
}

static void
unlink_block(fz_stext_block *block, fz_stext_block **first, fz_stext_block **last)
{
	if (block->prev)
		block->prev->next = block->next;
	else
		*first = block->next;
	if (block->next)
		block->next->prev = block->prev;
	else
		*last = block->prev;
}

static fz_rect
move_contained_content(fz_context *ctx, fz_stext_page *page, fz_stext_struct *dest, fz_stext_struct *src, fz_rect r)
{
	fz_stext_block *before = dest ? dest->first_block : page->first_block;
	fz_stext_block **sfirst = src ? &src->first_block : &page->first_block;
	fz_stext_block **slast = src ? &src->last_block : &page->last_block;
	fz_stext_block *block, *next;

	for (block = *sfirst; block != NULL; block = next)
	{
		fz_rect bbox = fz_intersect_rect(block->bbox, r);
		next = block->next;
		/* Don't use fz_is_empty_rect here, as that will exclude zero height areas like spaces. */
		if (bbox.x0 > bbox.x1 || bbox.y0 > bbox.y1)
			continue; /* Trivially excluded */
		if (bbox.x0 == block->bbox.x0 && bbox.y0 == block->bbox.y0 && bbox.x1 == block->bbox.x1 && bbox.y1 == block->bbox.y1)
		{
			/* Trivially included */
			unlink_block(block, sfirst, slast);
			insert_block_before(block, before, page, dest);
			before = block->next;
			continue;
		}
		if (block->type == FZ_STEXT_BLOCK_TEXT)
		{
			/* Partially included text block */
			fz_stext_line *line, *next_line;
			fz_stext_block *newblock = NULL;

			for (line = block->u.t.first_line; line != NULL; line = next_line)
			{
				fz_rect lrect = fz_intersect_rect(line->bbox, r);
				next_line = line->next;

				/* Don't use fz_is_empty_rect here, as that will exclude zero height areas like spaces. */
				if (lrect.x0 > lrect.x1 || lrect.y0 > lrect.y1)
					continue; /* Trivial exclusion */
				if (line->bbox.x0 == lrect.x0 && line->bbox.y0 == lrect.y0 && line->bbox.x1 == lrect.x1 && line->bbox.y1 == lrect.y1)
				{
					/* Trivial inclusion */
					if (newblock == NULL)
					{
						newblock = fz_pool_alloc(ctx, page->pool, sizeof(fz_stext_block));
						insert_block_before(newblock, before, page, dest);
						before = newblock->next;
					}

					unlink_line_from_block(line, block);
					append_line_to_block(line, newblock);
				}
				else
				{
					/* Need to walk the line and just take parts */
					fz_stext_line *newline = NULL;
					fz_stext_char *ch, *next_ch, *prev_ch = NULL;

					for (ch = line->first_char; ch != NULL; ch = next_ch)
					{
						fz_rect crect = fz_rect_from_quad(ch->quad);
						float x = (crect.x0 + crect.x1)/2;
						float y = (crect.y0 + crect.y1)/2;
						next_ch = ch->next;
						if (r.x0 > x || r.x1 < x || r.y0 > y || r.y1 < y)
						{
							prev_ch = ch;
							continue;
						}
						/* Take this char */
						if (newline == NULL)
						{
							newline = fz_pool_alloc(ctx, page->pool, sizeof(*newline));
							newline->dir = line->dir;
							newline->wmode = line->wmode;
							newline->bbox = fz_empty_rect;
						}
						/* Unlink char */
						if (prev_ch == NULL)
							line->first_char = next_ch;
						else
							prev_ch->next = next_ch;
						if (next_ch == NULL)
							line->last_char = prev_ch;
						/* Relink char */
						ch->next = NULL;
						if (newline->last_char == NULL)
							newline->first_char = ch;
						else
							newline->last_char->next = ch;
						newline->last_char = ch;
						newline->bbox = fz_union_rect(newline->bbox, crect);
					}
					if (newline)
					{
						if (newblock == NULL)
						{
							newblock = fz_pool_alloc(ctx, page->pool, sizeof(fz_stext_block));

							/* Add the block onto our target list */
							insert_block_before(newblock, before, page, dest);
							before = newblock->next;
						}
						append_line_to_block(newline, newblock);
					}
				}
			}
			if (newblock)
			{
				recalc_bbox(block);
				recalc_bbox(newblock);
			}
		}
	}

	return r;
}

static fz_stext_block *
find_table_insertion_point(fz_context *ctx, fz_rect r, fz_stext_block *block)
{
	fz_stext_block *after = NULL;

	for (; block != NULL; block = block->next)
	{
		fz_rect s = fz_intersect_rect(r, block->bbox);

		if (s.x0 > s.x1 || s.y0 > s.y1)
			continue;
		after = block;
	}

	/* Convert to before */
	if (after)
		after = after->next;

	return after;
}

static fz_stext_struct *
transcribe_table(fz_context *ctx, grid_walker_data *gd, fz_stext_page *page, fz_stext_struct *parent)
{
	int w = gd->xpos->len;
	int h = gd->ypos->len;
	int x, y;
	char *sent_tab = fz_calloc(ctx, 1, w*h);
	fz_stext_block **first_block = parent ? &parent->first_block : &page->first_block;
	fz_stext_struct *table, *tr, *td;
	fz_stext_block *before;
	fz_rect r;

	/* Where should we insert the table in the data? */
	r.x0 = gd->xpos->list[0].pos;
	r.x1 = gd->xpos->list[w-1].pos;
	r.y0 = gd->ypos->list[0].pos;
	r.y1 = gd->ypos->list[h-1].pos;
	before = find_table_insertion_point(ctx, r, *first_block);

	/* Make table */
	table = add_struct_block_before(ctx, before, page, parent, FZ_STRUCTURE_TABLE, "Table");

	/* Run through the cells, and guess at spanning. */
	for (y = 0; y < h-1; y++)
	{
		/* Have we sent this entire row before? */
		for (x = 0; x < w-1; x++)
		{
			if (!sent_tab[x+y*w])
				break;
		}
		if (x == w-1)
			continue; /* No point in sending a row with nothing in it! */

		/* Make TR */
		tr = add_struct_block_before(ctx, NULL, page, table, FZ_STRUCTURE_TR, "TR");

		for (x = 0; x < w-1; x++)
		{
			int x2, y2;
			int cellw = 1;
			int cellh = 1;

			/* Have we sent this cell already? */
			if (sent_tab[x+y*w])
				continue;

			/* Find the width of the cell */
			for (x2 = x+1; x2 < w-1; x2++)
			{
				cell_t *cell = get_cell(gd->cells, x2, y);
				if (cell->v_line)
					break; /* Can't go past a line */
				if (gd->xpos->list[x2].uncertainty == 0)
					break; /* An uncertainty of 0 is as good as a line. */
				if (!cell->v_crossed)
					break;
				cellw++;
			}
			/* Find the height of the cell */
			for (y2 = y+1; y2 < h-1; y2++)
			{
				cell_t *cell;
				int h_crossed = 0;
				if (gd->ypos->list[y2].uncertainty == 0)
					break; /* An uncertainty of 0 is as good as a line. */

				cell = get_cell(gd->cells, x, y2);
				if (cell->h_line)
					break; /* Can't extend down through a line. */
				if (cell->h_crossed)
					h_crossed = 1;
				for (x2 = x+1; x2 < x+cellw; x2++)
				{
					cell_t *cell = get_cell(gd->cells, x2, y2);
					if (cell->h_line)
						break;
					if (cell->v_line)
						break; /* Can't go past a line */
					if (gd->xpos->list[x2].uncertainty == 0)
						break; /* An uncertainty of 0 is as good as a line. */
					if (!cell->v_crossed)
						break;
					if (cell->h_crossed)
						h_crossed = 1;
				}
				if (x2 == x+cellw && h_crossed)
					cellh++;
				else
					break;
			}
			/* Make TD */
			td = add_struct_block_before(ctx, NULL, page, tr, FZ_STRUCTURE_TD, "TD");
			r.x0 = gd->xpos->list[x].pos;
			r.x1 = gd->xpos->list[x+cellw].pos;
			r.y0 = gd->ypos->list[y].pos;
			r.y1 = gd->ypos->list[y+cellh].pos;
			/* Use r, not REAL contents bbox, as otherwise spanned rows
			 * can end up empty. */
			td->up->bbox = r;
			move_contained_content(ctx, page, td, parent, r);
#ifdef DEBUG_TABLE_STRUCTURE
			printf("(%d,%d) + (%d,%d)\n", x, y, cellw, cellh);
#endif
			for (y2 = y; y2 < y+cellh; y2++)
				for (x2 = x; x2 < x+cellw; x2++)
					sent_tab[x2+y2*w] = 1;
		}
		r.x0 = gd->xpos->list[0].pos;
		r.x1 = gd->xpos->list[gd->xpos->len-1].pos;
		r.y0 = gd->ypos->list[y].pos;
		r.y1 = gd->ypos->list[y+1].pos;
		tr->up->bbox = r;
		table->up->bbox = fz_union_rect(table->up->bbox, tr->up->bbox);
	}
	fz_free(ctx, sent_tab);

	return table;
}

static void
merge_column(grid_walker_data *gd, int x)
{
	int y;
	for (y = 0; y < gd->cells->h; y++)
	{
		cell_t *d = &gd->cells->cell[x + y * (gd->cells->w-1)];
		cell_t *s = &gd->cells->cell[x + y * gd->cells->w];

		if (x > 0)
			memcpy(d-x, s-x, x * sizeof(*d));
		d->full = s[0].full || s[1].full;
		d->h_crossed = s[0].h_crossed || s[1].h_crossed;
		d->h_line = s[0].h_line; /* == s[1].h_line */
		d->v_crossed = s[0].v_crossed;
		d->v_line = s[0].v_line;
		if (x < gd->cells->w - 2)
			memcpy(d+1, s+2, (gd->cells->w - 2 - x) * sizeof(*d));
	}
	gd->cells->w--;

	if (x < gd->xpos->len - 2)
		memcpy(&gd->xpos->list[x+1], &gd->xpos->list[x+2], (gd->xpos->len - 2 - x) * sizeof(gd->xpos->list[0]));
	gd->xpos->len--;
}

static void
merge_columns(grid_walker_data *gd)
{
	int x, y;

	for (x = gd->cells->w-3; x >= 0; x--)
	{
		/* Can column x be merged with column x+1? */
		/* This requires all the pairs of cells in those 2 columns to be mergeable. */
		for (y = 0; y < gd->cells->h-1; y++)
		{
			cell_t *a = get_cell(gd->cells, x, y);
			cell_t *b = get_cell(gd->cells, x+1, y);
			/* If there is a divider, we can't merge. */
			if (b->v_line)
				break;
			/* If either is empty, we can merge. */
			if (!a->full || !b->full)
				continue;
			/* If we differ in h linedness, we can't merge */
			if (!!a->h_line != !!b->h_line)
				break;
			/* If both are full, we can only merge if we cross. */
			if (a->full && b->full && b->v_crossed)
				continue;
			/* Otherwise we can't merge */
			break;
		}
		if (y == gd->cells->h-1)
		{
			/* Merge the column! */
#ifdef DEBUG_TABLE_STRUCTURE
			printf("Merging column %d\n", x);
#endif
			merge_column(gd, x);
#ifdef DEBUG_TABLE_STRUCTURE
			asciiart_table(gd);
#endif
		}
	}
}

static void
merge_row(grid_walker_data *gd, int y)
{
	int x;
	int w = gd->cells->w;
	cell_t *d = &gd->cells->cell[y * w];
	for (x = 0; x < gd->cells->w-1; x++)
	{
		if (d->full == 0)
			d->full = d[w].full;
		if (d->h_crossed == 0)
			d->h_crossed = d[w].h_crossed;
		d++;
	}
	if (y < gd->cells->h - 2)
		memcpy(d, d+w, (gd->cells->h - 2 - y) * w * sizeof(*d));
	gd->cells->h--;

	if (y < gd->ypos->len - 2)
		memcpy(&gd->ypos->list[y+1], &gd->ypos->list[y+2], (gd->ypos->len - 2 - y) * sizeof(gd->ypos->list[0]));
	gd->ypos->len--;
}

static void
merge_rows(grid_walker_data *gd)
{
	int x, y;

	for (y = gd->cells->h-3; y >= 0; y--)
	{
		/* Can row y be merged with row y+1? */
		/* This requires all the pairs of cells in those 2 rows to be mergeable. */
		for (x = 0; x < gd->cells->w-1; x++)
		{
			cell_t *a = get_cell(gd->cells, x, y);
			cell_t *b = get_cell(gd->cells, x, y+1);
			/* If there is a divider, we can't merge. */
			if (b->h_line)
				break;
			/* If either is empty, we can merge. */
			if (!a->full || !b->full)
				continue;
			/* If we differ in v linedness, we can't merge */
			if (!!a->v_line != !!b->v_line)
				break;
			/* If both are full, we can only merge if we cross. */
			if (a->full && b->full && b->h_crossed)
				continue;
			/* Otherwise we can't merge */
			break;
		}
		if (x == gd->cells->w-1)
		{
			/* Merge the row! */
#ifdef DEBUG_TABLE_STRUCTURE
			printf("Merging row %d\n", y);
#endif
			merge_row(gd, y);
#ifdef DEBUG_TABLE_STRUCTURE
			asciiart_table(gd);
#endif
		}
	}
}

static fz_stext_struct *
check_for_grid_lines(fz_context *ctx, fz_stext_grid_positions *xps, fz_stext_grid_positions *yps, fz_stext_page *page, fz_stext_struct *parent)
{
	fz_stext_block **first_blockp = parent ? &parent->first_block : &page->first_block;
	grid_walker_data gd = { 0 };
	fz_stext_struct *table = NULL;

	gd.xpos = xps;
	gd.ypos = yps;

	fz_var(gd);

	fz_try(ctx)
	{
		gd.cells = new_cells(ctx, xps->len, yps->len);

		/* First we walk the content looking for grid lines. These
		 * lines refine our positions. */
		walk_grid_lines(ctx, &gd, *first_blockp);
		/* Now, we walk the content looking for content that crosses
		 * these grid lines. This allows us to spot spanned cells. */
		erase_grid_lines(ctx, &gd, *first_blockp);

#ifdef DEBUG_TABLE_STRUCTURE
		asciiart_table(&gd);
#endif
		/* Now, can we remove some columns or rows? i.e. have we oversegmented? */
		merge_columns(&gd);
		merge_rows(&gd);

		/* Did we shrink the table so much it's not a table any more? */
		if (gd.xpos->len < 3 || gd.ypos->len < 3)
			break;

		/* Now we should have the entire table calculated. */
		table = transcribe_table(ctx, &gd, page, parent);
	}
	fz_always(ctx)
	{
		fz_free(ctx, gd.cells);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);

	return table;
}

static fz_rect
bbox_of_blocks(fz_stext_block *block)
{
	fz_rect r = fz_empty_rect;

	while (block)
	{
		r = fz_union_rect(r, block->bbox);
		block = block->next;
	}

	return r;
}

static void
do_table_hunt(fz_context *ctx, fz_stext_page *page, fz_stext_struct *parent)
{
	div_list xs = { 0 };
	div_list ys = { 0 };
	fz_stext_block *block;
	int count;
	fz_stext_block **first_block = parent ? &parent->first_block : &page->first_block;
	fz_stext_grid_positions *xps = NULL;
	fz_stext_grid_positions *yps = NULL;

	/* No content? Just bale. */
	if (*first_block == NULL)
		return;

	/* First off, descend into any children to see if those look like tables. */
	count = 0;
	for (block = *first_block; block != NULL; block = block->next)
	{
		if (block->type == FZ_STEXT_BLOCK_STRUCT)
		{
			if (block->u.s.down)
			{
				do_table_hunt(ctx, page, block->u.s.down);
				count++;
			}
		}
		else if (block->type == FZ_STEXT_BLOCK_TEXT)
			count++;
	}

	/* If all we have is a single child, no more to hunt. */
	if (count <= 1)
		return;

	fz_var(xps);
	fz_var(yps);

	fz_try(ctx)
	{
		/* Now see whether the content looks like tables.
		 * Currently, we pass descend == 0, which means we only consider content at
		 * this level. If we pass 1, then we'll consider all the content at this
		 * level, plus the children. This might allow for where we have oversegmented,
		 * but really needs us to fixup the content. */
		walk_blocks(ctx, &xs, &ys, *first_block, 0);

		sanitize_positions(ctx, &xs);
		sanitize_positions(ctx, &ys);

		/* Run across the line, counting 'winding' */
		if (xs.len > 2 && ys.len > 2)
		{
			fz_stext_struct *table;
			fz_rect rect = bbox_of_blocks(*first_block);
			xps = make_table_positions(ctx, &xs, rect.x0, rect.x1);
			yps = make_table_positions(ctx, &ys, rect.y0, rect.y1);
			table = check_for_grid_lines(ctx, xps, yps, page, parent);

			if (table != NULL)
			{
				fz_stext_block *block;
				fz_stext_grid_positions *xps2 = clone_grid_positions(ctx, page, xps);
				fz_stext_grid_positions *yps2 = clone_grid_positions(ctx, page, yps);
				block = add_grid_block(ctx, page, &table->first_block);
				block->u.b.xs = xps2;
				block->u.b.ys = yps2;
				block->bbox.x0 = block->u.b.xs->list[0].pos;
				block->bbox.y0 = block->u.b.ys->list[0].pos;
				block->bbox.x1 = block->u.b.xs->list[block->u.b.xs->len-1].pos;
				block->bbox.y1 = block->u.b.ys->list[block->u.b.ys->len-1].pos;
			}
#ifdef DEBUG_WRITE_AS_PS
			{
				int i;
				printf("%% TABLE\n");
				for (i = 0; i < block->u.b.xs->len; i++)
				{
					if (block->u.b.xs->list[i].uncertainty)
						printf("0 1 0 setrgbcolor\n");
					else
						printf("0 0.5 0 setrgbcolor\n");
					printf("%g %g moveto %g %g lineto stroke\n",
						block->u.b.xs->list[i].pos, block->bbox.y0,
						block->u.b.xs->list[i].pos, block->bbox.y1);
				}
				for (i = 0; i < block->u.b.ys->len; i++)
				{
					if (block->u.b.ys->list[i].uncertainty)
						printf("0 1 0 setrgbcolor\n");
					else
						printf("0 0.5 0 setrgbcolor\n");
					printf("%g %g moveto %g %g lineto stroke\n",
						block->bbox.x0, block->u.b.ys->list[i].pos,
						block->bbox.x1, block->u.b.ys->list[i].pos);
				}
			}
#endif
		}
	}
	fz_always(ctx)
	{
		fz_free(ctx, xs.list);
		fz_free(ctx, ys.list);
		fz_free(ctx, xps);
		fz_free(ctx, yps);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

void
fz_table_hunt(fz_context *ctx, fz_stext_page *page)
{
	if (page == NULL)
		return;

	do_table_hunt(ctx, page, NULL);
}
