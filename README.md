EpgDataCap_Bon
==============
**BonDriver based multifunctional EPG software**

Documents are stored in the 'Document' directory.  
Configuration files are stored in the 'ini' directory.

**このForkについて**

自分で使うために自由に改造しています。
プルリクエストはしないので、必要ならつまみ食いしてください。

## ■変更点(by hyrolean)
- バッチファイルに渡されたファイル名に%文字が含まれる場合に、ファイル名が化ける現象を修正しました(%文字のエスケープ処理を追加)。
- 高速チャンネルスキャンに対応しました。BonCtrl.ini に FastScan=1 を指定することによって機能します。**※1**

**※1:高速チャンネルスキャンの動作には、FastScanに対応したBonDriver([BonDriver_PTx-ST](https://github.com/hyrolean/BonDriver_PTx-ST_mod)など)が別途必要です。** 
