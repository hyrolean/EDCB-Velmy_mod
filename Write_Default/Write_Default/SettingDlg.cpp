#include "StdAfx.h"
#include "SettingDlg.h"
#include "resource.h"

wstring g_size = L"";
wstring g_packet = L"";

CSettingDlg::CSettingDlg(void)
{
}


CSettingDlg::~CSettingDlg(void)
{
}

DWORD CSettingDlg::CreateSettingDialog(HINSTANCE hInstance, HWND parentWnd)
{
	DWORD ret = 0;

	g_size = this->size;
    g_packet = this->packet;
	ret = (DWORD)DialogBox(hInstance,MAKEINTRESOURCE(IDD_DIALOG_SET), parentWnd, (DLGPROC)DlgProc );
	if( ret == IDOK ){
		this->size = g_size;
        this->packet = g_packet;
	}

	return ret;
}

static void DlgDecide(HWND hDlgWnd)
{
    {
        WCHAR buff[1024] = L"";
        GetDlgItemText(hDlgWnd,IDC_EDIT_SIZE, buff, 1024);
        g_size = buff;
    }
    {
        WCHAR buff[1024] = L"";
        GetDlgItemText(hDlgWnd,IDC_EDIT_PACKET, buff, 1024);
        g_packet = buff;
    }
}

LRESULT CALLBACK CSettingDlg::DlgProc(HWND hDlgWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
		case WM_KEYDOWN:
			if(wp == VK_RETURN){
				DlgDecide(hDlgWnd);
				EndDialog(hDlgWnd, IDOK);
			}
			break;
		case WM_INITDIALOG:
			SetDlgItemText(hDlgWnd, IDC_EDIT_SIZE, g_size.c_str());
			SetDlgItemText(hDlgWnd, IDC_EDIT_PACKET, g_packet.c_str());
			return FALSE;
        case WM_COMMAND:
			switch (LOWORD(wp)) {
				case IDOK:
					DlgDecide(hDlgWnd);
					EndDialog(hDlgWnd, IDOK);
					break;
				case IDCANCEL:
					EndDialog(hDlgWnd, IDCANCEL);
					break;
				default:
					return FALSE;
				}
		default:
			return FALSE;
	}
	return TRUE;
}

