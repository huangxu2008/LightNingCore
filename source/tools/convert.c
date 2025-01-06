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

#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdio.h>
#include <time.h>

#include "cJson.h"
#include "mupdf/fitz.h"
#include "toolhelper.h"


/* global params */
HWND _caller_msg_hwnd = NULL;		// ���÷����ڽ�����Ϣ�Ĵ��ھ��

int _post_message_id = WM_USER;		// ���ڽ��ս��ȵ���ϢID
int _copy_data_msg_id = 0;			// ���ڽ�����ϸ��Ϣ��copyData��ϢID
int _caller_task_id = 0;			// ���÷�������ID

/* message hwnd */
static sMsgHwnd = NULL;

/* error code */
static int s_error_code = 0;

/* input options */
static int alphabits = 8;
static int layout_use_doc_css = 1;
static float engine_dpi = 72.0f;
static float engine_inc = 2.54f;
static float layout_w = FZ_DEFAULT_LAYOUT_W;
static float layout_h = FZ_DEFAULT_LAYOUT_H;
static float layout_em = FZ_DEFAULT_LAYOUT_EM;

/*----------------------------------------------------------------------------------------------------*/

static wchar_t* lightning_wchar_from_utf8(const char* s) {
	wchar_t* d, * r;
	int c;
	/* This allocation is larger than we need, but it's guaranteed
	 * to be safe. */
	r = d = malloc((strlen(s) + 1) * sizeof(wchar_t));
	if (!r)
		return NULL;
	while (*s) {
		s += fz_chartorune(&c, s);
		/* Truncating c to a wchar_t can be problematic if c
		 * is 0x10000. */
		if (c >= 0x10000) {
			c -= 0x10000;
			*d++ = 0xd800 + (c >> 10);
			c = 0xdc00 + (c & 1023);
		}
		*d++ = c;
	}
	*d = 0;
	return r;
}

static void lightning_runpage(fz_context* ctx, fz_document* doc, fz_document_writer* out, int number) {
	fz_page* page = fz_load_page(ctx, doc, number - 1);
	fz_device* dev = NULL;
	fz_var(dev);
	fz_try(ctx) {
		fz_rect box = fz_bound_page_box(ctx, page, FZ_CROP_BOX);
		fz_matrix ctm = fz_translate(-box.x0, -box.y0);
		box = fz_transform_rect(box, ctm);
		dev = fz_begin_page(ctx, out, box);
		fz_run_page(ctx, page, dev, ctm, NULL);
		fz_end_page(ctx, out);
	}
	fz_always(ctx) {
		fz_drop_page(ctx, page);
	}
	fz_catch(ctx) {
		fz_rethrow(ctx);
	}
}

static void lightning_runrange(fz_context* ctx, fz_document* doc, fz_document_writer* out, const char* range, int count) {
	int start, end, i;
	while ((range = fz_parse_page_range(ctx, range, &start, &end, count))) {
		if (start < end) {
			for (i = start; i <= end; ++i) {
				lightning_runpage(ctx, doc, out, i);
			}
		} else {
			for (i = start; i >= end; --i) {
				lightning_runpage(ctx, doc, out, i);
			}
		}
	}
}

