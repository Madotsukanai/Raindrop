[English](README_en.md)

# **Raindrop**

『五月雨〜samidare〜』Ver. 1.054および『RefRain - prism memories -』V1.03.7用の、ステージセレクトなどの機能を追加するパッチです。

## 使い方

### 五月雨

[Releases](https://github.com/Madotsukanai/Raindrop/releases/latest)から`ddraw.dll`をダウンロードし、`Rain.exe`と同じディレクトリにコピーしてゲームを起動してください。

Rain.exeのSHA-256: `ae35b0ad17094bd21f86bc464504ceeafc50941e5207d75a3065e44049138211`

グラフィックスAPIは、`Rain.exe`と同じディレクトリに置く`raindrop.ini`で選択できます。

```ini
[Graphics]
API=DirectDraw
```

`API=DirectDraw`はシステムのDirectDrawを使用します。利用できない場合はD3D9へフォールバックします。`API=D3D9`にすると、起動時から内蔵D3D9実装を使用します。ファイルまたは設定がない場合は`DirectDraw`を使用します。

### RefRain

[Releases](https://github.com/Madotsukanai/Raindrop/releases/latest)から`d3d9.dll`をダウンロードし、`RefRain.exe`と同じディレクトリにコピーしてゲームを起動してください。

RefRain.exeのSHA-256: `db092baa547241a0217ca41be8538672b79094f671884f92a32c06943c857e78`

「DIVE 2 M.R.S.」にカーソルを合わせ、コンセプトリアクターに割り当てたキーを入力するとプラクティスメニューが開きます。

## ビルド（Windows）

このプロジェクトは32-bit MinGW-w64 GCCを使用します。`i686-w64-mingw32-gcc`と、MinGW-w64の`gdi32`、`dxguid`、`winmm`ライブラリが必要です。`RefRain.c`と`Smd.c`がGNU形式の属性およびインラインアセンブリを使うためです。

MinGW-w64 GCCをPATHに追加し、リポジトリのルートで実行します。

```bat
nmake /f NMakefile
```

通常版は`build\d3d9.dll`と`build\ddraw.dll`、デバッグ版のSmdは次のコマンドで`build\debug\ddraw.dll`に作成します。

```bat
nmake /f NMakefile smd-debug
```

コンパイラ名や配置が異なる場合は`MINGW_CC`で指定できます。

```bat
nmake /f NMakefile MINGW_CC=C:\mingw32\bin\i686-w64-mingw32-gcc.exe
```

### MSVCツールチェーン

Microsoftの`cl.exe`はGNU形式のインラインアセンブリを解釈できないため、Visual Studioのx86 Native Tools環境で`clang-cl`を使います。Visual Studio InstallerでLLVM/Clangツールを追加し、次を実行してください。MSVCのWindows SDK、`link.exe`およびライブラリを使ってx86 DLLを作成します。D3DX9ヘッダーは同梱の互換定義を使うため、旧DirectX SDKは不要です。

```bat
nmake /f NMakefile.msvc
```

出力先はMinGWビルドと同じです。デバッグ版は次のコマンドで作成します。

```bat
nmake /f NMakefile.msvc smd-debug
```

## 機能

- 入力遅延の軽減

### 五月雨

- Windows 11対応
- Practiceメニューへの項目追加

![Practice](https://madotsukanai.github.io/raindrop2.png)
全ステージクリア後、PracticeメニューからExtraを選択できます。Extraの行で左右キーを押すと、Area 1/4〜4/4、Boss 1/2、Boss 2/2の順に選択が切り替わり、選んだエリアまたはボス戦から練習を開始できます。
- バグの修正

リザルト表示中にリトライするとスコアが引き継がれる、敵弾警告音など一部の効果音が鳴らない、リプレイメニューに見えない選択肢がある、といったゲーム内のバグを修正します。

- 一部環境で発生するリプレイのずれを修正

リプレイ中の弾や自機の座標を、24bit浮動小数点演算で処理します。

- ショートカットキー
  - **Dキー**：設定画面で押すと、設定をデフォルトに戻します。
  - **Qキー**：ポーズ画面で押すと、タイトル画面に戻ります。
  - **Rキー**：ポーズ画面で押すと、リトライします。
  - **Alt+Enter**：ウィンドウ表示とフルスクリーン表示を切り替えます。

- ポーズ画面でBGM・SEを一時停止
- リプレイ再生中のポーズを有効化
- Practice開始時のパワー調整

ステージ2ではパワー6段階、ステージ3ではパワーMAXで開始します。

### RefRain

- ショートカットキー
  - **Qキー**：ポーズ画面で押すと、タイトル画面に戻ります。
  - **Rキー**：ポーズ画面で押すと、リトライします。

- プラクティス

ステージ、残機、M.E.F.A.2ゲージ残量、コンセプトリアクターのストック数とゲージ量を調整できます。ステージ終了後は、そのキャラクターのデスクトップ画面に戻ります。プレイ中にF1〜F5キーを押すと、各ステージへすぐに移動できます（スコアやプリズムなどはリセットされます）。

![ステージセレクト](https://madotsukanai.github.io/raindrop.png)

## 注意

- 本ソフトウェアはRebRank様とは一切関係のない非公式ツールです。**本ソフトウェアについてRebRank様や関係者へ問い合わせないでください。**
- 非公式ツールを使用する前に、セーブデータをバックアップしてください。
- **本ツールで作成したリプレイは、公式スコアボードに投稿できません。**
