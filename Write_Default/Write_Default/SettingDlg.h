#pragma once

#include <string>
using namespace std;

class CSettingDlg
{
public:
	CSettingDlg(void);
	~CSettingDlg(void);

	DWORD CreateSettingDialog(HINSTANCE hInstance, HWND parentWnd);

public:
	wstring size;
	wstring packet;
	bool flush;
	bool reserve;
	int priority;

protected:
	static LRESULT CALLBACK DlgProc(HWND hDlgWnd, UINT msg, WPARAM wp, LPARAM lp);
	static void DlgDecide(HWND hDlgWnd);

private:
	bool triStatePriority;
};