static void lightning_image2pdf(fz_context* ctx, fz_document* doc, fz_document_writer* out, int pageWidth, int pageHeight,
	int pageLeftMargin, int pageRightMargin, int pageTopMargin, int pageBottomMargin) {
	fz_device* dev = NULL;
	fz_page* page = fz_load_page(ctx, doc, 0);
	fz_var(dev);
	fz_try(ctx) {
		fz_rect mediabox = fz_bound_page(ctx, page);
		if (pageWidth * pageHeight == 0 || pageWidth < 0 || pageHeight < 0) {
			// ֻҪ��һ��Ϊ0������һ��Ϊ��������ô��ָ����С����Ч������ԭͼ��С����
			dev = fz_begin_page(ctx, out, mediabox);
			fz_run_page(ctx, page, dev, fz_identity, NULL);
			fz_end_page(ctx, out);
		} else {
			if (pageTopMargin + pageBottomMargin  >= pageHeight || pageLeftMargin + pageRightMargin >= pageWidth) {
				// ����߾������ô�߾�����ý�ʧЧ
				pageTopMargin = pageBottomMargin = pageLeftMargin = pageRightMargin = 0;
			}
			float imageX1 = (mediabox.x1 - mediabox.x0);
			float imageY1 = (mediabox.y1 - mediabox.y0);
			float mgLeft = (float)pageLeftMargin * engine_dpi / engine_inc / 10;
			float mgRight = (float)pageRightMargin * engine_dpi / engine_inc / 10;
			float mgTop = (float)pageTopMargin * engine_dpi / engine_inc / 10;
			float mgBottom = (float)pageBottomMargin * engine_dpi / engine_inc / 10;
			float x1 = (float)pageWidth * engine_dpi / engine_inc / 10;
			float y1 = (float)pageHeight * engine_dpi / engine_inc / 10;
			fz_rect pageRect = { 0, 0, x1, y1 };
			dev = fz_begin_page(ctx, out, pageRect);
			fz_matrix ctm = fz_identity;
			if (x1 - (mgLeft + mgRight) >= imageX1 && y1 - (mgTop + mgBottom) >= imageY1) {
				// ֽ�ű�ͼƬ��ͼƬ��þ���
				ctm.e = (x1 - imageX1) / 2;
				ctm.f = (y1 - imageY1) / 2;
			} else {
				float scale = min((x1 - (mgLeft + mgRight)) / imageX1, (y1 - (mgTop + mgBottom)) / imageY1);
				ctm.a = scale;
				ctm.d = scale;
				ctm.e = (x1 - imageX1 * scale) / 2;
				ctm.f = (y1 - imageY1 * scale) / 2;
			}
			fz_run_page(ctx, page, dev, ctm, NULL);
			fz_end_page(ctx, out);
		}		
	}
	fz_always(ctx) {
		fz_drop_page(ctx, page);
	}
	fz_catch(ctx) {
		OutputDebugString("[SLPDF] ִ�е�ǰҳת��ʧ��");
		fz_rethrow(ctx);
	}
}

/*----------------------------------------------------------------------------------------------------*/

