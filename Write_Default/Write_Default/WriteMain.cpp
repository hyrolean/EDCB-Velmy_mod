#include "StdAfx.h"
#include <process.h>
#include "WriteMain.h"

extern HINSTANCE g_instance;

#define WAIT_THREAD_MAX_TIME 30000

CWriteMain::CWriteMain(void)
{
	InitializeCriticalSection(&critical);

	file = NULL;
	pushingIndex = -1;
	reserveSize = 0;
	writerThread = INVALID_HANDLE_VALUE;
	writerTerminated = FALSE;
	writerFailed = FALSE;

	writerEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
	pusherEvent = CreateEvent(NULL, FALSE, TRUE, NULL);

	WCHAR dllPath[MAX_PATH] = L"";
	GetModuleFileName(g_instance, dllPath, MAX_PATH);

	wstring iniPath = dllPath;
	iniPath += L".ini";

	bufferSize = max( 188, (size_t)GetPrivateProfileIntW(L"SET", L"Size", DEFAULT_BUFFER_SIZE, iniPath.c_str()) );
	maxPackets = max( 2, (size_t)GetPrivateProfileIntW(L"SET", L"Packet", DEFAULT_BUFFER_PACKET, iniPath.c_str()) );

	for(int i=0;i<2;i++) { // 初期パケット登録 ( ダブルバッファリング )
		packets.push_back(shared_ptr<PACKET>(new PACKET(bufferSize)));
		emptyIndices.push_back(i);
	}

	_OutputDebugString(L"★CWriteMain Size=%d Packet=%d\n", bufferSize, maxPackets);
}


CWriteMain::~CWriteMain(void)
{
	if( file != NULL ){
		_StopSave();
	}

	bufferSize = 0;
	maxPackets = 0;

	if(writerEvent!=NULL) CloseHandle(writerEvent);
	if(pusherEvent!=NULL) CloseHandle(pusherEvent);

	emptyIndices.clear();
	queueIndices.clear();
	packets.clear();
	pushingIndex = - 1 ;

	DeleteCriticalSection(&critical);
}

//ファイル保存を開始する
//戻り値：
// TRUE（成功）、FALSE（失敗）
//引数：
// fileName             [IN]保存ファイルフルパス（必要に応じて拡張子変えたりなど行う）
// overWriteFlag        [IN]同一ファイル名存在時に上書きするかどうか（TRUE：する、FALSE：しない）
// createSize           [IN]入力予想容量（188バイトTSでの容量。即時録画時など総時間未定の場合は0。延長などの可能性もあるので目安程度）
BOOL CWriteMain::_StartSave(
	LPCWSTR fileName,
	BOOL overWriteFlag,
	ULONGLONG createSize
	)
{
	this->savePath = L"";

	wstring errMsg = L"";
	DWORD err = 0;

	wstring recFilePath = fileName;
	if( overWriteFlag == TRUE ){
		_OutputDebugString(L"★_StartSave CreateFile:%s\n", recFilePath.c_str());
		this->file = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
		if( this->file == INVALID_HANDLE_VALUE ){
			err = GetLastError();
			GetLastErrMsg(err, errMsg);
			_OutputDebugString(L"★_StartSave Err:0x%08X %s\n", err, errMsg.c_str());
			if( GetNextFileName(fileName, recFilePath) == TRUE ){
				_OutputDebugString(L"★_StartSave CreateFile:%s\n", recFilePath.c_str());
				this->file = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
			}
		}
	}else{
		_OutputDebugString(L"★_StartSave CreateFile:%s\n", recFilePath.c_str());
		this->file = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
		if( this->file == INVALID_HANDLE_VALUE ){
			err = GetLastError();
			GetLastErrMsg(err, errMsg);
			_OutputDebugString(L"★_StartSave Err:0x%08X %s\n", err, errMsg.c_str());
			if( GetNextFileName(fileName, recFilePath) == TRUE ){
				_OutputDebugString(L"★_StartSave CreateFile:%s\n", recFilePath.c_str());
				this->file = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
			}
		}
	}
	if( this->file == INVALID_HANDLE_VALUE ){
		err = GetLastError();
		GetLastErrMsg(err, errMsg);
		_OutputDebugString(L"★_StartSave Err:0x%08X %s\n", err, errMsg.c_str());
		this->file = NULL;
		return FALSE;
	}

	this->savePath = recFilePath;
	this->reserveSize = createSize;

	writerThread = (HANDLE)_beginthreadex(NULL, 0, WriterThreadProc, this, CREATE_SUSPENDED, NULL) ;
	if(writerThread != INVALID_HANDLE_VALUE) {
		writerTerminated = FALSE;
		writerFailed = FALSE;
		//SetThreadPriority(writerThread,THREAD_PRIORITY_HIGHEST);
		::ResumeThread(writerThread) ;
	}else {
		_OutputDebugString(L"★_StartSave Thread Creation Failed!!\n");
		return FALSE;
	}

	_OutputDebugString(L"★_StartSave Succeeded.\n");
	return TRUE;
}

