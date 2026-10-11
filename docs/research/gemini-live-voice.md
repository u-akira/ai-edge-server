# Gemini Live音声会話の調査結果

調査日: 2026-10-10

## 採用するLive API仕様

- Live APIは状態を持つWebSocket接続で、低遅延の音声入出力を扱う。
- 入力音声はraw PCM、16bit、16kHz、little-endianで送信する。
- 出力音声はraw PCM、16bit、24kHz、little-endianで受信する。
- Python SDKでは`client.aio.live.connect()`で接続し、`send_realtime_input()`で音声を送信し、`session.receive()`でサーバーイベントを受信する。
- 音声応答は`server_content.model_turn.parts`内の`inline_data`として複数チャンクで届くため、1イベント内のすべてのpartを処理する。
- 音声入力のMIME typeは`audio/pcm;rate=16000`とする。

出典:

- [Gemini Live API overview](https://ai.google.dev/gemini-api/docs/live-api)
- [Live API capabilities guide](https://ai.google.dev/gemini-api/docs/live-api/capabilities)
- [Google Gen AI Python SDK](https://github.com/googleapis/python-genai)

## 発話終了の扱い

タップ式UIでは録音終了が明確なので、サーバー側の自動VADを無効化するのではなく、自動VADを有効にしたままSTOP時に`audio_stream_end=True`を送るハイブリッド方式を採用する。公式ガイドでは、この方式はサーバー側の発話開始検出とクライアント側の即時発話終了を組み合わせる方式として説明されている。

これにより、録音開始直前の音声を取りこぼしにくくしつつ、STOP後の応答開始をサーバーの無音判定待ちより早められる。完全な手動VADで`activity_start`/`activity_end`を送る方式も仕様上可能だが、CoreS3側で前置き音声を管理する必要があり、初期実装では採用しない。

出典:

- [Automatic VAD and hybrid VAD](https://ai.google.dev/gemini-api/docs/live-api/capabilities#automatic-vad-configuration)

## 既存プロジェクトとの接続方針

```text
CoreS3 -- WSS /ws/voice --> FastAPI -- google-genai Live WebSocket --> Gemini Live API
   ^                           |
   |                           +-- 16kHz PCM upload
   +-- 24kHz PCM download -----+
```

CoreS3とFastAPI間のプロトコルは、制御をJSONテキストフレーム、音声をバイナリフレームに分離する。認証トークンはURLクエリに含めず、接続直後の最初のJSONフレームで送信する。サーバーは認証成功後にLive APIセッションを作成する。

バイナリフレームは直前の制御イベントに従う。

- `recording.start`後: CoreS3からサーバーへ16kHz・16bit・mono PCM
- `response.start`後: サーバーからCoreS3へ24kHz・16bit・mono PCM
- 音声フレーム自体にはJSONヘッダーを付けず、PCMデータをそのまま送る

## 参考リポジトリとライセンス

- [AI_StackChan_Ex](https://github.com/ronron-gh/AI_StackChan_Ex)はMIT License。CoreS3向けRealtime APIのUI・設定・会話フローを参考にしたが、コードは流用しない。
- [stackchan-ai-server](https://github.com/rudyll/stackchan-ai-server)は現行版がAGPL-3.0-only。CoreS3からサーバーへのWebSocket、Gemini Liveへの音声中継、16kHz入力／24kHz出力という構成上の知見だけを参考にし、コードはコピーしない。
- [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets)はLGPL-2.1。CoreS3のWSSクライアント依存として使用する。

## CoreS3の音声制約

M5Unifiedの公式Microphone例では、`M5.Mic.record()`で録音し、再生時に`M5.Mic.end()`と`M5.Speaker.begin()`を切り替えている。公式例のコメントにも、マイクとスピーカーを同時利用できない構成が示されている。そのため初期実装では、応答開始時にマイクを停止し、応答完了またはキャンセル後にスピーカーを停止してマイクを再開する。

出典:

- [M5Unified Microphone example](https://github.com/m5stack/M5Unified/blob/master/examples/Basic/Microphone/Microphone.ino)
- [M5Unified Mic API](https://github.com/m5stack/M5Unified/blob/master/src/utility/Mic_Class.hpp)
- [M5Unified Speaker API](https://github.com/m5stack/M5Unified/blob/master/src/utility/Speaker_Class.hpp)

## セキュリティと検証方針

- Gemini APIキーはFastAPIプロセスだけが保持する。
- WebSocketはWSSを使い、CoreS3側はCA証明書でサーバー証明書を検証する。
- `setInsecure()`は新規音声サンプルでは使用しない。
- Liveセッション時間、同時接続数、1セッションの受信バイト数をサーバーで制限する。
- ログにはトークンや音声データを出力しない。
- Live APIに接続しないモックテストで、プロトコルと切断処理を検証する。
