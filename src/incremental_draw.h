/**
 Reimplementation of 
 https://github.com/ArtifexSoftware/mupdf/blob/master/source/fitz/list-device.c
 */
#ifndef INCREMENTAL_DRAW_H
#define INCREMENTAL_DRAW_H

#include <mupdf/fitz.h>

#define SIZE_IN_NODES(t) \
	((t + sizeof(fz_display_node) - 1) / sizeof(fz_display_node))

enum {
	CS_UNCHANGED = 0,
	CS_GRAY_0    = 1,
	CS_GRAY_1    = 2,
	CS_RGB_0     = 3,
	CS_RGB_1     = 4,
	CS_CMYK_0    = 5,
	CS_CMYK_1    = 6,
	CS_OTHER_0   = 7,

	ALPHA_UNCHANGED = 0,
	ALPHA_1         = 1,
	ALPHA_0         = 2,
	ALPHA_PRESENT   = 3,

	CTM_UNCHANGED = 0,
	CTM_CHANGE_AD = 1,
	CTM_CHANGE_BC = 2,
	CTM_CHANGE_EF = 4,

	INDIRECT_NODE_THRESHOLD = (1<<9)-1
};

typedef struct
{
	unsigned int cmd    : 5;
	unsigned int size   : 9;
	unsigned int rect   : 1;
	unsigned int path   : 1;
	unsigned int cs     : 3;
	unsigned int color  : 1;
	unsigned int alpha  : 2;
	unsigned int ctm    : 3;
	unsigned int stroke : 1;
	unsigned int flags  : 6;
} fz_display_node;

typedef struct
{
	float xstep;
	float ystep;
	fz_rect view;
	int id;
} fz_list_tile_data;

typedef enum
{
	FZ_CMD_FILL_PATH,
	FZ_CMD_STROKE_PATH,
	FZ_CMD_CLIP_PATH,
	FZ_CMD_CLIP_STROKE_PATH,
	FZ_CMD_FILL_TEXT,
	FZ_CMD_STROKE_TEXT,
	FZ_CMD_CLIP_TEXT,
	FZ_CMD_CLIP_STROKE_TEXT,
	FZ_CMD_IGNORE_TEXT,
	FZ_CMD_FILL_SHADE,
	FZ_CMD_FILL_IMAGE,
	FZ_CMD_FILL_IMAGE_MASK,
	FZ_CMD_CLIP_IMAGE_MASK,
	FZ_CMD_POP_CLIP,
	FZ_CMD_BEGIN_MASK,
	FZ_CMD_END_MASK,
	FZ_CMD_BEGIN_GROUP,
	FZ_CMD_END_GROUP,
	FZ_CMD_BEGIN_TILE,
	FZ_CMD_END_TILE,
	FZ_CMD_RENDER_FLAGS,
	FZ_CMD_DEFAULT_COLORSPACES,
	FZ_CMD_BEGIN_LAYER,
	FZ_CMD_END_LAYER,
	FZ_CMD_BEGIN_STRUCTURE,
	FZ_CMD_END_STRUCTURE,
	FZ_CMD_BEGIN_METATEXT,
	FZ_CMD_END_METATEXT
} fz_display_command;

enum { ISOLATED = 1, KNOCKOUT = 2 };
enum { OPM = 1, OP = 2, BP = 3, RI = 4};

static void
fz_unpack_color_params(fz_color_params *color_params, int flags)
{
	color_params->ri = (flags >> RI) & 3;
	color_params->bp = (flags >> BP) & 1;
	color_params->op = (flags >> OP) & 1;
	color_params->opm = (flags >> OPM) & 1;
}
static void align_node_for_pointer(fz_display_node **node)
{
	intptr_t ptr;

	if (FZ_POINTER_ALIGN_MOD <= 4)
		return;

	ptr = (intptr_t)*node;
	if (FZ_POINTER_ALIGN_MOD == 8)
	{
		if (ptr & 4)
			(*node) = (fz_display_node *)(ptr+4);
	}
	else
		(*node) = (fz_display_node *)((ptr + FZ_POINTER_ALIGN_MOD - 1) & ~(FZ_POINTER_ALIGN_MOD-1));
}