int lightning_pdf2image_main(int argc, char** argv) {
	int c;
	int task_id = 0;
	int msg_hwnd = 0;
	int process_id = 0;
	char* options = "";
	char* filepath = "";
	char* password = "";
	char* filelist = "";
	char* fileformat = "";
	char* outputpath = "";
	char* append_params = "";
	fz_document_writer* out = NULL;
	while ((c = fz_getopt(argc, argv, "m:i:t:f:F:p:P:o:O:")) != -1) {
		switch (c) {
		case 'm': msg_hwnd = atoi(fz_optarg); break;
		case 'i': process_id = atoi(fz_optarg); break;
		case 't': task_id = atoi(fz_optarg); break;
		case 'f': filepath = fz_optarg; break;
		case 'F': filelist = fz_optarg; break;
		case 'p': password = fz_optarg; break;
		case 'P': append_params = fz_optarg; break;
		case 'o': outputpath = fz_optarg; break;
		case 'O': options = fz_optarg; break;
		default: break;
		}
	}
	sMsgHwnd = (HWND)(msg_hwnd);
#if GLOBAL_CHECK
	if (!check_identity(process_id, s_msg_hwnd)) {
		SendMessage(s_msg_hwnd, WM_PDF2IMAGE, (WPARAM)task_id, MAKELPARAM(_check_identity_failed, 0));
		exit(1);
	}
#endif // _GLOBAL_CHECK_
	cJSON* params = cJSON_Parse(append_params);
	cJSON* format = cJSON_GetObjectItem(params, "format");
	if (format && format->type == cJSON_String) {
		fileformat = format->valuestring;
	}
	if (strlen(filepath) != 0) {
		/* prioritize using filepath. */
		/* Create a context to hold the exception stack and various caches. */
		fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
		if (!ctx) {
			fprintf(stderr, "cannot initialise context\n");
			SendMessage(sMsgHwnd, WM_PDF2IMAGE, (WPARAM)task_id, MAKELPARAM(_create_mupdf_ctx_failed, 0));
			exit(1);
		}
		/* Register the default file types to handle. */
		fz_try(ctx) {
			fz_register_document_handlers(ctx);
		}
		fz_catch(ctx) {
			fz_report_error(ctx);
			fz_drop_context(ctx);
			fprintf(stderr, "cannot register document handlers\n");
			SendMessage(sMsgHwnd, WM_PDF2IMAGE, (WPARAM)task_id, MAKELPARAM(_reg_doc_handlers_error, 0));
			exit(1);
		}
		fz_set_aa_level(ctx, alphabits);
		fz_set_use_document_css(ctx, layout_use_doc_css);
		/* Open the output document. */
		fz_try(ctx) {
			out = fz_new_document_writer(ctx, outputpath, fileformat, options);
		}
		fz_catch(ctx) {
			fz_report_error(ctx);
			fz_drop_context(ctx);
			fprintf(stderr, "cannot create document\n");
			SendMessage(sMsgHwnd, WM_PDF2IMAGE, (WPARAM)task_id, MAKELPARAM(_create_doc_error, 0));
			exit(1);
		}
		fz_document* doc = NULL;
		fz_var(doc);
		fz_try(ctx) {
			doc = fz_open_document(ctx, filepath);
			if (fz_needs_password(ctx, doc)) {
				if (!fz_authenticate_password(ctx, doc, password)) {
					/// to modify ��֤������ʧ�ܣ��޷���ѯ��ҳ��
					fz_throw(ctx, FZ_ERROR_ARGUMENT, "cannot authenticate password: %s", filepath);
				}
			}
			fz_layout_document(ctx, doc, layout_w, layout_h, layout_em);
			int pagecounts = fz_count_pages(ctx, doc);
			lightning_runrange(ctx, doc, out, "1-N", pagecounts);
			fz_drop_document(ctx, doc);
			fz_close_document_writer(ctx, out);
		}
		fz_always(ctx) {
			fz_drop_document(ctx, doc);
			fz_drop_document_writer(ctx, out);
		}
		fz_catch(ctx) {
			fz_report_error(ctx);
			s_error_code = EXIT_FAILURE;
		}
	}
	return s_error_code;
}

/*
 * -m msgHwnd �������� ������ ���÷��Ĵ��ھ��
 * -i Id �������� ������ ���÷��Ľ���ID
 * -t taskId �������� ��ѡ��-ǿ�ҽ�����д ���÷�������ID
 * -f filePath �ַ������� ��ѡ�� ������ͼƬȫ·����������
 * -F fileList �ַ������� ��ѡ�� ������ͼƬȫ·���������
 * -o output �ַ������� ������ PDF�ļ������·��
 * -c combine �������� ��ѡ�� �Ƿ�ϲ������Ĭ��ֵ1
 * -W pageWidth �������� ��ѡ�� PDF�ļ�ҳ���ȣ�Ĭ��ֵΪ0
 * -H pageHeight �������� ��ѡ�� PDF�ļ�ҳ��߶ȣ�Ĭ��Ϊֵ0
 * -M pageMargin �������� ��ѡ�� PDF�ļ�ҳ��߾࣬Ĭ��ֵΪ0
 * -S sourceOutput �������� ��ѡ�� �����������·���Ƿ�ΪԴ�ļ�·����Ĭ��ֵΪ0
 * -P postMessage �������� ��ѡ�� ���ڽ��ս��ȵ���ϢID
 * -C copyData �������� ��ѡ�� ���ڽ�����ϸ��Ϣ��copyData��ϢID
 */
