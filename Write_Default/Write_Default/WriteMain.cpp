#include "StdAfx.h"
#include <process.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

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
	writerTerminated = TRUE;
	writerFailed = FALSE;
	writerWritten = 0;
	savePath = L"";

	writerEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
	pusherEvent = CreateEvent(NULL, FALSE, TRUE, NULL);

	WCHAR dllPath[MAX_PATH] = L"";
	GetModuleFileName(g_instance, dllPath, MAX_PATH);

	wstring iniPath = dllPath;
	iniPath += L".ini";

	bufferSize = max( 188, (size_t)GetPrivateProfileIntW(L"SET", L"Size", DEFAULT_BUFFER_SIZE, iniPath.c_str()) );
	maxPackets = max( 2, (size_t)GetPrivateProfileIntW(L"SET", L"Packet", DEFAULT_BUFFER_PACKET, iniPath.c_str()) );
	doReserve = GetPrivateProfileIntW(L"SET", L"Reserve", 1, iniPath.c_str() ) ? TRUE : FALSE ;
	writerPriority = GetPrivateProfileIntW(L"SET", L"Priority", 0, iniPath.c_str() ) ;
	doFlush = GetPrivateProfileIntW(L"SET", L"Flush", 0, iniPath.c_str() ) ? TRUE : FALSE ;
	doShrink = GetPrivateProfileIntW(L"SET", L"Shrink", 0, iniPath.c_str() ) ? TRUE : FALSE ;

	numAlloc=2;
	for(int i=0;i<numAlloc;i++) { // 初期パケット登録 ( ダブルバッファリング )
		packets.push_back(shared_ptr<PACKET>(new PACKET(bufferSize)));
		emptyIndices.push_back(i);
	}

	_OutputDebugString(L"★CWriteMain Size=%d Packet=%d Reserve=%d Priority=%d Flush=%d\n",
		bufferSize, maxPackets, doReserve, writerPriority, doFlush);
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
	pushingIndex = -1;

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
	//savePath = L"";

	wstring errMsg = L"";
	DWORD err = 0;

	wstring recFilePath = fileName;
	if( overWriteFlag == TRUE ){
		_OutputDebugString(L"★_StartSave CreateFile:%s\n", recFilePath.c_str());
		file = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
		if( file == INVALID_HANDLE_VALUE ){
			err = GetLastError();
			GetLastErrMsg(err, errMsg);
			_OutputDebugString(L"★_StartSave Err:0x%08X %s\n", err, errMsg.c_str());
			if( GetNextFileName(fileName, recFilePath) == TRUE ){
				_OutputDebugString(L"★_StartSave CreateFile:%s\n", recFilePath.c_str());
				file = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
			}
		}
	}else{
		_OutputDebugString(L"★_StartSave CreateFile:%s\n", recFilePath.c_str());
		file = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
		if( file == INVALID_HANDLE_VALUE ){
			err = GetLastError();
			GetLastErrMsg(err, errMsg);
			_OutputDebugString(L"★_StartSave Err:0x%08X %s\n", err, errMsg.c_str());
			if( GetNextFileName(fileName, recFilePath) == TRUE ){
				_OutputDebugString(L"★_StartSave CreateFile:%s\n", recFilePath.c_str());
				file = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
			}
		}
	}
	if( file == INVALID_HANDLE_VALUE ){
		err = GetLastError();
		GetLastErrMsg(err, errMsg);
		_OutputDebugString(L"★_StartSave Err:0x%08X %s\n", err, errMsg.c_str());
		file = NULL;
		return FALSE;
	}

	if(writerFailed && writerWritten>0) {
		// 容量不足やHDDの不意な切断など、前回出力失敗した際の後処理
		if(savePath != L"") {
			wstring failureLogPath = recFilePath + L".Write_Default.err";
			FILE *erst = NULL ;
			errno_t err = _wfopen_s(&erst,failureLogPath.c_str(),L"a+t");
			if(!err&&erst!=NULL) {
				string strOrg, strRec;
				WtoA(savePath, strOrg);
				WtoA(recFilePath, strRec);
				string buff;
				// 最後に書いたバイトサイズなどの情報をログに書き残しておく
				// （後にサイズを元に損傷ファイルを手動で復元する際のヒント）
				fprintf(erst, "ファイル \"%s\" は、 %I64u バイト書き込み後、"
					"出力に失敗した為、ファイル \"%s\" へリダイレクトされました。\n",
					strOrg.c_str(), (unsigned __int64)writerWritten, strRec.c_str());
				fclose(erst);
			}
		}
		if(!createSize && reserveSize>=writerWritten)
			createSize = reserveSize - writerWritten;
	}

	writerWritten = 0 ;
	savePath = recFilePath;
	reserveSize = createSize;
	writerFailed = FALSE;

	writerThread = (HANDLE)_beginthreadex(NULL, 0, WriterThreadProc, this, CREATE_SUSPENDED, NULL) ;
	if(writerThread != INVALID_HANDLE_VALUE) {
		writerTerminated = FALSE;
		SetEvent(pusherEvent);
		if(writerPriority) SetThreadPriority(writerThread, writerPriority);
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

	if( file != NULL ) {
		while( !writerFailed )
			if( !WriterWriteOnePacket() )
				break;
		if( !writerFailed ) {
			if( pushingIndex >= 0 ) {
				DWORD writeSize = 0;
				DWORD err = 0;
				wstring errMsg = L"";
				shared_ptr<PACKET> packet = packets[pushingIndex] ;
				if( WriteFile(file, packet->data(), (DWORD)packet->wrote(), &writeSize, NULL) == FALSE ){
					err = GetLastError();
					GetLastErrMsg(err, errMsg);
					_OutputDebugString(L"★_StopSave WriteFile Err:0x%08X %s", err, errMsg.c_str());
					//ファイルポインタ戻す
					LONG lpos = (LONG)writeSize;
					SetFilePointer(file, -lpos, NULL, FILE_CURRENT);
					writeSize = 0 ;
					writerFailed = TRUE ;
				}else {
					emptyIndices.push_back(pushingIndex);
					pushingIndex = - 1 ;
				}
				writerWritten += writeSize ;
			}
		}
		SetEndOfFile(file);
		CloseHandle(file);
		file = NULL;
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
			*filePathSize = (DWORD)savePath.size()+1;
		}
	}else{
		if( filePathSize == NULL ){
			return FALSE;
		}else{
			if( *filePathSize < (DWORD)savePath.size()+1 ){
				*filePathSize = (DWORD)savePath.size()+1;
				return FALSE;
			}else{
				wcscpy_s(filePath, *filePathSize, savePath.c_str());
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
		if(writerFailed||writerTerminated) {
			_OutputDebugString(L"★_AddTSBuff Leaving (writer was %s).\n",writerFailed?L"failed":L"terminated");
			*writeSize=wSize;
			return FALSE;
		}
		if(pushingIndex<0) {
			EnterCriticalSection(&critical);
			if(!writerFailed&&!writerTerminated) {
				bool empty = false;
				if(emptyIndices.empty()) {
					if(packets.size()<maxPackets) {
						LeaveCriticalSection(&critical);
						shared_ptr<PACKET> packet(new PACKET(bufferSize));
						EnterCriticalSection(&critical);
						packets.push_back(packet);
						emptyIndices.push_back((int)packets.size()-1);
						numAlloc++;
					}else {
						WaitForSingleObject(writerEvent,0);
						empty = true;
					}
				}
				LeaveCriticalSection(&critical);
				if(empty) {
					if(WaitForSingleObject(writerEvent,WAIT_THREAD_MAX_TIME)!=WAIT_OBJECT_0||writerFailed||writerTerminated) {
						_OutputDebugString(L"★_AddTSBuff Leaving (failed waiting writer event).\n");
						*writeSize=wSize;
						return FALSE;
					}
				}
				EnterCriticalSection(&critical);
				if(!emptyIndices.empty()) {
					pushingIndex=emptyIndices.back() ;
					emptyIndices.pop_back();
					packets[pushingIndex]->clear();
					if(packets[pushingIndex]->allocated())
						numAlloc--;
				}
			}
			LeaveCriticalSection(&critical);
		}
		if(pushingIndex<0) {
			_OutputDebugString(L"★_AddTSBuff Leaving (failed allocating memory).\n");
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
	if( file == NULL ) {
		writerFailed=TRUE;
		return FALSE;
	}

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
	if( WriteFile(file, packet->data(), (DWORD)packet->wrote(), &wsz, NULL) == FALSE ){
		//エラー
		DWORD err = GetLastError();
		wstring errMsg = L"";
		GetLastErrMsg(err, errMsg);
		_OutputDebugString(L"★_StopSave WriteFile Err:0x%08X %s", err, errMsg.c_str());
		//ファイルポインタ戻す
		LONG lpos = (LONG)wsz;
		SetFilePointer(file, -lpos, NULL, FILE_CURRENT);
		wsz = 0 ;
		writerFailed = TRUE;
	}else if(doFlush) {
		if(FlushFileBuffers(file) == FALSE) {
			wsz=0;
			writerFailed = TRUE;
		}
	}
	writerWritten += wsz ;

	EnterCriticalSection(&critical);
	if(!writerFailed) {
		bool beShrunk=false;
		if(doShrink) {
			//バッファ容量に余裕があれば領域を開放する
			size_t numBorder = max(2,queueIndices.size()) ;
			beShrunk = numAlloc>=numBorder;
		}
		//パケットを未使用に
		if(beShrunk) {
			packets[index]->dispose();
			emptyIndices.push_front(index);
		}
		else {
			emptyIndices.push_back(index);
			numAlloc++;
		}
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
	if( doReserve && reserveSize > 0 ){
		LARGE_INTEGER stPos;
		stPos.QuadPart = reserveSize;
		SetFilePointerEx( file, stPos, NULL, FILE_BEGIN );
		SetEndOfFile( file );
		CloseHandle( file );
		file = _CreateFile2( savePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
		SetFilePointer( file, 0, NULL, FILE_BEGIN );
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

	EnterCriticalSection(&critical);
	SetEvent(writerEvent);
	writerTerminated=TRUE;
	LeaveCriticalSection(&critical);

	_OutputDebugString(L"★WriterThreadProcMain Leaving.\n");

	return writerFailed ? 1 : 0 ;
}

unsigned int __stdcall CWriteMain::WriterThreadProc (PVOID pv)
{
	_endthreadex(static_cast<CWriteMain*>(pv)->WriterThreadProcMain());
	return static_cast<CWriteMain*>(pv)->writerFailed ? 1 : 0 ;
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

	for( int i=1; i<1000; i++ ){
		wstring name;
		Format(name, L"%s%s-(%d)%s", szPath, szFname, i, szExt);

		if(!PathFileExists(name.c_str())) {
			newPath = name;
			return TRUE;
		}
	}
	return FALSE;
}