typedef struct fz_display_list_t
{
	fz_storable storable;
	fz_display_node *list;
	fz_rect mediabox;
	size_t max;
	size_t len;
} fz_display_list_valid;

typedef struct incr_state_t {
    /* Current graphics state as unpacked from list */
	fz_path *path;
	float alpha;
	fz_matrix ctm;
	fz_stroke_state *stroke;
	float color[FZ_MAX_COLORS];
	fz_colorspace *colorspace;
	fz_color_params color_params;
	fz_rect rect;
} incr_state;

typedef struct incremental_runner_t {
    fz_device *device;
    fz_display_list *list;
    incr_state *state;

    size_t max;
    size_t pointer;
} incremental_runner;

incr_state *create_incr_state(fz_context *ctx)
{
    incr_state *state = fz_malloc_struct(ctx, incr_state);

    state->path = NULL;
    state->alpha = 1.0f;
    state->ctm = fz_identity;
    state->stroke = NULL;
    memset(state->color, 0, sizeof(state->color));
    state->colorspace = fz_keep_colorspace(ctx, fz_device_gray(ctx));
    state->color_params = fz_default_color_params;
    state->rect = fz_empty_rect;

    return state;
}

void
run_display_list_incr( fz_context *ctx, incremental_runner *runner, fz_matrix top_ctm, fz_rect scissor, size_t max_steps )
{
    // already done
    if (runner->pointer >= runner->max) {
        return;
    }

    fz_display_node *node;
	fz_display_node *node_end;
	fz_display_node *next_node;
	int clipped = 0;
	int tiled = 0;
	int progress = 0;

	/* Get or create state */
	if (!runner->state) {
		runner->state = create_incr_state(ctx);
	}
	incr_state *state = runner->state;

	/* Current graphics state as unpacked from list */
    /* Recovered from runner storage */
	fz_path *path = state->path;
	float alpha = state->alpha;
	fz_matrix ctm = state->ctm;
	fz_stroke_state *stroke = state->stroke;
	float color[FZ_MAX_COLORS];
	memcpy(color, state->color, sizeof(color));
	fz_colorspace *colorspace = fz_keep_colorspace(ctx, state->colorspace);
	fz_color_params color_params = state->color_params;
	fz_rect rect = state->rect;

	/* Transformed versions of graphic state entries */
	fz_rect trans_rect;
	fz_matrix trans_ctm;
	int tile_skip_depth = 0;

    fz_device *dev = runner->device;
    fz_display_list_valid *list = (fz_display_list_valid*)runner->list;

	color_params = fz_default_color_params;

	size_t last_pointer = runner->pointer;
    size_t node_taps = 0;

	node = list->list + last_pointer;
	node_end = &list->list[list->len];

	for (; node != node_end ; node = next_node)
	{
        // drop when we not pop all masks and clips and over max steps
        // skip clipped nodes
        // this means than node that is clipped not included for clip
		if (node_taps > max_steps && !tiled && !clipped) {
			state->path = path;
			state->alpha = alpha;
			state->ctm = ctm;
			state->stroke = stroke;
			memcpy(state->color, color, sizeof(color));
			state->colorspace = fz_keep_colorspace(ctx, colorspace);
			state->color_params = color_params;
			state->rect = rect;
			return;
		}

		int empty;
		fz_display_node n = *node;
		size_t size = n.size;

		if (size == INDIRECT_NODE_THRESHOLD)
		{
			memcpy(&size, &node[1], sizeof(size_t));
			node += SIZE_IN_NODES(sizeof(size_t));
			size -= SIZE_IN_NODES(sizeof(size_t));
		}

		next_node = node + size;

        // valid pointer pos
		runner->pointer = last_pointer + progress;
        progress += (int)size;

		node++;
		if (n.rect)
		{
			rect = *(fz_rect *)node;
			node += SIZE_IN_NODES(sizeof(fz_rect));
		}
		if (n.cs)
		{
			int i, en;

			fz_drop_colorspace(ctx, colorspace);
			switch (n.cs)
			{
			default:
			case CS_GRAY_0:
				colorspace = fz_keep_colorspace(ctx, fz_device_gray(ctx));
				color[0] = 0.0f;
				break;
			case CS_GRAY_1:
				colorspace = fz_keep_colorspace(ctx, fz_device_gray(ctx));
				color[0] = 1.0f;
				break;
			case CS_RGB_0:
				colorspace = fz_keep_colorspace(ctx, fz_device_rgb(ctx));
				color[0] = 0.0f;
				color[1] = 0.0f;
				color[2] = 0.0f;
				break;
			case CS_RGB_1:
				colorspace = fz_keep_colorspace(ctx, fz_device_rgb(ctx));
				color[0] = 1.0f;
				color[1] = 1.0f;
				color[2] = 1.0f;
				break;
			case CS_CMYK_0:
				colorspace = fz_keep_colorspace(ctx, fz_device_cmyk(ctx));
				color[0] = 0.0f;
				color[1] = 0.0f;
				color[2] = 0.0f;
				color[3] = 0.0f;
				break;
			case CS_CMYK_1:
				colorspace = fz_keep_colorspace(ctx, fz_device_cmyk(ctx));
				color[0] = 0.0f;
				color[1] = 0.0f;
				color[2] = 0.0f;
				color[3] = 1.0f;
				break;
			case CS_OTHER_0:
				align_node_for_pointer(&node);
				colorspace = fz_keep_colorspace(ctx, *(fz_colorspace **)(node));
				node += SIZE_IN_NODES(sizeof(fz_colorspace *));
				en = fz_colorspace_n(ctx, colorspace);
				for (i = 0; i < en; i++)
					color[i] = 0.0f;
				break;
			}
		}
		if (n.color)
		{
			int nc = fz_colorspace_n(ctx, colorspace);
			memcpy(color, (float *)node, nc * sizeof(float));
			node += SIZE_IN_NODES(nc * sizeof(float));
		}
		if (n.alpha)
		{
			switch(n.alpha)
			{
			default:
			case ALPHA_0:
				alpha = 0.0f;
				break;
			case ALPHA_1:
				alpha = 1.0f;
				break;
			case ALPHA_PRESENT:
				alpha = *(float *)node;
				node += SIZE_IN_NODES(sizeof(float));
				break;
			}
		}
		if (n.ctm != 0)
		{
			float *packed_ctm = (float *)node;
			if (n.ctm & CTM_CHANGE_AD)
			{
				ctm.a = *packed_ctm++;
				ctm.d = *packed_ctm++;
				node += SIZE_IN_NODES(2*sizeof(float));
			}
			if (n.ctm & CTM_CHANGE_BC)
			{
				ctm.b = *packed_ctm++;
				ctm.c = *packed_ctm++;
				node += SIZE_IN_NODES(2*sizeof(float));
			}
			if (n.ctm & CTM_CHANGE_EF)
			{
				ctm.e = *packed_ctm++;
				ctm.f = *packed_ctm;
				node += SIZE_IN_NODES(2*sizeof(float));
			}
		}
		if (n.stroke)
		{
			align_node_for_pointer(&node);
			fz_drop_stroke_state(ctx, stroke);
			stroke = fz_keep_stroke_state(ctx, *(fz_stroke_state **)node);
			node += SIZE_IN_NODES(sizeof(fz_stroke_state *));
		}
		if (n.path)
		{
			align_node_for_pointer(&node);
			fz_drop_path(ctx, path);
			path = fz_keep_path(ctx, (fz_path *)node);
			node += SIZE_IN_NODES(fz_packed_path_size(path));
		}

		if (tile_skip_depth > 0)
		{
			if (n.cmd == FZ_CMD_BEGIN_TILE)
				tile_skip_depth++;
			else if (n.cmd == FZ_CMD_END_TILE)
				tile_skip_depth--;
			if (tile_skip_depth > 0)
				continue;
		}

		trans_rect = fz_transform_rect(rect, top_ctm);

		/* cull objects to draw using a quick visibility test */

		if (tiled ||
			n.cmd == FZ_CMD_BEGIN_TILE || n.cmd == FZ_CMD_END_TILE ||
			n.cmd == FZ_CMD_RENDER_FLAGS || n.cmd == FZ_CMD_DEFAULT_COLORSPACES ||
			n.cmd == FZ_CMD_BEGIN_LAYER || n.cmd == FZ_CMD_END_LAYER ||
			n.cmd == FZ_CMD_BEGIN_STRUCTURE || n.cmd == FZ_CMD_END_STRUCTURE ||
			n.cmd == FZ_CMD_BEGIN_METATEXT || n.cmd == FZ_CMD_END_METATEXT
			)
		{
			empty = 0;
		}
		else if (n.cmd == FZ_CMD_FILL_PATH || n.cmd == FZ_CMD_STROKE_PATH)
		{
			/* Zero area paths are suitable for stroking. */
			empty = !fz_is_valid_rect(fz_intersect_rect(trans_rect, scissor));
		}
		else if (n.cmd == FZ_CMD_FILL_TEXT || n.cmd == FZ_CMD_STROKE_TEXT ||
			n.cmd == FZ_CMD_CLIP_TEXT || n.cmd == FZ_CMD_CLIP_STROKE_TEXT)
		{
			/* Zero area text (such as spaces) should be passed
			 * through. Text that is completely outside the scissor
			 * can be elided. */
			empty = !fz_is_valid_rect(fz_intersect_rect(trans_rect, scissor));
		}
		else
		{
			empty = fz_is_empty_rect(fz_intersect_rect(trans_rect, scissor));
		}

		/* clipped starts out as 0. It only goes non-zero here if we move inside
		 * an 'empty' region. Whenever clipped is non zero, or we are in an empty
		 * region, we therefore may need to increment clipped according to the
		 * nesting. */
		if (clipped || empty)
		{
			switch (n.cmd)
			{
			case FZ_CMD_CLIP_PATH:
			case FZ_CMD_CLIP_STROKE_PATH:
			case FZ_CMD_CLIP_TEXT:
			case FZ_CMD_CLIP_STROKE_TEXT:
			case FZ_CMD_CLIP_IMAGE_MASK:
			case FZ_CMD_BEGIN_MASK:
			case FZ_CMD_BEGIN_GROUP:
				clipped++;
				continue;
			case FZ_CMD_BEGIN_STRUCTURE:
			case FZ_CMD_END_STRUCTURE:
			case FZ_CMD_BEGIN_METATEXT:
			case FZ_CMD_END_METATEXT:
				/* These may not nest as nicely as we'd like. Just ignore them for
				 * the purposes of clipping. */
				break;
			case FZ_CMD_POP_CLIP:
			case FZ_CMD_END_GROUP:
				if (!clipped)
					goto visible;
				clipped--;
				continue;
			case FZ_CMD_END_MASK:
				if (!clipped)
					goto visible;
				continue;
			default:
				continue;
			}
		}

visible:
		trans_ctm = fz_concat(ctm, top_ctm);
        node_taps++;

		fz_try(ctx)
		{
			switch (n.cmd)
			{
			case FZ_CMD_FILL_PATH:
				fz_unpack_color_params(&color_params, n.flags);
				fz_fill_path(ctx, dev, path, n.flags & 1, trans_ctm, colorspace, color, alpha, color_params);
				break;
			case FZ_CMD_STROKE_PATH:
				fz_unpack_color_params(&color_params, n.flags);
				fz_stroke_path(ctx, dev, path, stroke, trans_ctm, colorspace, color, alpha, color_params);
				break;
			case FZ_CMD_CLIP_PATH:
				fz_clip_path(ctx, dev, path, n.flags, trans_ctm, trans_rect);
				break;
			case FZ_CMD_CLIP_STROKE_PATH:
				fz_clip_stroke_path(ctx, dev, path, stroke, trans_ctm, trans_rect);
				break;
			case FZ_CMD_FILL_TEXT:
				fz_unpack_color_params(&color_params, n.flags);
				align_node_for_pointer(&node);
				fz_fill_text(ctx, dev, *(fz_text **)node, trans_ctm, colorspace, color, alpha, color_params);
				break;
			case FZ_CMD_STROKE_TEXT:
				fz_unpack_color_params(&color_params, n.flags);
				align_node_for_pointer(&node);
				fz_stroke_text(ctx, dev, *(fz_text **)node, stroke, trans_ctm, colorspace, color, alpha, color_params);
				break;
			case FZ_CMD_CLIP_TEXT:
				align_node_for_pointer(&node);
				fz_clip_text(ctx, dev, *(fz_text **)node, trans_ctm, trans_rect);
				break;
			case FZ_CMD_CLIP_STROKE_TEXT:
				align_node_for_pointer(&node);
				fz_clip_stroke_text(ctx, dev, *(fz_text **)node, stroke, trans_ctm, trans_rect);
				break;
			case FZ_CMD_IGNORE_TEXT:
				align_node_for_pointer(&node);
				fz_ignore_text(ctx, dev, *(fz_text **)node, trans_ctm);
				break;
			case FZ_CMD_FILL_SHADE:
				fz_unpack_color_params(&color_params, n.flags);
				align_node_for_pointer(&node);
				fz_fill_shade(ctx, dev, *(fz_shade **)node, trans_ctm, alpha, color_params);
				break;
			case FZ_CMD_FILL_IMAGE:
				fz_unpack_color_params(&color_params, n.flags);
				align_node_for_pointer(&node);
				fz_fill_image(ctx, dev, *(fz_image **)node, trans_ctm, alpha, color_params);
				break;
			case FZ_CMD_FILL_IMAGE_MASK:
				fz_unpack_color_params(&color_params, n.flags);
				align_node_for_pointer(&node);
				fz_fill_image_mask(ctx, dev, *(fz_image **)node, trans_ctm, colorspace, color, alpha, color_params);
				break;
			case FZ_CMD_CLIP_IMAGE_MASK:
				align_node_for_pointer(&node);
				fz_clip_image_mask(ctx, dev, *(fz_image **)node, trans_ctm, trans_rect);
				break;
			case FZ_CMD_POP_CLIP:
				fz_pop_clip(ctx, dev);
				break;
			case FZ_CMD_BEGIN_MASK:
				fz_unpack_color_params(&color_params, n.flags);
				fz_begin_mask(ctx, dev, trans_rect, n.flags & 1, colorspace, color, color_params);
				break;
			case FZ_CMD_END_MASK:
 				align_node_for_pointer(&node);
				fz_end_mask_tr(ctx, dev, *(fz_function **)node);
				break;
			case FZ_CMD_BEGIN_GROUP:
				fz_begin_group(ctx, dev, trans_rect, colorspace, (n.flags & ISOLATED) != 0, (n.flags & KNOCKOUT) != 0, (n.flags>>2), alpha);
				break;
			case FZ_CMD_END_GROUP:	
    			fz_end_group(ctx, dev);
				break;
			case FZ_CMD_BEGIN_TILE:
			{
				int cached;
				fz_list_tile_data *data;
				fz_rect tile_rect;
				data = (fz_list_tile_data *)node;
				tiled++;
				tile_rect = data->view;
				cached = fz_begin_tile_id(ctx, dev, rect, tile_rect, data->xstep, data->ystep, trans_ctm, data->id);
				if (cached)
					tile_skip_depth = 1;
				break;
			}
			case FZ_CMD_END_TILE:
				tiled--;
				fz_end_tile(ctx, dev);
				break;
			case FZ_CMD_RENDER_FLAGS:
				if (n.flags == 0)
					fz_render_flags(ctx, dev, 0, FZ_DEVFLAG_GRIDFIT_AS_TILED);
				else if (n.flags == 1)
					fz_render_flags(ctx, dev, FZ_DEVFLAG_GRIDFIT_AS_TILED, 0);
				break;
			case FZ_CMD_DEFAULT_COLORSPACES:
				align_node_for_pointer(&node);
				fz_set_default_colorspaces(ctx, dev, *(fz_default_colorspaces **)node);
				break;
			case FZ_CMD_BEGIN_LAYER:
				fz_begin_layer(ctx, dev, (const char *)node);
				break;
			case FZ_CMD_END_LAYER:
				fz_end_layer(ctx, dev);
				break;
			case FZ_CMD_BEGIN_STRUCTURE:
			{
				const unsigned char *data;
				int idx;
				data = (const unsigned char *)node;
				memcpy(&idx, data+1, sizeof(idx));
				fz_begin_structure(ctx, dev, (fz_structure)data[0], (const char *)(&data[1+sizeof(idx)]), idx);
				break;
			}
			case FZ_CMD_END_STRUCTURE:
				fz_end_structure(ctx, dev);
				break;
			case FZ_CMD_BEGIN_METATEXT:
			{
				const unsigned char *data;
				const char *text;
				data = (const unsigned char *)node;
				text = (const char *)&data[1];
				fz_begin_metatext(ctx, dev, (fz_metatext)data[0], text);
				break;
			}
			case FZ_CMD_END_METATEXT:
				fz_end_metatext(ctx, dev);
				break;
			}
		}
		fz_catch(ctx)
		{
			if (fz_caught(ctx) == FZ_ERROR_SYSTEM)
			{
				fz_drop_colorspace(ctx, colorspace);
				fz_drop_stroke_state(ctx, stroke);
				fz_drop_path(ctx, path);
				fz_rethrow(ctx);
			}

			if (fz_caught(ctx) == FZ_ERROR_ABORT)
			{
				fz_ignore_error(ctx);
				break;
			}
			fz_report_error(ctx);
			fz_warn(ctx, "Ignoring error during interpretation");
		}
	}
	fz_drop_colorspace(ctx, colorspace);
	fz_drop_stroke_state(ctx, stroke);
	fz_drop_path(ctx, path);

    if (runner->state) {
        fz_drop_colorspace(ctx, runner->state->colorspace);
        fz_drop_stroke_state(ctx, runner->state->stroke);
        fz_drop_path(ctx, runner->state->path);
        fz_free(ctx, runner->state);
        runner->state = NULL;
    }

    runner->pointer = progress;
}

