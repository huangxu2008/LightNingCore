#pragma once

// ���Կ���
#define GLOBAL_CHECK 0

#define WM_PDF2IMAGE			(WM_USER + 0x1000)
#define WM_IMAGE2PDF			(WM_USER + 0x1001)

#define WM_COPYDATE_PDF_INFO	(1000)

static int _check_identity_failed = 101;		// ��֤�����Ϣʧ��
static int _create_mupdf_ctx_failed = 102;		// ����fz_contextʧ��
static int _filepath_params_error = 103;		// �ļ���������
static int _filelist_params_error = 104;		// �ļ��б�����޷���ȷ��ȡ�������ļ�
static int _reg_doc_handlers_error = 105;		// ע���ĵ����ʧ��
static int _create_doc_error = 106;				// �����ĵ�ʧ��
static int _outputfile_empty = 107;				// ���·��Ϊ��

static BOOL check_identity(int process_id, HWND msg_hwnd) {
	if (!process_id || !msg_hwnd) {
		return FALSE;
	}
	if (!IsWindow(msg_hwnd)) {
		return FALSE;
	}
	DWORD check_pid = 0;
	GetWindowThreadProcessId(msg_hwnd, &check_pid);
	if (check_pid != process_id) {
		return FALSE;
	}
	TCHAR text[MAX_PATH] ={0};
	GetClassName(msg_hwnd, text, MAX_PATH);
	if (_tcsnicmp(text, "lightning", 9) != 0) {
		return FALSE;
	}
	return TRUE;
}

static void send_copydata(HWND msg_hwnd, char* data) {
	COPYDATASTRUCT cds ={0};
	cds.dwData = WM_COPYDATE_PDF_INFO;
	cds.cbData = strlen(data) + 1;
	cds.lpData = (void*)data;
	SendMessage(msg_hwnd, WM_COPYDATA, (WPARAM)0, (LPARAM)&cds);
}

static void free_char_array(char** array, int n) {
	for (size_t i = 0; i < n; i++) {
		free(array[i]);
	}
	free(array);
}
