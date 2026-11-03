# nemu-link用のruntime Tap Dance

このmoduleはnemu-link用のruntime Tap Dance slotを提供します。1つのslotでtap、hold、double tap、tap-then-holdを設定できます。

本moduleはnemu-link用のZMK構成を前提とします。

## 導入（最小構成）

### west.yml

この例は既存の`west.yml`へ追記します。`sekigon-gonnoc` remoteが既にある場合は重ねて定義しません。nemu-link構成では、互換性のあるZMK forkと`zmk-feature-nemu-link`もmanifestに必要です。

```yaml
manifest:
  remotes:
    - name: sekigon-gonnoc
      url-base: https://github.com/sekigon-gonnoc
  projects:
    - name: zmk-feature-dynamic-settings
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
    - name: zmk-feature-dynamic-tap-dance
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
```

revisionには利用するZMK / nemu-link moduleと互換性のあるtagまたはcommitを指定し、manifest更新後に`west update`を実行します。

### .conf

```conf
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_STUDIO_RPC=y
CONFIG_ZMK_DYNAMIC_SETTINGS=y
CONFIG_SETTINGS_NVS=y
```

`CONFIG_SETTINGS_NVS`の代わりに、対象boardで利用するSettings backendを有効にできます。 behavior nodeがあると`CONFIG_ZMK_DYNAMIC_TAP_DANCE`が有効になります。

### overlay

利用するbehaviorごとに一意の`slot-index`を指定します。node labelはkeymapから参照します。

```dts
/ {
    runtime_tap_dance: runtime_tap_dance {
        compatible = "sekigon,dynamic-behavior-tap-dance";
        #binding-cells = <0>;
        slot-index = <0>;
        target-role = "keyboard-right";
    };
};
```

keymapのbindingに`&runtime_tap_dance`を指定します。

## 追加設定

`CONFIG_ZMK_DYNAMIC_TAP_DANCE_MAX_SLOTS`は生成slot数の上限（既定値4、範囲1〜32）、`CONFIG_ZMK_DYNAMIC_TAP_DANCE_MAX_ACTIVE`は同時に処理するTap Dance数（既定値2、範囲1〜4）です。

初期値は`default-tap`、`default-hold`、`default-double-tap`、`default-tap-hold`で指定します。どれかの出力を指定する場合は、必要に応じて`default-term-ms`を設定できます（既定値200 ms）。出力bindingがないslotは無効状態で開始します。

recordは45 byteで、有効flag、term、出力mask、最大4つのBehavior identityと各2つのparameterを保持します。Behavior bindingは最初のpressより前に解決し、activeなbindingはreleaseまで維持します。activeなdanceがあるslotへのSettings更新は`-EBUSY`を返します。

標準の`zmk,behavior-tap-dance` nodeとこのruntime Tap Danceは同じfirmwareで併用できません。両方のstate machineが同じposition interruptを処理しないよう、build時に拒否します。