//ファイル保存を終了する
//戻り値：
// TRUE（成功）、FALSE（失敗）
BOOL CWriteMain::_StopSave(
	)
{
	if(writerThread!=INVALID_HANDLE_VALUE) {
		writerTerminated = TRUE ;
		SetEvent(pusherEvent);
		if(::WaitForSingleObject(writerThread, WAIT_THREAD_MAX_TIME) != WAIT_OBJECT_0) {
			::TerminateThread(writerThread, 0);
			_OutputDebugString(L"★_StopSave Writer Thread Abnormal Terminated!!\n");
			writerFailed=TRUE;
		}
		::CloseHandle(writerThread) ;
		writerThread = INVALID_HANDLE_VALUE;
	}

	if( this->file != NULL ) {
		while( !writerFailed )
			if( !WriterWriteOnePacket() )
				break;
		if( !writerFailed ) {
			if( pushingIndex >= 0 ) {
				DWORD writeSize = 0;
				DWORD err = 0;
				wstring errMsg = L"";
				shared_ptr<PACKET> packet = packets[pushingIndex] ;
				if( WriteFile(this->file, packet->data(), (DWORD)packet->wrote(), &writeSize, NULL) == FALSE ){
					err = GetLastError();
					GetLastErrMsg(err, errMsg);
					_OutputDebugString(L"★_StopSave WriteFile Err:0x%08X %s", err, errMsg.c_str());
					//ファイルポインタ戻す
					LONG lpos = (LONG)writeSize;
					SetFilePointer(this->file, -lpos, NULL, FILE_CURRENT);
					writerFailed = TRUE ;
				}else {
					emptyIndices.push_back(pushingIndex);
					pushingIndex = - 1 ;
				}
			}
		}
		SetEndOfFile(this->file);
		CloseHandle(this->file);
		this->file = NULL;
	}

	return TRUE;
}

//実際に保存しているファイルパスを取得する（再生やバッチ処理に利用される）
//filePathがNULL時は必要なサイズをfilePathSizeで返す
//通常filePathSize=512で呼び出し
//戻り値：
// TRUE（成功）、FALSE（失敗）
//引数：
// filePath             [OUT]保存ファイルフルパス
// filePathSize         [IN/OUT]filePathのサイズ(WCHAR単位)
BOOL CWriteMain::_GetSaveFilePath(
	WCHAR* filePath,
	DWORD* filePathSize
	)
{
	if( filePath == NULL ){
		if( filePathSize == NULL ){
			return FALSE;
		}else{
			*filePathSize = (DWORD)this->savePath.size()+1;
		}
	}else{
		if( filePathSize == NULL ){
			return FALSE;
		}else{
			if( *filePathSize < (DWORD)this->savePath.size()+1 ){
				*filePathSize = (DWORD)this->savePath.size()+1;
				return FALSE;
			}else{
				wcscpy_s(filePath, *filePathSize, this->savePath.c_str());
			}
		}
	}
	return TRUE;
}

//保存用TSデータを送る
//空き容量不足などで書き出し失敗した場合、writeSizeの値を元に
//再度保存処理するときの送信開始地点を決める
//戻り値：
// TRUE（成功）、FALSE（失敗）
//引数：
// data                 [IN]TSデータ
// size                 [IN]dataのサイズ
// writeSize            [OUT]保存に利用したサイズ
BOOL CWriteMain::_AddTSBuff(
	BYTE* data,
	DWORD size,
	DWORD* writeSize
	)
{
	DWORD wSize=0;
	//_OutputDebugString(L"★_AddTSBuff Entering.\n");
	while(size>0) {
		if(writerFailed) {
			_OutputDebugString(L"★_AddTSBuff Leaving (writer failed).\n");
			*writeSize=wSize;
			return FALSE;
		}
		if(pushingIndex<0) {
			EnterCriticalSection(&critical);
			bool empty = false;
			if(emptyIndices.empty()) {
				if(packets.size()<maxPackets) {
					LeaveCriticalSection(&critical);
					shared_ptr<PACKET> packet(new PACKET(bufferSize));
					EnterCriticalSection(&critical);
					packets.push_back(packet);
					emptyIndices.push_back((int)packets.size()-1);
				}else {
					WaitForSingleObject(writerEvent,0);
					empty = true;
				}
			}
			LeaveCriticalSection(&critical);
			if(empty) {
				if(WaitForSingleObject(writerEvent,WAIT_THREAD_MAX_TIME)!=WAIT_OBJECT_0) {
					_OutputDebugString(L"★_AddTSBuff Leaving (failed).\n");
					*writeSize=wSize;
					return FALSE;
				}
			}
			EnterCriticalSection(&critical);
			if(!emptyIndices.empty()) {
				pushingIndex=emptyIndices.back() ;
				packets[pushingIndex]->clear();
				emptyIndices.pop_back();
			}
			LeaveCriticalSection(&critical);
		}
		if(pushingIndex<0) {
			_OutputDebugString(L"★_AddTSBuff Leaving (failed2).\n");
			*writeSize=wSize;
			return FALSE ;
		}
		shared_ptr<PACKET> packet(packets[pushingIndex]);
		size_t ln = packet->write(data,size) ;
		size-=(DWORD)ln; data+=ln; wSize+=(DWORD)ln;
		if(packet->full()) {
			EnterCriticalSection(&critical);
			queueIndices.push_back(pushingIndex);
			SetEvent(pusherEvent);
			//_OutputDebugString(L"★_AddTSBuff Packet Queue Appended[%d].\n",pushingIndex);
			pushingIndex=-1;
			LeaveCriticalSection(&critical);
		}
	}
	//_OutputDebugString(L"★_AddTSBuff Leaving.\n");
	//*writeSize=wSize;
	return TRUE;
}

