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