int lightning_image2pdf_main(int argc, char** argv) {
	int c = 0;
	int merge = 1;
	int processId = 0;
	int sourceput = 0;
	int pageWidth = 0;
	int pageHeight = 0;
	int pageLeftMargin = 0;
	int pageRightMargin = 0;
	int pageTopMargin = 0;
	int pageBottomMargin = 0;
	char* output = "";
	char* filePath = "";
	char* fileList = "";
	char* ff = "";
	while ((c = fz_getopt(argc, argv, "o:f:F:W:H:c:i:S:t:P:C:m:M:")) != -1) {
		switch (c) {
		case 'o': output = fz_optarg; break;
		case 'f': filePath = fz_optarg; break;
		case 'F': fileList = fz_optarg; break;
		case 'c': merge = atoi(fz_optarg); break;
		case 'i': processId = atoi(fz_optarg); break;
		case 'S': sourceput = atoi(fz_optarg); break;
		case 'W': pageWidth = atoi(fz_optarg); break;
		case 'H': pageHeight = atoi(fz_optarg); break;
		// case 'M': pageMargin = atoi(fz_optarg); break;
		case 'M': ff = fz_optarg; break;
		case 't': _caller_task_id = atoi(fz_optarg); break;
		case 'P': _post_message_id = atoi(fz_optarg); break;
		case 'C': _copy_data_msg_id = atoi(fz_optarg); break;
		case 'm': _caller_msg_hwnd = (HWND)(atoi(fz_optarg)); break;
		default: break;
		}
	}
	int count = 0;
	int* numbers = NULL;
	char* token = strtok(ff, ",");
	while (token != NULL) {
		int num = atoi(token);
		numbers = realloc(numbers, (count + 1) * sizeof(int));
		if (numbers == NULL) {
			perror("realloc failed");
			return EXIT_FAILURE;
		}
		numbers[count] = num;
		count++;

		// �����ָ�ʣ����ַ���
		token = strtok(NULL, ",");
	}
	if (count == 1) {
		pageLeftMargin = pageRightMargin = pageTopMargin = pageBottomMargin = numbers[0];
	} else if (count == 2) {
		pageTopMargin = pageBottomMargin = numbers[0];
		pageLeftMargin = pageRightMargin = numbers[1];
	} else if (count == 3) {
		pageTopMargin = numbers[0];
		pageLeftMargin = pageRightMargin = numbers[1];
		pageBottomMargin = numbers[2];
	} else if (count == 4) {
		pageTopMargin = numbers[0];
		pageRightMargin = numbers[1];
		pageBottomMargin = numbers[2];
		pageLeftMargin = numbers[3];
	} else {
		pageLeftMargin = pageRightMargin = pageTopMargin = pageBottomMargin = 0;
	}
	if (!check_identity(processId, _caller_msg_hwnd)) {
		if (_caller_msg_hwnd) {
			PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(_check_identity_failed, 0));
		} else {
			/// to modify ʹ��copyData��ʽ��������
		}
		OutputDebugString("[SLPDF] ��֤�����Ϣʧ��\n");
		exit(1);
	}
	if (_post_message_id < WM_USER) {
		char buffer[200];
		sprintf(buffer, "[SLPDF] ������һ��Σ�յ���ϢID:%d\n", _post_message_id);
		OutputDebugString(buffer);
	}
	// ��֤���·�������������������-ûָ��sourceOutput-ûָ��output ���� �ϲ����-ûָ��output�����������֤����
	if ((!merge && !sourceput && !strlen(output)) || (merge && !strlen(output))) {
		if (_caller_msg_hwnd) {
			PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(_outputfile_empty, 0));
		} else {
			/// to modify ʹ��copyData��ʽ��������
		}
		char buffer[200];
		sprintf(buffer, "[SLPDF] �޷���λ��׼ȷ�����·��:��merge:%d, sourceput:%d, output:%s��\n", merge, sourceput, output);
		OutputDebugString(buffer);
		exit(1);
	}
	// ��ȡ�������ļ����ϣ����ȼ���ϵ��-f -F ĩβ����
	char** fileArray;
	int fileCounts = 0;
	if (strlen(filePath)) {
		fileCounts = 1;
		fileArray = (char**)malloc(fileCounts * sizeof(char*));
		if (fileArray == NULL) {
			OutputDebugString("[SLPDF] �������������ļ�ʱ�ڴ����ʧ��\n");
			exit(1);
		}
		fileArray[0] = (char*)malloc(strlen(filePath) * sizeof(char) + 1);
		strcpy(fileArray[0], filePath);
	}
	else if (strlen(fileList)) {
		FILE* file = fopen(fileList, "r");
		char line[1024];
		while (fgets(line, sizeof(line), file)) {
			fileCounts++;
		}
		fileArray = (char**)malloc(fileCounts * sizeof(char*));
		if (fileArray == NULL) {
			OutputDebugString("[SLPDF] ������������ļ�ʱ�ڴ����ʧ��\n");
			fclose(fileList);
			exit(1);
		}
		int current = 0;
		fseek(file, 0, SEEK_SET);
		while (fgets(line, sizeof(line), file)) {
			if (line[strlen(line) - 1] == '\n') {
				line[strlen(line) - 1] = '\0';
			}
			if (current < fileCounts) {
				fileArray[current] = (char*)malloc(strlen(line) * sizeof(char) + 1);
				strcpy(fileArray[current++], line);
			}
		}
	}
	else if (fz_optind < argc) {
		fileCounts = argc - fz_optind;
		fileArray = (char**)malloc(fileCounts * sizeof(char*));
		if (fileArray == NULL) {
			OutputDebugString("[SLPDF] �����޲ζ�������ļ�ʱ�ڴ����ʧ��\n");
			exit(1);
		}
		int current = 0;
		for (int i = fz_optind; i < argc; ++i) {
			char* item = argv[i];
			fileArray[current] = (char*)malloc(strlen(item) * sizeof(char) + 1);
			strcpy(fileArray[current++], item);
		}
	}
	// ��֤�������ļ�����
	if (!fileCounts) {
		if (_caller_msg_hwnd) {
			PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(_filelist_params_error, 0));
		} else {
			/// to modify ʹ��copyData��ʽ��������
		}
		OutputDebugString("[SLPDF] �޷��ҵ���Ч�������ļ�\n");
		exit(1);
	}
	fz_document* doc = NULL;
	fz_document_writer* out = NULL;
	fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
	if (!ctx) {
		free_char_array(fileArray, fileCounts);
		if (_caller_msg_hwnd) {
			PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(_create_mupdf_ctx_failed, 0));
		} else {
			/// to modify ʹ��copyData��ʽ��������
		}
		OutputDebugString("[SLPDF] ����ctxʧ��\n");
		exit(1);
	}
	// �ϲ����
	fz_var(doc);
	if (merge) {
		fz_try(ctx) {
			fz_register_document_handlers(ctx);
		}
		fz_catch(ctx) {
			free_char_array(fileArray, fileCounts);
			if (_caller_msg_hwnd) {
				PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(_reg_doc_handlers_error, 0));
			} else {
				/// to modify ʹ��copyData��ʽ��������
			}
			char buffer[200];
			sprintf(buffer, "[SLPDF] ע��doc���ʧ��:%s\n", ctx->error.message);
			OutputDebugString(buffer);
			fz_drop_context(ctx);
			exit(1);
		}
		fz_set_aa_level(ctx, alphabits);
		fz_set_use_document_css(ctx, layout_use_doc_css);
		// Open the output document
		fz_try(ctx) {
			out = fz_new_document_writer(ctx, output, "pdf", "");
		}
		fz_catch(ctx) {
			free_char_array(fileArray, fileCounts);
			if (_caller_msg_hwnd) {
				PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(_create_doc_error, 0));
			} else {
				/// to modify ʹ��copyData��ʽ��������
			}
			char buffer[200];
			sprintf(buffer, "[SLPDF] ��������ļ�ʧ��:��output:%s, errmsg:%s��\n", output, ctx->error.message);
			OutputDebugString(buffer);
			fz_drop_context(ctx);
			exit(1);
		}
		// Convert images to pdf
		fz_try(ctx) {
			if (_caller_msg_hwnd) {
				PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(0, fileCounts));
				OutputDebugString("[SLPDF] ��ʼִ������\n");
			} else {
				/// to modify ʹ��copyData��ʽ��������
			}
			for (size_t i = 0; i < fileCounts; i++) {
				doc = fz_open_document(ctx, fileArray[i]);
				fz_layout_document(ctx, doc, layout_w, layout_h, layout_em);
				lightning_image2pdf(ctx, doc, out, pageWidth, pageHeight, pageLeftMargin, pageRightMargin, pageTopMargin, pageBottomMargin);
				fz_drop_document(ctx, doc);
				doc = NULL;
				if (_caller_msg_hwnd) {
					PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(i + 1, fileCounts));
					char buffer[200];
					sprintf(buffer, "[SLPDF] ����ִ�н���: %d/%d\n", i + 1, fileCounts);
					OutputDebugString(buffer);
				} else {
					/// to modify ʹ��copyData��ʽ��������
				}
			}
		}
		fz_catch(ctx) {
			free_char_array(fileArray, fileCounts);
			if (_caller_msg_hwnd) {
				PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(ctx->error.errcode, 0));
			} else {
				/// to modify ʹ��copyData��ʽ��������
			}
			char buffer[200];
			sprintf(buffer, "[SLPDF] ͼƬתPDF����ʧ��:%s\n", ctx->error.message);
			OutputDebugString(buffer);
			fz_drop_context(ctx);
			exit(1);
		}
		// Close writer
		fz_try(ctx) {
			fz_close_document_writer(ctx, out);
		}
		fz_catch(ctx) {
			free_char_array(fileArray, fileCounts);
			fz_drop_document_writer(ctx, out);
			if (_caller_msg_hwnd) {
				PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(ctx->error.errcode, 0));
			} else {
				/// to modify ʹ��copyData��ʽ��������
			}
			char buffer[200];
			sprintf(buffer, "[SLPDF] �����ļ�ʧ��:%s\n", ctx->error.message);
			OutputDebugString(buffer);
			fz_drop_context(ctx);
			exit(1);
		}
		if (_caller_msg_hwnd) {
			PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(fileCounts + 1, fileCounts));
			OutputDebugString("[SLPDF] ����ִ�����\n");
		} else {
			/// to modify ʹ��copyData��ʽ��������
		}
		fz_drop_document_writer(ctx, out);
		fz_drop_context(ctx);
	} else {
		// ������ʱ�����outputȫ·���ļ�����Ϊ�ļ�������
		char newPath[1024];
		memcpy(newPath, output, strlen(output) - 4);
		newPath[strlen(output) - 4] = '\0';
		// ����ļ��в����ڣ���ô��Ҫ�������ļ���
		struct stat st;
		if (stat(newPath, &st) != 0 || S_ISDIR(st.st_mode) <= 0) {
			wchar_t* wname = lightning_wchar_from_utf8(newPath);
			CreateDirectoryW(wname, NULL);
		}
		fz_try(ctx) {
			if (_caller_msg_hwnd) {
				PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(0, fileCounts));
				OutputDebugString("[SLPDF] ��ʼִ������\n");
			} else {
				/// to modify ʹ��copyData��ʽ��������
			}
			for (size_t i = 0; i < fileCounts; i++) {
				char pdfFormat[5] = ".pdf\0";
				char newOutput[1024]; // ���·��
				char* fileName = strrchr(fileArray[i], '\\');
				if (!fileName || !strlen(fileName)) {
					fz_throw(ctx, FZ_ERROR_GENERIC, "input file name error");
				}
				char* format = strrchr(fileName, '.');
				if (!format || !strlen(format)) {
					fz_throw(ctx, FZ_ERROR_GENERIC, "input file format error");
				}
				time_t now;
				time(&now);
				char timestamp[30];
				size_t timeLen = strftime(timestamp, sizeof(timestamp), "_%y%m%d%H%M%S", localtime(&now));
				if (sourceput) {
					int outputLenght = strlen(fileArray[i]) - strlen(fileName);
					memcpy(newOutput, fileArray[i], outputLenght);
					for (size_t i = 0; i < strlen(fileName) - strlen(format); i++) {
						newOutput[outputLenght++] = fileName[i];
					}
					for (size_t i = 0; i < timeLen; i++) {
						newOutput[outputLenght++] = timestamp[i];
					}
					for (size_t i = 0; i < strlen(pdfFormat); i++) {
						newOutput[outputLenght++] = pdfFormat[i];
					}
					newOutput[outputLenght] = '\0';					
				} else {
					int length = strlen(newPath);
					memcpy(newOutput, newPath, strlen(newPath));
					for (size_t i = 0; i < strlen(fileName) - strlen(format); i++) {
						newOutput[length++] = fileName[i];
					}
					for (size_t i = 0; i < timeLen; i++) {
						newOutput[length++] = timestamp[i];
					}
					for (size_t i = 0; i < strlen(pdfFormat); i++) {
						newOutput[length++] = pdfFormat[i];
					}
					newOutput[length] = '\0';
				}
				fz_try(ctx) {
					fz_register_document_handlers(ctx);
				}
				fz_catch(ctx) {
					fz_drop_context(ctx);
					fz_throw(ctx, _reg_doc_handlers_error, "can not reg doc handlers");
				}
				fz_set_aa_level(ctx, alphabits);
				fz_set_use_document_css(ctx, layout_use_doc_css);
				// Open the output document
				fz_try(ctx) {
					out = fz_new_document_writer(ctx, newOutput, "pdf", "");
				}
				fz_catch(ctx) {
					fz_drop_context(ctx);
					fz_throw(ctx, _create_doc_error, "can not create doc");
				}
				// Convert image to pdf
				fz_try(ctx) {
					doc = fz_open_document(ctx, fileArray[i]);
					fz_layout_document(ctx, doc, layout_w, layout_h, layout_em);
					lightning_image2pdf(ctx, doc, out, pageWidth, pageHeight, pageLeftMargin, pageRightMargin, pageTopMargin, pageBottomMargin);
					fz_drop_document(ctx, doc);
				}
				fz_catch(ctx) {
					fz_throw(ctx, FZ_ERROR_GENERIC, "can not convert image to pdf");
				}
				// Close writer
				fz_try(ctx) {
					fz_close_document_writer(ctx, out);
				}
				fz_catch(ctx) {
					fz_throw(ctx, FZ_ERROR_GENERIC, "can not close doc");
				}
				if (_caller_msg_hwnd) {
					PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(i + 1, fileCounts));
					char buffer[200];
					sprintf(buffer, "[SLPDF] ����ִ�н���: %d/%d\n", i + 1, fileCounts);
					OutputDebugString(buffer);
				} else {
					/// to modify ʹ��copyData��ʽ��������
				}
			}
			if (_caller_msg_hwnd) {
				PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(fileCounts + 1, fileCounts));
				OutputDebugString("[SLPDF] ����ִ�����\n");
			} else {
				/// to modify ʹ��copyData��ʽ��������
			}
		}
		fz_catch(ctx) {
			free_char_array(fileArray, fileCounts);
			if (_caller_msg_hwnd) {
				PostMessage(_caller_msg_hwnd, _post_message_id, (WPARAM)_caller_task_id, MAKELPARAM(ctx->error.errcode, 0));
				char buffer[200];
				sprintf(buffer, "[SLPDF] ͼƬתPDF����ʧ��:%s\n", ctx->error.message);
				OutputDebugString(buffer);
			} else {
				/// to modify ʹ��copyData��ʽ��������
			}
			exit(1);
		}
		fz_drop_document_writer(ctx, out);
		fz_drop_context(ctx);
	}
	free_char_array(fileArray, fileCounts);
	OutputDebugString("[SLPDF] ����ִ����ϲ��˳�\n");
	return 0;
}