incremental_runner *new_incremental(fz_context *ctx, fz_device *device, fz_display_list *list) {
    incremental_runner *runner = fz_malloc(ctx, sizeof(incremental_runner));
    runner->device = device;
    runner->list = list;
    runner->pointer = 0;
    runner->max =((fz_display_list_valid*)list)->len;
    return runner;
}

int
get_is_incremental_done( fz_context *ctx, incremental_runner *runner )
{
    return runner->pointer >= runner->max;
}

int step_runner_clipped(fz_context *ctx, incremental_runner *runner, fz_matrix ctm, fz_rect clip, int max_steps)
{
    if (!runner || !runner->device || !runner->list) {
        return -1;
    }

    run_display_list_incr(ctx, runner, ctm, clip, max_steps);
    return runner->pointer;

}

int step_runner(fz_context *ctx, incremental_runner *runner, fz_matrix ctm, int max_steps)
{
    return step_runner_clipped(ctx, runner, ctm, fz_infinite_rect, max_steps);
}

void drop_runner(fz_context *ctx, incremental_runner *runner)
{
    if (runner) {
        if (runner->state) {
            fz_drop_colorspace(ctx, runner->state->colorspace);
            fz_drop_stroke_state(ctx, runner->state->stroke);
            fz_drop_path(ctx, runner->state->path);
            fz_free(ctx, runner->state);
        }
        runner->pointer = 0;
        runner->max = 0;
        runner->list = NULL;
        runner->device = NULL;
        runner->state = NULL;
    }
}


#endif /* INCREMENTAL_DRAW_H */
