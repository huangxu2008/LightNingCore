// Copyright (C) 2004-2021 Artifex Software, Inc.
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

/* import SendMessage */
#include <Windows.h>

#include "cJson.h"
#include "mupdf/fitz.h"
#include "mupdf/pdf.h"
#include "toolhelper.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* message hwnd */
static sMsgHwnd = NULL;

/* error code */
static int s_error_code = 0;

/* pdf file infos */
static cJSON* datas = NULL;

struct info {
	int page;
	pdf_obj *pageref;
	union {
		struct {
			pdf_obj *obj;
		} info;
		struct {
			pdf_obj *obj;
		} crypt;
		struct {
			pdf_obj *obj;
			fz_rect *bbox;
		} dim;
		struct {
			pdf_obj *obj;
			pdf_obj *subtype;
			pdf_obj *name;
			pdf_obj *encoding;
		} font;
		struct {
			pdf_obj *obj;
			pdf_obj *width;
			pdf_obj *height;
			pdf_obj *bpc;
			pdf_obj *filter;
			pdf_obj *cs;
			pdf_obj *altcs;
		} image;
		struct {
			pdf_obj *obj;
			pdf_obj *type;
		} shading;
		struct {
			pdf_obj *obj;
			pdf_obj *type;
			pdf_obj *paint;
			pdf_obj *tiling;
			pdf_obj *shading;
		} pattern;
		struct {
			pdf_obj *obj;
			pdf_obj *groupsubtype;
			pdf_obj *reference;
		} form;
	} u;
};

typedef struct {
	pdf_document *doc;
	fz_context *ctx;
	fz_output *out;
	int pagecount;
	struct info *dim;
	int dims;
	struct info *font;
	int fonts;
	struct info *image;
	int images;
	struct info *shading;
	int shadings;
	struct info *pattern;
	int patterns;
	struct info *form;
	int forms;
	struct info *psobj;
	int psobjs;
} globals;

static void clearinfo(fz_context *ctx, globals *glo) {
	int i;
	if (glo->dim) {
		for (i = 0; i < glo->dims; i++)
			fz_free(ctx, glo->dim[i].u.dim.bbox);
		fz_free(ctx, glo->dim);
		glo->dim = NULL;
		glo->dims = 0;
	}
	if (glo->font) {
		fz_free(ctx, glo->font);
		glo->font = NULL;
		glo->fonts = 0;
	}
	if (glo->image)	{
		fz_free(ctx, glo->image);
		glo->image = NULL;
		glo->images = 0;
	}
	if (glo->shading) {
		fz_free(ctx, glo->shading);
		glo->shading = NULL;
		glo->shadings = 0;
	}
	if (glo->pattern) {
		fz_free(ctx, glo->pattern);
		glo->pattern = NULL;
		glo->patterns = 0;
	}
	if (glo->form) {
		fz_free(ctx, glo->form);
		glo->form = NULL;
		glo->forms = 0;
	}
	if (glo->psobj) {
		fz_free(ctx, glo->psobj);
		glo->psobj = NULL;
		glo->psobjs = 0;
	}
}

static void closexref(fz_context *ctx, globals *glo) {
	if (glo->doc) 	{
		pdf_drop_document(ctx, glo->doc);
		glo->doc = NULL;
	}
	clearinfo(ctx, glo);
}

static void lightning_pdfinfo_info(fz_context* ctx, char* filename, char* password) {
	globals glo = { 0 };
	glo.ctx = ctx;
	fz_try(ctx) {
		cJSON* data = NULL;
		cJSON_AddItemToArray(datas, data = cJSON_CreateObject());
		cJSON_AddStringToObject(data, "filePath", filename);
		glo.doc = pdf_open_document(glo.ctx, filename);
		if (pdf_needs_password(ctx, glo.doc)) {
			if (!pdf_authenticate_password(ctx, glo.doc, password)) {
				cJSON_AddTrueToObject(data, "userPassword");
				cJSON_AddNumberToObject(data, "pageCounts", 0);
				/* check file user password failed, can not query page counts of this file */
				fz_throw(glo.ctx, FZ_ERROR_ARGUMENT, "cannot authenticate password: %s", filename);
			}
		}
		glo.pagecount = pdf_count_pages(ctx, glo.doc);
		cJSON_AddNumberToObject(data, "pageCounts", glo.pagecount);
		/// to modify 如果有需要，进一步验证是否有编辑密码
	}
	fz_always(ctx) {
		closexref(ctx, &glo);
	}
	fz_catch(ctx) {
		fz_rethrow(ctx);
	}
}

int lightning_pdfinfo_main(int argc, char** argv) {
	int c;
	int task_id = 0;
	int msg_hwnd = 0;
	int process_id = 0;
	char* filepath = "";
	char* password = "";
	char* filelist = "";
	char* append_params = "";
	while ((c = fz_getopt(argc, argv, "m:i:t:f:F:p:P:")) != -1) {
		switch (c) {
		case 'm': msg_hwnd = atoi(fz_optarg); break;
		case 'i': process_id = atoi(fz_optarg); break;
		case 't': task_id = atoi(fz_optarg); break;
		case 'f': filepath = fz_optarg; break;
		case 'F': filelist = fz_optarg; break;
		case 'p': password = fz_optarg; break;
		case 'P': append_params = fz_optarg; break;
		default: break;
		}
	}
	sMsgHwnd = (HWND)(msg_hwnd);
	cJSON* root = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "taskId", cJSON_CreateNumber(task_id));
