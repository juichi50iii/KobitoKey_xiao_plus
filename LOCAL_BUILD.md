# Dockerでローカルビルド

Docker Desktopを起動した状態で、リポジトリのルートから実行します。

```sh
./scripts/local-build.sh right
```

指定できる対象は次の4つです。

- `right`: 右手
- `left`: 左手
- `reset`: 設定リセット
- `all`: 3種類すべて

生成されたUF2は、Codex作業フォルダの
`outputs/local-docker-build/`へ保存されます。

初回だけ公式ZMKビルドイメージと依存リポジトリを取得するため、時間がかかります。
2回目以降はDockerボリューム`kobitokey-xiao-plus-zmk-workspace`のキャッシュを再利用します。

## 配布・書き込み

小人キーは市販状態のSeeed XIAO nRF52840 Plus（純正UF2ブートローダー）を前提にします。
カスタムブートローダーは不要です。新品のXIAOでは、開いた状態でリセットを2回押して
`XIAO-BOOT`を表示し、生成された`KobitoKey_right.uf2`または`KobitoKey_left.uf2`を
コピーしてください。

右側の折り畳み・USB通知、振動、バッテリー残量表示はZMKファーム内で完結します。

依存モジュールをゼロから検証する場合は、別の空ボリューム名を指定できます。

```sh
ZMK_WORKSPACE_VOLUME=kobitokey-clean-test ./scripts/local-build.sh all
```
