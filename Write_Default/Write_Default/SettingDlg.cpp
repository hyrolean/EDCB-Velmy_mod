#include "StdAfx.h"
#include "SettingDlg.h"
#include "resource.h"

CSettingDlg::CSettingDlg(void)
{}

CSettingDlg::~CSettingDlg(void)
{}

DWORD CSettingDlg::CreateSettingDialog(HINSTANCE hInstance, HWND parentWnd)
{
	return (DWORD)DialogBoxParam(hInstance,MAKEINTRESOURCE(IDD_DIALOG_SET),
		parentWnd, (DLGPROC)DlgProc, (LPARAM)this );
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
		case WM_INITDIALOG: {
			CSettingDlg *this_ = reinterpret_cast<CSettingDlg*>(lp);
			SetWindowLongPtr(hDlgWnd, GWLP_USERDATA, (LONG_PTR)this_);
			SetDlgItemText(hDlgWnd, IDC_EDIT_SIZE, this_->size.c_str());
			SetDlgItemText(hDlgWnd, IDC_EDIT_PACKET, this_->packet.c_str());
			auto setCheck = [&](int id, bool chk) {
				SendMessage(GetDlgItem(hDlgWnd, id), BM_SETCHECK,
					chk?BST_CHECKED:BST_UNCHECKED, 0);
			};
			setCheck(IDC_CHECK_FLUSHBUFFERS, this_->flush);
			setCheck(IDC_CHECK_SHRINK, this_->shrink);
			setCheck(IDC_CHECK_RESERVE, this_->reserve);
			int pchk;
			this_->triStatePriority = false;
			switch(this_->priority) {
				case 0:
					pchk=BST_UNCHECKED;
					break;
				case THREAD_PRIORITY_HIGHEST:
					pchk=BST_CHECKED;
					break;
				default:
					pchk=BST_INDETERMINATE;
					this_->triStatePriority = true;
			}
			SendMessage(GetDlgItem(hDlgWnd, IDC_CHECK_PRIORITY), BM_SETCHECK, pchk, 0);
			return FALSE;
		}
		case WM_COMMAND:
			switch (LOWORD(wp)) {
				case IDOK:
					DlgDecide(hDlgWnd);
					EndDialog(hDlgWnd, IDOK);
					break;
				case IDCANCEL:
					EndDialog(hDlgWnd, IDCANCEL);
					break;
				case IDC_CHECK_PRIORITY: {
					CSettingDlg *this_ = reinterpret_cast<CSettingDlg*>(
						GetWindowLongPtr(hDlgWnd, GWLP_USERDATA)) ;
					if(this_&&!this_->triStatePriority) {
						HWND wpchk = GetDlgItem(hDlgWnd, IDC_CHECK_PRIORITY);
						int pchk = (int) SendMessage(wpchk, BM_GETCHECK, 0, 0);
						if(pchk==BST_INDETERMINATE)
							SendMessage(wpchk, BM_SETCHECK, BST_UNCHECKED, 0);
					}
					return FALSE;
				}
				default:
					return FALSE;
				}
		default:
			return FALSE;
	}
	return TRUE;
}

void CSettingDlg::DlgDecide(HWND hDlgWnd)
{
	CSettingDlg *this_ = reinterpret_cast<CSettingDlg*>(
		GetWindowLongPtr(hDlgWnd, GWLP_USERDATA)) ;

	auto getItemText = [&](int id) {
		WCHAR buff[32] = L"";
		GetDlgItemText(hDlgWnd, id, buff, 32);
		return wstring(buff);
	};

	auto getCheck = [&](int id) {
		return SendMessage(GetDlgItem(hDlgWnd, id),
			BM_GETCHECK, 0, 0) == BST_CHECKED ;
	};

	this_->size = getItemText(IDC_EDIT_SIZE);
	this_->packet = getItemText(IDC_EDIT_PACKET);
	this_->flush = getCheck(IDC_CHECK_FLUSHBUFFERS);
	this_->shrink = getCheck(IDC_CHECK_SHRINK);
	this_->reserve = getCheck(IDC_CHECK_RESERVE);
	int pchk = (int) SendMessage(GetDlgItem(hDlgWnd, IDC_CHECK_PRIORITY),
		BM_GETCHECK, 0, 0) ;
	switch(pchk) {
		case BST_UNCHECKED:
			this_->priority=0;
			break;
		case BST_CHECKED:
			this_->priority=THREAD_PRIORITY_HIGHEST;
			break;
		default:
			/* N/A */;
	}
}