#if GLOBAL_CHECK
	if (!check_identity(process_id, s_msg_hwnd)) {
		cJSON_AddItemToObject(root, "errorMsg", cJSON_CreateString("验证身份信息失败"));
		cJSON_AddItemToObject(root, "errorCode", cJSON_CreateNumber(_check_identity_failed));
		char* out = cJSON_Print(root);
		send_copydata(s_msg_hwnd, out);
		cJSON_Delete(root);
		root = NULL;
		exit(1);
}
#endif // _GLOBAL_CHECK_
	BOOL append_psw = FALSE;
	cJSON* params = cJSON_Parse(append_params);
	cJSON* appendPsw = cJSON_GetObjectItem(params, "appendPsw");
	if (appendPsw) {
		if (appendPsw->type == cJSON_False) {
			append_psw = FALSE;
		} else if (appendPsw->type == cJSON_True) {
			append_psw = TRUE;
		}
	}
	datas = cJSON_CreateArray();
	cJSON_Delete(params);
	params = NULL;
	if (strlen(filepath) != 0) {
		/* prioritize using filepath. */
		/* Create a context to hold the exception stack and various caches. */
		fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
		if (!ctx) {
			fprintf(stderr, "cannot initialise fz_context\n");
			cJSON_AddItemToObject(root, "errorMsg", cJSON_CreateString("创建fz_context失败"));
			cJSON_AddItemToObject(root, "errorCode", cJSON_CreateNumber(_create_mupdf_ctx_failed));
			char* out = cJSON_Print(root);
			send_copydata(sMsgHwnd, out);
			cJSON_Delete(root);
			root = NULL;
			exit(1);
		}
		fz_try(ctx) {
			lightning_pdfinfo_info(ctx, filepath, password);
		}
		fz_catch(ctx) {
			fz_report_error(ctx);
		}
		fz_drop_context(ctx);
	}
	else {
		if (strlen(filelist) == 0) {
			s_error_code = _filepath_params_error;
		} else {
			FILE* file = fopen(filelist, "r");
			if (!file) {
				s_error_code = _filelist_params_error;
			} else {
				if (!append_psw) {
					char line[MAX_PATH];
					while (fgets(line, sizeof(line), file)) {
						if (line[strlen(line) - 1] == '\n') {
							line[strlen(line) - 1] = '\0';
						}
						fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
						if (!ctx) {
							fprintf(stderr, "cannot initialise fz_context\n");
							cJSON_AddItemToObject(root, "errorMsg", cJSON_CreateString("创建fz_context失败"));
							cJSON_AddItemToObject(root, "errorCode", cJSON_CreateNumber(_create_mupdf_ctx_failed));
							char* out = cJSON_Print(root);
							send_copydata(sMsgHwnd, out);
							cJSON_Delete(root);
							root = NULL;
							exit(1);
						}
						fz_try(ctx) {
							lightning_pdfinfo_info(ctx, line, "");
						}
						fz_catch(ctx) {
							fz_report_error(ctx);
						}
						fz_drop_context(ctx);
					}
				}
				else {
					int index = 0;
					char line[MAX_PATH];
					char first[MAX_PATH];
					char second[MAX_PATH];
					while (fgets(line, sizeof(line), file) != NULL) {
						if (line[strlen(line) - 1] == '\n') {
							line[strlen(line) - 1] = '\0';
						}
						int lenght = strlen(line);
						if (index % 2 == 0) {
							strncpy(first, line, strlen(line) + 1);
						} else {
							strncpy(second, line, strlen(line) + 1);
							fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
							if (!ctx) {
								fprintf(stderr, "cannot initialise fz_context\n");
								cJSON_AddItemToObject(root, "errorMsg", cJSON_CreateString("创建fz_context失败"));
								cJSON_AddItemToObject(root, "errorCode", cJSON_CreateNumber(_create_mupdf_ctx_failed));
								char* out = cJSON_Print(root);
								send_copydata(sMsgHwnd, out);
								cJSON_Delete(root);
								root = NULL;
								exit(1);
							}
							fz_try(ctx) {
								lightning_pdfinfo_info(ctx, first, second);
							}
							fz_catch(ctx) {
								fz_report_error(ctx);
							}
							fz_drop_context(ctx);
						}
						index++;
					}
				}
			}
		}
	}
	cJSON_AddItemToObject(root, "errorCode", cJSON_CreateNumber(s_error_code));
	cJSON_AddItemToObject(root, "datas", datas);
	char* out = cJSON_Print(root);
	send_copydata(sMsgHwnd, out);
	cJSON_Delete(root);
	root = NULL;
	return s_error_code;
}
