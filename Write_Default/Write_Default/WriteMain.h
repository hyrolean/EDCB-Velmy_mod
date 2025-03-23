#pragma once

#include <memory>
#include <deque>

#include "../../Common/Util.h"
#include "../../Common/StringUtil.h"

// 既定バッファ容量は DEFAULT_BUFFER_SIZE × DEFAULT_BUFFER_PACKET のバイト容量
// 規定値 385024 bytes * 32 packets = 12320768 bytes = 最大 94 Mbits
// ※ 地デジ約16Mbpsのビットレートで最大約6秒弱分のバッファ保障容量として計上
// ※ 初期値は 385024 bytes * 2 packets = 770048 bytes の最小容量で始まり、
//    容量が足りなければ 最大 94 Mbits まで自動で随時補填される
#define DEFAULT_BUFFER_SIZE 385024
#define DEFAULT_BUFFER_PACKET 32

class CWriteMain
{
	friend class CSettingDlg;

public:
	CWriteMain(void);
	~CWriteMain(void);

	//ファイル保存を開始する
	//戻り値：
	// TRUE（成功）、FALSE（失敗）
	//引数：
	// fileName             [IN]保存ファイルフルパス（必要に応じて拡張子変えたりなど行う）
	// overWriteFlag        [IN]同一ファイル名存在時に上書きするかどうか（TRUE：する、FALSE：しない）
	// createSize           [IN]入力予想容量（188バイトTSでの容量。即時録画時など総時間未定の場合は0。延長などの可能性もあるので目安程度）
	BOOL _StartSave(
		LPCWSTR fileName,
		BOOL overWriteFlag,
		ULONGLONG createSize
		);

	//ファイル保存を終了する
	//戻り値：
	// TRUE（成功）、FALSE（失敗）
	BOOL _StopSave(
		);

	//実際に保存しているファイルパスを取得する（再生やバッチ処理に利用される）
	//filePathがNULL時は必要なサイズをfilePathSizeで返す
	//通常filePathSize=512で呼び出し
	//戻り値：
	// TRUE（成功）、FALSE（失敗）
	//引数：
	// filePath             [OUT]保存ファイルフルパス
	// filePathSize         [IN/OUT]filePathのサイズ(WCHAR単位)
	BOOL _GetSaveFilePath(
		WCHAR* filePath,
		DWORD* filePathSize
		);

	//保存用TSデータを送る
	//空き容量不足などで書き出し失敗した場合、writeSizeの値を元に
	//再度保存処理するときの送信開始地点を決める
	//戻り値：
	// TRUE（成功）、FALSE（失敗）
	//引数：
	// data                 [IN]TSデータ
	// size                 [IN]dataのサイズ
	// writeSize            [OUT]保存に利用したサイズ
	BOOL _AddTSBuff(
		BYTE* data,
		DWORD size,
		DWORD* writeSize
		);

protected: // MPWM (Massive Packet Writing Moderator) MOD3 Fixed by hyrolean
	class PACKET {
		BYTE *data_;
		size_t size_;
		size_t wrote_;
		BYTE *alloc() {
			if(data_==nullptr&&size_>0) data_ = new BYTE[size_], wrote_=0;
			return data_;
		}
	public:
		PACKET(size_t packet_size) : data_(nullptr), wrote_(0), size_(packet_size) {
			if(size_>0) alloc();
		}
		~PACKET() {
			dispose() ;
		}
		size_t write(BYTE *buffer, size_t buffer_size) {
			alloc();
			if(wrote_ >= size_) return 0;
			size_t w = min(buffer_size , size_ - wrote_);
			memcpy(&data_[wrote_], buffer, w);
			wrote_ += w ;
			return w;
		}
		void clear() { wrote_ = 0 ; }
		BYTE *data() { return alloc(); }
		size_t size() { return size_; }
		size_t wrote() { return wrote_; }
		bool allocated() { return data_!=nullptr; }
		bool full() { return wrote_ >= size_ ; }
		void dispose() {
			if(data_!=nullptr) {
				delete [] data_ ;
				data_=nullptr;
			}
			wrote_=0;
		}
	};
	vector< shared_ptr<PACKET> > packets ;
	deque<int> emptyIndices;
	deque<int> queueIndices;
	int pushingIndex, numAlloc;

	HANDLE writerThread;
	HANDLE pusherEvent, writerEvent;
	BOOL writerFailed, writerTerminated;
	CRITICAL_SECTION critical;

	BOOL WriterWriteOnePacket();
	unsigned int WriterThreadProcMain () ;
	static unsigned int __stdcall WriterThreadProc (PVOID pv) ;

	BOOL FileRescueToolSub(HWND hWnd, FILE *batSt, wstring errFile, vector<string> &recFiles);
	BOOL FileRescueToolMain(HWND hWnd, wstring batFile, wstring outPath, wstring errFile);
	static void FileRescueTool(HWND hWnd);

protected:
	HANDLE file;
	wstring savePath;
	BOOL doReserve;
	ULONGLONG reserveSize;
	ULONGLONG writerWritten;
	int writerPriority;
	BOOL doFlush;
	BOOL doShrink;
	BOOL doLazyOpen;

	struct {
		std::wstring fileName ;
		BOOL overWriteFlag ;
		ULONGLONG createSize ;
	} openParams ;
	BOOL OpenFile();

	size_t bufferSize;
	size_t maxPackets;
protected:
	BOOL GetNextFileName(wstring filePath, wstring& newPath);

};

