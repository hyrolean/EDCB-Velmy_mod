#include "StdAfx.h"
#include <process.h>
#include <commdlg.h>
#include <shlObj.h>
#include <ShellAPI.h>
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
	doLazyOpen = GetPrivateProfileIntW(L"SET", L"LazyOepn", 0, iniPath.c_str() ) ? TRUE : FALSE ;

	numAlloc=2;
	for(int i=0;i<numAlloc;i++) { // �����p�P�b�g�o�^ ( �_�u���o�b�t�@�����O )
		packets.push_back(shared_ptr<PACKET>(new PACKET(bufferSize)));
		emptyIndices.push_back(i);
	}

	_OutputDebugString(L"��CWriteMain Size=%d Packet=%d Reserve=%d Priority=%d Flush=%d Shrink=%d\n",
		bufferSize, maxPackets, doReserve, writerPriority, doFlush, doShrink);
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


BOOL CWriteMain::OpenFile()
{
	wstring errMsg = L"";
	DWORD err = 0;

	wstring recFilePath = openParams.fileName;
	HANDLE newFile = NULL ;
	if( openParams.overWriteFlag ){
		_OutputDebugString(L"��_StartSave CreateFile:%s\n", recFilePath.c_str());
		newFile = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
		if( newFile == INVALID_HANDLE_VALUE ){
			err = GetLastError();
			GetLastErrMsg(err, errMsg);
			_OutputDebugString(L"��_StartSave Err:0x%08X %s\n", err, errMsg.c_str());
			if( GetNextFileName(openParams.fileName, recFilePath) ){
				_OutputDebugString(L"��_StartSave CreateFile:%s\n", recFilePath.c_str());
				newFile = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
			}
		}
	}else{
		_OutputDebugString(L"��_StartSave CreateFile:%s\n", recFilePath.c_str());
		newFile = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
		if( newFile == INVALID_HANDLE_VALUE ){
			err = GetLastError();
			GetLastErrMsg(err, errMsg);
			_OutputDebugString(L"��_StartSave Err:0x%08X %s\n", err, errMsg.c_str());
			if( GetNextFileName(openParams.fileName, recFilePath) ){
				_OutputDebugString(L"��_StartSave CreateFile:%s\n", recFilePath.c_str());
				newFile = _CreateFile2( recFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
			}
		}
	}
	if( newFile == INVALID_HANDLE_VALUE ){
		err = GetLastError();
		GetLastErrMsg(err, errMsg);
		_OutputDebugString(L"��_StartSave Err:0x%08X %s\n", err, errMsg.c_str());
		file = NULL;
		return FALSE;
	}

	if(writerFailed && writerWritten>0) {
		// �e�ʕs����HDD�̕s�ӂȐؒf�ȂǁA�O��o�͎��s�����ۂ̌㏈��
		if(savePath != L"") {
			wstring failureLogPath = recFilePath + L".Write_Default.err";
			FILE *erst = NULL ;
			errno_t err = _wfopen_s(&erst,failureLogPath.c_str(),L"a+t");
			if(!err&&erst!=NULL) {
				string strOrg, strRec;
				WtoA(savePath, strOrg);
				WtoA(recFilePath, strRec);
				string buff;
				// �Ō�ɏ������o�C�g�T�C�Y�Ȃǂ̏������O�ɏ����c���Ă���
				// �i��ɃT�C�Y�����ɑ����t�@�C�����蓮�ŕ�������ۂ̃q���g�j
				fprintf(erst, "�t�@�C�� \"%s\" �́A %I64u �o�C�g�������݌�A"
					"�o�͂Ɏ��s�����ׁA�t�@�C�� \"%s\" �փ��_�C���N�g����܂����B\n",
					strOrg.c_str(), (unsigned __int64)writerWritten, strRec.c_str());
				fclose(erst);
			}
		}
		if(!openParams.createSize && reserveSize>=writerWritten)
			openParams.createSize = reserveSize - writerWritten;
	}

	writerWritten = 0 ;
	savePath = recFilePath;
	reserveSize = openParams.createSize;
	writerFailed = FALSE;

	file = newFile;
	return TRUE ;
}


//�t�@�C���ۑ����J�n����
//�߂�l�F
// TRUE�i�����j�AFALSE�i���s�j
//�����F
// fileName             [IN]�ۑ��t�@�C���t���p�X�i�K�v�ɉ����Ċg���q�ς�����ȂǍs���j
// overWriteFlag        [IN]����t�@�C�������ݎ��ɏ㏑�����邩�ǂ����iTRUE�F����AFALSE�F���Ȃ��j
// createSize           [IN]���͗\�z�e�ʁi188�o�C�gTS�ł̗e�ʁB�����^�掞�ȂǑ����Ԗ���̏ꍇ��0�B�����Ȃǂ̉\��������̂Ŗڈ����x�j
BOOL CWriteMain::_StartSave(
	LPCWSTR fileName,
	BOOL overWriteFlag,
	ULONGLONG createSize
	)
{
	//savePath = L"";

	openParams.fileName = fileName ;
	openParams.overWriteFlag = overWriteFlag ;
	openParams.createSize = createSize ;

	BOOL result = TRUE ;

	if(!doLazyOpen) result = OpenFile();
	else if(!overWriteFlag) {
		if(PathFileExists(fileName)) {
			std::wstring nextFileName = L"";
			if( GetNextFileName(fileName, nextFileName) ){
				openParams.fileName = nextFileName ;
			}
		}
	}

	if(!result) return FALSE ;

	writerThread = (HANDLE)_beginthreadex(NULL, 0, WriterThreadProc, this, CREATE_SUSPENDED, NULL) ;
	if(writerThread != INVALID_HANDLE_VALUE) {
		writerTerminated = FALSE;
		SetEvent(pusherEvent);
		if(writerPriority) SetThreadPriority(writerThread, writerPriority);
		::ResumeThread(writerThread) ;
	}else {
		_OutputDebugString(L"��_StartSave Thread Creation Failed!!\n");
		return FALSE;
	}

	_OutputDebugString(L"��_StartSave Succeeded.\n");
	return TRUE;
}

//�t�@�C���ۑ����I������
//�߂�l�F
// TRUE�i�����j�AFALSE�i���s�j
BOOL CWriteMain::_StopSave(
	)
{
	if(writerThread!=INVALID_HANDLE_VALUE) {
		writerTerminated = TRUE ;
		SetEvent(pusherEvent);
		if(::WaitForSingleObject(writerThread, WAIT_THREAD_MAX_TIME) != WAIT_OBJECT_0) {
			::TerminateThread(writerThread, 0);
			_OutputDebugString(L"��_StopSave Writer Thread Abnormal Terminated!!\n");
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
					_OutputDebugString(L"��_StopSave WriteFile Err:0x%08X %s", err, errMsg.c_str());
					//�t�@�C���|�C���^�߂�
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

//���ۂɕۑ����Ă���t�@�C���p�X���擾����i�Đ���o�b�`�����ɗ��p�����j
//filePath��NULL���͕K�v�ȃT�C�Y��filePathSize�ŕԂ�
//�ʏ�filePathSize=512�ŌĂяo��
//�߂�l�F
// TRUE�i�����j�AFALSE�i���s�j
//�����F
// filePath             [OUT]�ۑ��t�@�C���t���p�X
// filePathSize         [IN/OUT]filePath�̃T�C�Y(WCHAR�P��)
BOOL CWriteMain::_GetSaveFilePath(
	WCHAR* filePath,
	DWORD* filePathSize
	)
{
	std::wstring path = file==NULL ? openParams.fileName : savePath ;
	if( filePath == NULL ){
		if( filePathSize == NULL ){
			return FALSE;
		}else{
			*filePathSize = (DWORD)path.size()+1;
		}
	}else{
		if( filePathSize == NULL ){
			return FALSE;
		}else{
			if( *filePathSize < (DWORD)path.size()+1 ){
				*filePathSize = (DWORD)path.size()+1;
				return FALSE;
			}else{
				wcscpy_s(filePath, *filePathSize, path.c_str());
			}
		}
	}
	return TRUE;
}

//�ۑ��pTS�f�[�^�𑗂�
//�󂫗e�ʕs���Ȃǂŏ����o�����s�����ꍇ�AwriteSize�̒l������
//�ēx�ۑ���������Ƃ��̑��M�J�n�n�_�����߂�
//�߂�l�F
// TRUE�i�����j�AFALSE�i���s�j
//�����F
// data                 [IN]TS�f�[�^
// size                 [IN]data�̃T�C�Y
// writeSize            [OUT]�ۑ��ɗ��p�����T�C�Y
BOOL CWriteMain::_AddTSBuff(
	BYTE* data,
	DWORD size,
	DWORD* writeSize
	)
{
	DWORD wSize=0;
	//_OutputDebugString(L"��_AddTSBuff Entering.\n");
	while(size>0) {
		if(writerFailed||writerTerminated) {
			_OutputDebugString(L"��_AddTSBuff Leaving (writer was %s).\n",writerFailed?L"failed":L"terminated");
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
						_OutputDebugString(L"��_AddTSBuff Leaving (failed waiting writer event).\n");
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
			_OutputDebugString(L"��_AddTSBuff Leaving (failed allocating memory).\n");
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
			//_OutputDebugString(L"��_AddTSBuff Packet Queue Appended[%d].\n",pushingIndex);
			pushingIndex=-1;
			LeaveCriticalSection(&critical);
		}
	}
	//_OutputDebugString(L"��_AddTSBuff Leaving.\n");
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

	//�p�P�b�g���L���[����ЂƂ��o��
	EnterCriticalSection(&critical);
	if(!queueIndices.empty()) {
		packet = packets[index=queueIndices.front()];
		queueIndices.pop_front();
	}
	LeaveCriticalSection(&critical);
	if(index<0) return FALSE;

	DWORD wsz=0;

	BYTE *data = (BYTE*) packet->data() ;
	DWORD len = (DWORD) packet->wrote() ;
	BOOL bWrite = FALSE ;

          while(len>0) {
            DWORD wSize = 0 ;
            bWrite = WriteFile(file, data, len, &wSize, 0) ;
            if(!bWrite) break ;
            else data += wSize , len -= wSize , wsz += wSize ;
          }

	if( bWrite == FALSE ){
		//�G���[
		DWORD err = GetLastError();
		wstring errMsg = L"";
		GetLastErrMsg(err, errMsg);
		_OutputDebugString(L"��_StopSave WriteFile Err:0x%08X %s", err, errMsg.c_str());
		//�t�@�C���|�C���^�߂�
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
			//�o�b�t�@�e�ʂɗ]�T������Η̈���J������
			auto numBorder = max(2,queueIndices.size()) ;
			beShrunk = numAlloc>=static_cast<decltype(numAlloc)>(numBorder);
		}
		//�p�P�b�g�𖢎g�p��
		if(beShrunk) {
			LeaveCriticalSection(&critical);
			packets[index]->dispose();
			EnterCriticalSection(&critical);
			emptyIndices.push_front(index);
		}
		else {
			emptyIndices.push_back(index);
			numAlloc++;
		}
		SetEvent(writerEvent);
	}else {
		//�p�P�b�g���L���[�ɂЂƂ߂�
		queueIndices.push_front(index);
	}
	LeaveCriticalSection(&critical);

	return TRUE;
}

unsigned int CWriteMain::WriterThreadProcMain ()
{
	//�t�@�C���̒x���I�[�v������
	if(doLazyOpen) {
		if(!OpenFile()) {
			writerFailed = true ;
			writerTerminated = true ;
			return 1 ;
		}
	}

	//�f�B�X�N�ɗe�ʂ��m��
	if( doReserve && reserveSize > 0 ){
		LARGE_INTEGER stPos;
		stPos.QuadPart = reserveSize;
		SetFilePointerEx( file, stPos, NULL, FILE_BEGIN );
		SetEndOfFile( file );
		CloseHandle( file );
		file = _CreateFile2( savePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
		SetFilePointer( file, 0, NULL, FILE_BEGIN );
	}

	_OutputDebugString(L"��WriterThreadProcMain Entering.\n");

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

	_OutputDebugString(L"��WriterThreadProcMain Leaving.\n");

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


static wstring batCharEscSeqW(wstring src)
{
	wstring r;
	for(auto c : src) {
		if(c==L'%') r += L"%%";
		else r+=c;
	}
	return r;
}

static string batCharEscSeq(string src)
{
	wstring strW;
	AtoW(src, strW);
	string r;
	WtoA(batCharEscSeqW(strW), r);
	return r;
}

BOOL CWriteMain::FileRescueToolSub(HWND hWnd, FILE *batSt, wstring errFile, vector<string> &recFiles)
{
	FILE *errSt=NULL;
	errno_t err = _wfopen_s(&errSt,errFile.c_str(),L"rt");
	BOOL result=FALSE;
	if(!err&&errSt!=NULL) {
		char orgPath[MAX_PATH], rdrctPath[MAX_PATH];
		unsigned __int64 orgSize;
		for (;;) {
			int n = fscanf_s(errSt,
				"�t�@�C�� \"%[^\"]\" �́A %I64u �o�C�g�������݌�A"
				"�o�͂Ɏ��s�����ׁA�t�@�C�� \"%[^\"]\" �փ��_�C���N�g����܂����B\n",
				orgPath, MAX_PATH, &orgSize, rdrctPath, MAX_PATH);
			if(n==3) {
				wstring prevErrFile, orgW;
				AtoW(orgPath, orgW);
				prevErrFile = orgW+L".Write_Default.err";
				if(PathFileExists(prevErrFile.c_str())) {
					if(!FileRescueToolSub(hWnd,batSt,prevErrFile,recFiles))
					{ result = FALSE; break; }
				}else
					recFiles.push_back(orgPath);
				fprintf(batSt,"fsutil file seteof \"%s\" %I64u\n",batCharEscSeq(orgPath).c_str(),orgSize);
				recFiles.push_back(rdrctPath);
				result=TRUE;
			}else if(!result) {
				wstring mess ;
				Format(mess, L"�t�@�C�� \"%s\" �̓��e�𐳏�Ɍ��o�ł��܂���ł����B", errFile.c_str());
				MessageBox(hWnd, mess.c_str(), L"�^��t�@�C�����X�L���[", MB_ICONSTOP|MB_OK);
			}else break;
		}
		fclose(errSt);
	}else {
		wstring mess ;
		Format(mess, L"�t�@�C�� \"%s\" ���I�[�v���ł��܂���B", errFile.c_str());
		MessageBox(hWnd, mess.c_str(), L"�^��t�@�C�����X�L���[", MB_ICONSTOP|MB_OK);
	}
	return result;
}

BOOL CWriteMain::FileRescueToolMain(HWND hWnd, wstring batFile, wstring outPath, wstring errFile)
{
	FILE *batSt=NULL;
	errno_t err = _wfopen_s(&batSt,batFile.c_str(),L"a+t");
	BOOL result=FALSE;
	if(!err&&batSt!=NULL) {
		vector<string> recFiles;
		result = FileRescueToolSub(hWnd, batSt, errFile, recFiles);
		if(result) {
			fprintf(batSt, "copy /b ");
			string outFile="";
			for(size_t i=0;i<recFiles.size();i++) {
				if(i>0) fprintf(batSt, "+ ");
				else /*i==0*/ {
					WCHAR szPath[_MAX_PATH];
					WCHAR szDrive[_MAX_DRIVE];
					WCHAR szDir[_MAX_DIR];
					WCHAR szFname[_MAX_FNAME];
					WCHAR szExt[_MAX_EXT];
					wstring strW,strNw;
					AtoW(recFiles[i], strW);
					_wsplitpath_s( strW.c_str(), szDrive, _MAX_DRIVE, szDir, _MAX_DIR, szFname, _MAX_FNAME, szExt, _MAX_EXT );
					_wmakepath_s(  szPath, _MAX_PATH, szDrive, szDir, NULL, NULL );
					Format(strW,L"%s\\%s%s",outPath.c_str(),szFname,szExt);
					strNw=strW;
					if(PathFileExists(strNw.c_str()))
						GetNextFileName(strW,strNw);
					WtoA(strNw, outFile);
				}
				fprintf(batSt,"\"%s\" ",batCharEscSeq(recFiles[i]).c_str());
			}
			fprintf(batSt,"\"%s\"\n",batCharEscSeq(outFile).c_str());
		}else {
			fprintf(batSt,"REM ERROR occurred!!\n");
			fclose(batSt);
			return FALSE;
		}
		fclose(batSt);
	}else {
		wstring mess ;
		Format(mess, L"�o�b�`�t�@�C�� \"%s\" ���쐬�ł��܂���ł����B", batFile.c_str());
		MessageBox(hWnd, mess.c_str(), L"�^��t�@�C�����X�L���[", MB_ICONSTOP|MB_OK);
		return FALSE;
	}
	return TRUE;
}

void CWriteMain::FileRescueTool(HWND hWnd)
{
	const decltype(OPENFILENAME::nMaxFile) MAX_RESULTS = 8192;
	WCHAR strErrFiles[MAX_RESULTS]=L"";
	OPENFILENAME ofn={0};
	ofn.lStructSize = sizeof (OPENFILENAME);
	ofn.hwndOwner = hWnd;
	ofn.lpstrFilter = 	L"Error log (*.Write_Default.err)\0*.Write_Default.err\0"
					L"All files (*.*)\0*.*\0\0";
	ofn.lpstrTitle = L"�Ō�ɏo�͂��ꂽ�G���[���O�̑I��(�����I����)";
	ofn.lpstrFile = strErrFiles;
	ofn.nMaxFile = MAX_RESULTS;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT | OFN_EXPLORER ;

	if (GetOpenFileName(&ofn) == 0) {
		return ;
	}

	deque<wstring> errFiles;
	wstring dir = L"";
	bool nl=true;
	for(int len=0;len<MAX_RESULTS-1;len++) {
		if(nl) { if(dir==L"") dir = &strErrFiles[len]; errFiles.push_back(&strErrFiles[len]); nl=false; }
		if(!strErrFiles[len]) {
			if(!strErrFiles[len+1]) break ;
			nl=true;
		}
	}
	if(errFiles.size()>=2) {
		dir+=L"\\";
		errFiles.pop_front();
	}else {
		dir=L"";
	}

	WCHAR strOutPath[MAX_PATH]=L"";
	BROWSEINFO  binfo={0} ;
	LPITEMIDLIST idlist;

	binfo.hwndOwner=hWnd;
	binfo.pidlRoot=NULL;
	binfo.pszDisplayName=strOutPath;
	binfo.lpszTitle=L"�^��t�@�C�����X�L���[��̑I��";
	binfo.ulFlags=BIF_RETURNONLYFSDIRS;
	binfo.iImage=(int)NULL;
	idlist=SHBrowseForFolder(&binfo);

	if(idlist!=NULL) {
		SHGetPathFromIDList(idlist,strOutPath);
		CoTaskMemFree(idlist);
	}

	if(!strOutPath[0])
		return;

	wstring batFile = strOutPath ;
	batFile += L"\\Write_Default.rescue.bat" ;
	if(PathFileExists(batFile.c_str())) {
		wstring newBatFile = batFile;
		CWriteMain().GetNextFileName(batFile,newBatFile);
		batFile = newBatFile;
	}

	for(auto errFile : errFiles) {
		wstring errFullpath = dir + errFile ;
		if(!PathFileExists(errFullpath.c_str())) {
			wstring mess ;
			Format(mess, L"�G���[���O \"%s\" �́A���݂��܂���B", errFullpath.c_str());
			MessageBox(hWnd, mess.c_str(), L"�^��t�@�C�����X�L���[", MB_ICONSTOP|MB_OK);
			return ;
		}
		if(!CWriteMain().FileRescueToolMain(hWnd, batFile, strOutPath, errFullpath.c_str()))
			return ;
	}

	FILE *batSt=NULL;
	errno_t err = _wfopen_s(&batSt,batFile.c_str(),L"a+t");
	if(!err&&batSt!=NULL) {
		fprintf(batSt,"pause\n");
		fclose(batSt);
		#if 0 // �p�X�Ɋ��ʂ��܂܂�Ă���Ƃ��܂����삵�Ȃ�
		wstring cmd = L"\"" + batFile + L"\"" ;
		_wsystem(cmd.c_str());
		#else
		SHELLEXECUTEINFO info ;
		ZeroMemory(&info,sizeof(info));
		info.cbSize = sizeof(SHELLEXECUTEINFO) ;
		//info.fMask |= SEE_MASK_NOCLOSEPROCESS ;
		info.hwnd = hWnd ;
		info.lpFile = batFile.c_str();
		info.nShow = SW_SHOWNORMAL;
		ShellExecuteEx(&info);
		#endif
	}
}