BOOL CWriteMain::WriterWriteOnePacket()
{
	shared_ptr<PACKET> packet;
	int index=-1;

	//パケットをキューからひとつ取り出す
	EnterCriticalSection(&critical);
	if(!queueIndices.empty()) {
		packet = packets[index=queueIndices.front()];
		queueIndices.pop_front();
	}
	LeaveCriticalSection(&critical);
	if(index<0) return FALSE;

	DWORD wsz=0;
	if( WriteFile(this->file, packet->data(), (DWORD)packet->wrote(), &wsz, NULL) == FALSE ){
		//エラー
		DWORD err = GetLastError();
		wstring errMsg = L"";
		GetLastErrMsg(err, errMsg);
		_OutputDebugString(L"★_StopSave WriteFile Err:0x%08X %s", err, errMsg.c_str());
		//ファイルポインタ戻す
		LONG lpos = (LONG)wsz;
		SetFilePointer(this->file, -lpos, NULL, FILE_CURRENT);

		SetEndOfFile(this->file);
		CloseHandle(this->file);
		this->file = NULL;

		writerFailed = TRUE;
	}

	EnterCriticalSection(&critical);
	if(!writerFailed) {
		//パケットを未使用に
		emptyIndices.push_back(index);
		SetEvent(writerEvent);
	}else {
		//パケットをキューにひとつ戻す
		queueIndices.push_front(index);
	}
	LeaveCriticalSection(&critical);

	return TRUE;
}

unsigned int CWriteMain::WriterThreadProcMain ()
{
	//ディスクに容量を確保
	if( reserveSize > 0 ){
		LARGE_INTEGER stPos;
		stPos.QuadPart = reserveSize;
		SetFilePointerEx( this->file, stPos, NULL, FILE_BEGIN );
		SetEndOfFile( this->file );
		CloseHandle( this->file );
		this->file = _CreateFile2( savePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
		SetFilePointer( this->file, 0, NULL, FILE_BEGIN );
	}

	_OutputDebugString(L"★WriterThreadProcMain Entering.\n");

	while(!writerTerminated&&!writerFailed) {
		DWORD waitRes = WaitForSingleObject(pusherEvent, WAIT_THREAD_MAX_TIME);
		if(waitRes==WAIT_OBJECT_0) {
			while(!writerTerminated&&!writerFailed) {
				if(!WriterWriteOnePacket()) break;
			}
		}
	}

	_OutputDebugString(L"★WriterThreadProcMain Leaving.\n");

	return writerFailed ? 1 : 0 ;
}

unsigned int __stdcall CWriteMain::WriterThreadProc (PVOID pv)
{
	_endthreadex(static_cast<CWriteMain*>(pv)->WriterThreadProcMain());
	return 0;
}

BOOL CWriteMain::GetNextFileName(wstring filePath, wstring& newPath)
{
	WCHAR szPath[_MAX_PATH];
	WCHAR szDrive[_MAX_DRIVE];
	WCHAR szDir[_MAX_DIR];
	WCHAR szFname[_MAX_FNAME];
	WCHAR szExt[_MAX_EXT];
	_wsplitpath_s( filePath.c_str(), szDrive, _MAX_DRIVE, szDir, _MAX_DIR, szFname, _MAX_FNAME, szExt, _MAX_EXT );
	_wmakepath_s(  szPath, _MAX_PATH, szDrive, szDir, NULL, NULL );

	BOOL findFlag = FALSE;
	for( int i=1; i<1000; i++ ){
		WIN32_FIND_DATA findData;
		HANDLE find;

		wstring name;
		Format(name, L"%s%s-(%d)%s", szPath, szFname, i, szExt);
		newPath = name;

		find = FindFirstFile( newPath.c_str(), &findData);
		if ( find == INVALID_HANDLE_VALUE ) {
			findFlag = TRUE;
			break;
		}
		FindClose(find);
	}
	return findFlag;
}
