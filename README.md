[English](README_en.md)

# **Raindrop**

『五月雨〜samidare〜』Ver. 1.054及び『RefRain - prism memories -』V1.03.7用の、ステージセレクトなどの機能を追加するパッチです。

## 使い方

### 五月雨

[公式サイト](https://www.rebrank.org/download.htm)からアップデートパッチをダウンロードしてゲームに適用し、[Releases](https://github.com/Madotsukanai/Raindrop/releases/latest)から`ddraw.dll`をダウンロードしてから`Rain.exe` (SHA-256: `ae35b0ad17094bd21f86bc464504ceeafc50941e5207d75a3065e44049138211`)と同じディレクトリにコピーしてゲームを起動してください。

グラフィックスバックエンドは、`Rain.exe`と同じディレクトリに置く`raindrop.ini`で選択できます。

```ini
[General]
PrEx=1
UpdateCheck=1
UpdateSkip=
[Graphics]
Backend=wgpu
VSync=auto
```

| 項目 | 既定値 | 内容 |
|---|---|---|
| `PrEx` | `1` | プラクティス拡張（ボスシーク、プラクティスメニュー、EXプラクティス）の有効/無効。 |
| `UpdateCheck` | `1` | 起動時にGitHub Releasesを確認し、最新版があれば更新するか尋ねる。 |
| `UpdateSkip` | （空） | 更新を断ったバージョンタグが自動記録され、そのバージョンでは再び尋ねない。手動設定不要。 |
| `Backend` | `wgpu` | グラフィックスバックエンドの選択（下記参照）。 |
| `VSync` | `auto` | 内蔵グラフィックス実装の表示同期（下記参照）。 |

真偽値は`PrEx` / `UpdateCheck` / `VSync` / 環境変数`SMD_VSYNC`のいずれも`1` / `on` / `true` / `yes`（真）と`0` / `off` / `false` / `no`（偽）を同じ書式で使用できます（大文字小文字を区別しません）。`VSync`はさらに`auto`を指定できます。

存在しない項目は起動時に自動で追加されます。

`Backend=wgpu`（既定）はwgpu上に実装した内蔵DirectDraw7/Direct3D7エミュレーション（DirectDraw/Direct3D7をwgpuで置き換えるRust実装）を使用します。wgpuを初期化できない場合のみ、実行時に内蔵D3D9実装へフォールバックします（`raindrop.ini`は書き換えません）。`Backend=system`にすると、システムのd3d9.dll上で動く内蔵D3D9実装を起動時から使用します。ファイルまたは設定がない場合は`wgpu`を使用します。<br>
Wine/Protonではデフォルトの`wgpu`のままにすることを推奨します（システムのDirectDraw/D3D7はWineでは利用できないため）。

内蔵グラフィックス実装の表示同期は`VSync`で選択できます。

```ini
[Graphics]
Backend=wgpu
VSync=auto
```

`VSync=auto`は、Windowsでは59〜61Hzの表示なら垂直同期あり、Wine/Protonでは垂直同期なし（60fpsはゲーム自身のフレームタイマで維持されます）です。Wine/ProtonではX11/Waylandのコンポジタが既に画面の垂直同期に合わせて合成するため、Present内でも垂直同期を待つと入力遅延が1フレーム増えるためです。`VSync=on`で常に垂直同期あり、`VSync=off`で常に垂直同期なしになります。垂直同期なしでは、ティアリングよりも入力遅延を優先します。

### RefRain

[Releases](https://github.com/Madotsukanai/Raindrop/releases/latest)から`d3d9.dll`をダウンロードし、ディスク版`RefRain.exe` (SHA-256: `db092baa547241a0217ca41be8538672b79094f671884f92a32c06943c857e78`)またはSteam版`RefRain.exe` (SHA-256: 1941799d114dacbbd83a3d40c69ad4f5063ee4a175c591dc155d5deca4a6e959)と同じディレクトリにコピーしてゲームを起動してください。

`RefRain.exe`と同じディレクトリの`raindrop.ini`は自動更新の設定のみを使用します。ファイルや項目がなければ自動で追加されます。

```ini
[General]
UpdateCheck=1
UpdateSkip=
```

「DIVE 2 M.R.S.」にカーソルを合わせ、コンセプトリアクターを入力しながら決定するとプラクティスメニューが開きます。

## ビルド
### Windows
Visual Studioのx86 Native Tools環境で`clang-cl`を使います。Visual Studio InstallerでLLVM/Clangツールを追加し、リポジトリのルートで次を実行してください。`NMakefile`はMSVCのWindows SDK、`link.exe`およびライブラリを使ってx86 DLLを作成します。D3DX9ヘッダーは同梱の互換定義を使うため、旧DirectX SDKは不要です。

```bat
nmake /f NMakefile smd            :: 五月雨用 build\ddraw.dll
nmake /f NMakefile smd-debug      :: 五月雨デバッグ版 build\debug\ddraw.dll (SMD_DEBUG_BUILD, ログ有効)
nmake /f NMakefile refrain        :: RefRain用 build\d3d9.dll
nmake /f NMakefile refrain-debug  :: RefRainデバッグ版 build\debug\d3d9.dll (REFRAIN_DEBUG_BUILD, ログ有効)
nmake /f NMakefile debug          :: すべてのデバッグ版 (refrain-debug + smd-debug)
nmake /f NMakefile                :: refrain + smd
```

### Linux

MinGW-w64（32bit）とRust（cargo）でクロスコンパイルします。`i686-w64-mingw32-gcc`と`cargo`をインストールしてから、リポジトリのルートで`make`を実行してください。`ddraw.dll`にはRustクレート（wgpu）が静的にリンクされ、`rust/`配下は自動でビルドされます。`make clean`は`build/`とRustクレートのビルド成果物を両方削除します。

```sh
# Arch / CachyOS
sudo pacman -S mingw-w64-gcc rust

# Debian / Ubuntu
sudo apt install mingw-w64
```

```sh
make smd            # 五月雨用 build/ddraw.dll
make smd-debug      # 五月雨デバッグ版 build/debug/ddraw.dll (SMD_DEBUG_BUILD, ログ有効)
make refrain        # RefRain用 build/d3d9.dll
make refrain-debug  # RefRainデバッグ版 build/debug/d3d9.dll (REFRAIN_DEBUG_BUILD, ログ有効)
make debug          # すべてのデバッグ版 (refrain-debug + smd-debug)
make all            # refrain + smd
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

- Wine/Proton環境で発生するリプレイのずれを修正

リプレイ中の弾や自機の座標を、強制的に24bit浮動小数点演算で処理します。

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
  - **Qキー**：ポーズ画面で押すと、そのキャラクターのデスクトップ画面に戻ります。
  - **Rキー**：ポーズ画面で押すと、リトライします。

- プラクティス

ステージ、残機、M.E.F.A.2ゲージ残量、コンセプトリアクターのストック数とゲージ量を調整できます。ステージ終了後は、そのキャラクターのデスクトップ画面に戻ります。プレイ中にF1〜F5キーを押すと、各ステージへすぐに移動できます（スコアやプリズムなどはリセットされます）。

![ステージセレクト](https://madotsukanai.github.io/raindrop.png)

## 注意

- 本ソフトウェアはRebRank様とは一切関係のない非公式ツールです。**本ソフトウェアについてRebRank様や関係者へ問い合わせないでください。**
- 非公式ツールを使用する前に、セーブデータをバックアップしてください。
- **本ツールで作成したRefRainのプラクティスリプレイは、公式スコアボードに投稿できません。**
