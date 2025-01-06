// Copyright (C) 2004-2024 Artifex Software, Inc.
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

/*
 * mutool -- swiss army knife of pdf manipulation tools
 */

#include <windows.h>
#include "mupdf/fitz.h"
#include <string.h>
#include <stdio.h>

int lightning_pdfinfo_main(int argc, char* argv[]);
int lightning_pdf2image_main(int argc, char* argv[]);
int lightning_image2pdf_main(int argc, char* argv[]);

static struct {
	int (*func)(int argc, char* argv[]);
	char* name;
	char* desc;
} lightning_tools[] = {
	{ lightning_pdfinfo_main, "lightning_info", "show information about pdf resources" },
	{ lightning_pdf2image_main, "lightning_pdf2image", "convert a pdf to a image or multi images" },
	{ lightning_image2pdf_main, "lightning_image2pdf", "convert a image or multi images to a pdf" },
};

static int namematch(const char* end, const char* start, const char* match) {
	size_t len = strlen(match);
	return ((end - len >= start) && (strncmp(end - len, match, len) == 0));
}

int lightning_main(int argc, char** argv) {
	if (argc <= 1) {
		fprintf(stderr, "No command name found!\n");
		return 1;
	}
	/* Check argv[1] */
	for (int i = 0; i < (int)nelem(lightning_tools); i++) {
		if (!strcmp(lightning_tools[i].name, argv[1])) {
			return lightning_tools[i].func(argc - 1, argv + 1);
		}
	}
	fprintf(stderr, "No support command name found!\n");
	return 1;
}

/* Debug Main */
int wmain(int argc, wchar_t* wargv[]) {
	char** argv = fz_argv_from_wargv(argc, wargv);
	int ret = lightning_main(argc, argv);
	fz_free_argv(argc, argv);
	return ret;
}

/* Release Main */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
	int argc;
	LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
	char** argv = fz_argv_from_wargv(argc, wargv);
	int ret = lightning_main(argc, argv);
	fz_free_argv(argc, argv);
	return ret;
}
